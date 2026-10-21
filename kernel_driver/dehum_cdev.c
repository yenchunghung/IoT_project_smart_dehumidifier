#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/i2c.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/version.h>
#include <linux/hwmon.h>

#include "dehum_event.h"

#define DEVICE_NAME "dehum_actuator"
#define CLASS_NAME  "dehum"

#define PULSE_STANDBY_US 600
#define PULSE_PRESS_US 900

// 採樣間隔：1 秒 (可動態調整)
#define SAMPLE_INTERVAL_MS 1000

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yen-Chung Hung");
MODULE_DESCRIPTION("Smart Dehumidifier Event-Driven Platform Device Driver");
MODULE_VERSION("3.0");

static int major_number;
static struct class*  dehum_class  = NULL;
static struct device* dehum_device = NULL;

static struct gpio_desc *desc_sensor = NULL;
static struct gpio_desc *desc_servo  = NULL;

/* ---- 事件驅動與緩衝區機制 ---- */
static DECLARE_WAIT_QUEUE_HEAD(dehum_wait_queue);
static struct dehum_event_packet current_packet;
static bool data_ready = false;
static DEFINE_MUTEX(data_lock);

static struct hrtimer sample_timer;
static struct work_struct sample_work;
static struct i2c_client *sht30_client = NULL;

// 輔助函式：從 sysfs 節點直接安全讀取數值
static int read_sysfs_int(const char *path, long *val) {
    struct file *filp;
    char buf[32] = {0};
    ssize_t ret;
    loff_t pos = 0;

    filp = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(filp)) return PTR_ERR(filp);

    ret = kernel_read(filp, buf, sizeof(buf) - 1, &pos);
    filp_close(filp, NULL);

    if (ret <= 0) return -EIO;

    buf[ret] = '\0';
    return kstrtol(strim(buf), 10, val);
}

static int find_sht3x_hwmon_idx(void) {
    int i;
    char path[64];
    struct file *filp;
    char name_buf[16];
    loff_t pos;
    ssize_t ret;

    for (i = 0; i < 10; i++) {
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", i);
        filp = filp_open(path, O_RDONLY, 0);
        if (!IS_ERR(filp)) {
            memset(name_buf, 0, sizeof(name_buf));
            pos = 0;
            ret = kernel_read(filp, name_buf, sizeof(name_buf) - 1, &pos);
            filp_close(filp, NULL);

            if (ret > 0) {
                if (strncmp(strim(name_buf), "sht3x", 5) == 0) {
                    return i; // 找到對應的 hwmon 編號
                }
            }
        }
    }
    return -ENODEV;
}

/* ---- Bottom-Half: 核心工作執行緒 ---- */
static void sample_work_func(struct work_struct *work) {
    long t_mc = 0;
    long h_mp = 0;
    static int cached_hwmon_idx = -1;
    char path[64];
    int TOTAL_SAMPLES = 25;
    int THRESHOLD = 4;
    int on_count = 0;
    u8 debounced_light = 0;

    // 若尚未鎖定節點編號，動態搜尋一次
    if (cached_hwmon_idx < 0) {
        cached_hwmon_idx = find_sht3x_hwmon_idx();
    }

    // 透過動態編號讀取溫濕度
    if (cached_hwmon_idx >= 0) {
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp1_input", cached_hwmon_idx);
        if (read_sysfs_int(path, &t_mc) != 0) {
            cached_hwmon_idx = -1; // 節點失效時重置快取
        }

        if (cached_hwmon_idx >= 0) {
            snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/humidity1_input", cached_hwmon_idx);
            read_sysfs_int(path, &h_mp);
        }
    }

    // 讀取光敏感測器
    if (desc_sensor) {
        for (int i = 0; i < TOTAL_SAMPLES; ++i) {
            if (gpiod_get_value(desc_sensor) == 0) {
                on_count++;
            }
            if (i < TOTAL_SAMPLES - 1) {
                msleep(12);
            }
        }
        debounced_light = (on_count >= THRESHOLD) ? 1 : 0;
    }

    // 封裝封包並寫入緩衝區
    mutex_lock(&data_lock);
    current_packet.timestamp_ns = ktime_get_real_ns();
    current_packet.temperature_mc = (s32)t_mc;
    current_packet.humidity_mp = (u32)h_mp;
    current_packet.light_state = debounced_light;
    data_ready = true;
    mutex_unlock(&data_lock);

    // 喚醒在 poll/epoll 上等待的 User Space 行程
    wake_up_interruptible(&dehum_wait_queue);
}

/* ---- Top-Half: 高精度定時器中斷 ---- */
static enum hrtimer_restart sample_timer_callback(struct hrtimer *timer) {
    // 調度工作佇列以安全執行 I2C 通訊
    schedule_work(&sample_work);

    // 設置下一次定時週期 (1 秒)
    hrtimer_forward_now(timer, ms_to_ktime(SAMPLE_INTERVAL_MS));
    return HRTIMER_RESTART;
}

/* ---- 舵機控制 ---- */
static void servo_send_pulse(int high_us, int count) {
    int i;
    unsigned long flags;
    if (!desc_servo) return;

    for (i = 0; i < count; i++) {
        local_irq_save(flags);
        gpiod_set_value(desc_servo, 1);
        udelay(high_us);
        gpiod_set_value(desc_servo, 0);
        local_irq_restore(flags);

        usleep_range(18000, 19000);
    }
    gpiod_set_value(desc_servo, 0);
}

static void servo_trigger_press(void) {
    pr_info("SmartDehum: [KERNEL] 執行舵機下壓 (900 us)...\n");
    servo_send_pulse(PULSE_PRESS_US, 20);

    msleep(150);

    pr_info("SmartDehum: [KERNEL] 舵機自動復位 (600 us)...\n");
    servo_send_pulse(PULSE_STANDBY_US, 25);
}

/* ---- File Operations: 支援二進位讀取與 poll ---- */
static __poll_t dev_poll(struct file *filep, poll_table *wait) {
    __poll_t mask = 0;

    poll_wait(filep, &dehum_wait_queue, wait);

    mutex_lock(&data_lock);
    if (data_ready) {
        mask |= (EPOLLIN | EPOLLRDNORM); // 通知資料已準備好
    }
    mutex_unlock(&data_lock);

    return mask;
}

static ssize_t dev_read(struct file *filep, char __user *buffer, size_t len, loff_t *offset) {
    int ret = 0;

    // 若要求讀取大小為二進位封包大小，走事件推播二進位模式
    if (len >= sizeof(struct dehum_event_packet)) {
        // 如果是無阻塞模式且沒資料，立即返回
        if ((filep->f_flags & O_NONBLOCK) && !data_ready) {
            return -EAGAIN;
        }

        // 阻塞等待新數據產生
        ret = wait_event_interruptible(dehum_wait_queue, data_ready);
        if (ret) return -ERESTARTSYS;

        mutex_lock(&data_lock);
        if (copy_to_user(buffer, &current_packet, sizeof(struct dehum_event_packet))) {
            mutex_unlock(&data_lock);
            return -EFAULT;
        }
        data_ready = false; // 取走後重設標誌
        mutex_unlock(&data_lock);

        return sizeof(struct dehum_event_packet);
    } else {
        // 向下相容：傳統 cat /dev/dehum_actuator 純文字讀取光敏
        char status_str[16];
        int val = 0;
        size_t str_len;

        if (*offset > 0) return 0;
        if (desc_sensor) val = gpiod_get_value(desc_sensor);

        str_len = snprintf(status_str, sizeof(status_str), "%d\n", val);
        if (copy_to_user(buffer, status_str, str_len)) return -EFAULT;

        *offset += str_len;
        return str_len;
    }
}

static ssize_t dev_write(struct file *filep, const char __user *buffer, size_t len, loff_t *offset) {
    char k_buf[32] = {0};
    int pulse_val = 0;
    size_t copy_len = len < sizeof(k_buf) - 1 ? len : sizeof(k_buf) - 1;

    if (copy_from_user(k_buf, buffer, copy_len)) {
        return -EFAULT;
    }

    if (copy_len > 0 && k_buf[copy_len - 1] == '\n') {
        k_buf[copy_len - 1] = '\0';
    }

    // 若輸入數字 (例如 1000 ~ 2000)，直接驅動該脈衝
    if (kstrtoint(k_buf, 10, &pulse_val) == 0 && pulse_val >= 500 && pulse_val <= 2500) {
        pr_info("SmartDehum: [KERNEL] 直接測試脈衝: %d us\n", pulse_val);
        servo_send_pulse(pulse_val, 25);
    } else if (strcmp(k_buf, "PRESS") == 0) {
        servo_trigger_press();
    }

    return len;
}

static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .open    = simple_open,
    .read    = dev_read,
    .write   = dev_write,
    .poll    = dev_poll,
};

static int dehum_probe(struct platform_device *pdev) {
    struct device *dev = &pdev->dev;

    pr_info("SmartDehum: [KERNEL] 發現相容 Device Tree 節點，啟動 probe...\n");

    // 從 Device Tree 取得 GPIO descriptors
    // 核心會自動尋找 DTS 節點中的 sensor-gpios 與 servo-gpios
    desc_sensor = devm_gpiod_get(dev, "sensor", GPIOD_IN);
    if (IS_ERR(desc_sensor)) {
        dev_err(dev, "無法從 Device Tree 取得 sensor GPIO descriptor\n");
        return PTR_ERR(desc_sensor);
    }
    pr_info("SmartDehum: [KERNEL] 感測器 (sensor-gpios) 透過 DT 綁定成功！\n");

    desc_servo = devm_gpiod_get(dev, "servo", GPIOD_OUT_LOW);
    if (IS_ERR(desc_servo)) {
        dev_err(dev, "無法從 Device Tree 取得 servo GPIO descriptor\n");
        return PTR_ERR(desc_servo);
    }
    pr_info("SmartDehum: [KERNEL] 舵機 (servo-gpios) 透過 DT 綁定成功！\n");

    // 註冊字元設備
    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) {
        pr_err("SmartDehum: register_chrdev 失敗: %d\n", major_number);
        return major_number;
    }

    dehum_class = class_create(CLASS_NAME);
    if (IS_ERR(dehum_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(dehum_class);
    }

    dehum_device = device_create(dehum_class, dev, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(dehum_device)) {
        class_destroy(dehum_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(dehum_device);
    }

    // 載入模組時自動確保舵機處於待機抬起狀態
    servo_send_pulse(PULSE_STANDBY_US, 20);

    // 初始化 Workqueue 與高精度定時器
    INIT_WORK(&sample_work, sample_work_func);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
    hrtimer_setup(&sample_timer, sample_timer_callback, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
    hrtimer_init(&sample_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    sample_timer.function = sample_timer_callback;
#endif
    hrtimer_start(&sample_timer, ms_to_ktime(SAMPLE_INTERVAL_MS), HRTIMER_MODE_REL);

    pr_info("SmartDehum: [KERNEL] 事件驅動引擎啟動成功！(週期: %d ms)\n", SAMPLE_INTERVAL_MS);
    return 0;
}

/* ---- Platform Driver Remove (裝置卸載時呼叫) ---- */
static void dehum_remove(struct platform_device *pdev) {
    // 停止定時器並等待工作執行緒排空
    hrtimer_cancel(&sample_timer);
    cancel_work_sync(&sample_work);

    if (sht30_client) {
        i2c_unregister_device(sht30_client);
    }

    if (desc_servo) {
        gpiod_set_value(desc_servo, 0);
    }

    device_destroy(dehum_class, MKDEV(major_number, 0));
    class_destroy(dehum_class);
    unregister_chrdev(major_number, DEVICE_NAME);

    pr_info("SmartDehum: [KERNEL] 模組安全卸載完成。\n");
}

static const struct of_device_id dehum_dt_ids[] = {
    { .compatible = "myorg,dehum-actuator" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, dehum_dt_ids);

static struct platform_driver dehum_platform_driver = {
    .probe = dehum_probe,
    .remove = dehum_remove,
    .driver = {
        .name = "dehum_actuator_driver",
        .of_match_table = dehum_dt_ids,
    },
};

module_platform_driver(dehum_platform_driver);
