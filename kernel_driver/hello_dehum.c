#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yen-Chung Hung");
MODULE_DESCRIPTION("Smart Dehumidifier Skeleton Driver");
MODULE_VERSION("1.0");

static int __init dehum_driver_init(void) {
    pr_info("SmartDehum: [KERNEL] 模組載入成功！核心版本匹配正常。\n");
    return 0;
}

static void __exit dehum_driver_exit(void) {
    pr_info("SmartDehum: [KERNEL] 模組已自核心卸載。\n");
}

module_init(dehum_driver_init);
module_exit(dehum_driver_exit);
