#pragma once

#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/gpio.h>
#include <thread>
#include <chrono>
#include <cstring>

namespace SmartDehum {

class GpioActuator {
public:
    // RPi 5 上的 40-pin GPIO 均由 /dev/gpiochip4 (RP1晶片) 管理
    explicit GpioActuator(const std::string& chip_path = "/dev/gpiochip4",
                          int light_pin = 23,
                          int servo_pin = 18)
        : chip_path_(chip_path), light_pin_(light_pin), servo_pin_(servo_pin),
          chip_fd_(-1), light_req_fd_(-1), servo_req_fd_(-1) {}

    ~GpioActuator() {
        close_all();
    }

    bool init() {
        chip_fd_ = open(chip_path_.c_str(), O_RDWR);
        if (chip_fd_ < 0) {
            std::cerr << "[GPIO] 無法打開 " << chip_path_ << "，嘗試 /dev/gpiochip0..." << std::endl;
            chip_fd_ = open("/dev/gpiochip0", O_RDWR);
            if (chip_fd_ < 0) {
                std::cerr << "[GPIO] 無法打開任何 gpiochip" << std::endl;
                return false;
            }
        }

        // 1. 申請光敏 GPIO 輸入 (Input)
        struct gpio_v2_line_request req_in;
        std::memset(&req_in, 0, sizeof(req_in));
        req_in.offsets[0] = light_pin_;
        req_in.num_lines = 1;
        req_in.config.flags = GPIO_V2_LINE_FLAG_INPUT;
        std::strncpy(req_in.consumer, "smart_dehum_light", sizeof(req_in.consumer) - 1);

        if (ioctl(chip_fd_, GPIO_V2_GET_LINE_IOCTL, &req_in) < 0) {
            std::cerr << "[GPIO] 無法配置 Light Pin (" << light_pin_ << ")" << std::endl;
            return false;
        }
        light_req_fd_ = req_in.fd;

        // 2. 申請舵機 GPIO 輸出 (Output, 預設低電平)
        struct gpio_v2_line_request req_out;
        std::memset(&req_out, 0, sizeof(req_out));
        req_out.offsets[0] = servo_pin_;
        req_out.num_lines = 1;
        req_out.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
        std::strncpy(req_out.consumer, "smart_dehum_servo", sizeof(req_out.consumer) - 1);

        if (ioctl(chip_fd_, GPIO_V2_GET_LINE_IOCTL, &req_out) < 0) {
            std::cerr << "[GPIO] 無法配置 Servo Pin (" << servo_pin_ << ")" << std::endl;
            return false;
        }
        servo_req_fd_ = req_out.fd;

        return true;
    }

    // 讀取單次光敏腳位電平 (0 或 1)
    int read_light_raw() {
        if (light_req_fd_ < 0) return -1;
        struct gpio_v2_line_values vals;
        std::memset(&vals, 0, sizeof(vals));
        vals.mask = 1;
        if (ioctl(light_req_fd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
            return -1;
        }
        return (vals.bits & 1);
    }

    // 帶濾波的光敏開機狀態檢測 (20 次採樣, 間隔 12ms，抗 60Hz 閃爍)
    bool is_dehumidifier_on(int samples = 20, int delay_ms = 12) {
        int low_count = 0;
        for (int i = 0; i < samples; ++i) {
            if (read_light_raw() == 0) {
                low_count++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        return low_count >= 4;
    }

    // 發送特定脈寬 (微秒 us) 持續一段時間 (毫秒 duration_ms)
    void send_pwm_pulses(int pulse_us, int duration_ms) {
        if (servo_req_fd_ < 0) return;

        int total_pulses = duration_ms / 20; // 50Hz -> 週期 20ms
        struct gpio_v2_line_values val_high, val_low;
        std::memset(&val_high, 0, sizeof(val_high));
        std::memset(&val_low, 0, sizeof(val_low));

        val_high.mask = 1; val_high.bits = 1;
        val_low.mask = 1;  val_low.bits = 0;

        for (int i = 0; i < total_pulses; ++i) {
            // HIGH
            ioctl(servo_req_fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &val_high);
            std::this_thread::sleep_for(std::chrono::microseconds(pulse_us));

            // LOW
            ioctl(servo_req_fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &val_low);
            std::this_thread::sleep_for(std::chrono::microseconds(20000 - pulse_us));
        }
    }

    // 角度映射至 SG90 脈寬: 0° -> 500us, 180° -> 2500us
    int angle_to_pulse_us(double angle) {
        angle = std::max(0.0, std::min(180.0, angle));
        return static_cast<int>(500.0 + (angle / 180.0) * 2000.0);
    }

    // 觸發電源按鍵: 0° (待機) -> 27° (下壓 0.4s) -> 0° (回彈 0.4s) -> detach
    void trigger_power_button(double idle_angle = 0.0, double press_angle = 27.0) {
        int press_us = angle_to_pulse_us(press_angle);
        int idle_us  = angle_to_pulse_us(idle_angle);

        std::cout << "  ⚙️ [致動器] 下壓至 " << press_angle << "°..." << std::endl;
        send_pwm_pulses(press_us, 400); // 下壓維持 400ms

        std::cout << "  ⚙️ [致動器] 回彈至 " << idle_angle << "°..." << std::endl;
        send_pwm_pulses(idle_us, 400);  // 回彈維持 400ms

        // Detach (停止脈衝，完全釋放馬達)
        struct gpio_v2_line_values val_low;
        std::memset(&val_low, 0, sizeof(val_low));
        val_low.mask = 1; val_low.bits = 0;
        ioctl(servo_req_fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &val_low);
        std::cout << "  ⚙️ [致動器] 舵機已 Detach 放鬆。" << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 等待燈號穩定
    }

private:
    std::string chip_path_;
    int light_pin_;
    int servo_pin_;
    int chip_fd_;
    int light_req_fd_;
    int servo_req_fd_;

    void close_all() {
        if (light_req_fd_ >= 0) close(light_req_fd_);
        if (servo_req_fd_ >= 0) close(servo_req_fd_);
        if (chip_fd_ >= 0) close(chip_fd_);
        light_req_fd_ = -1;
        servo_req_fd_ = -1;
        chip_fd_ = -1;
    }
};

} // namespace SmartDehum
