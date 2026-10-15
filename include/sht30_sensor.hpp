#pragma once

#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <cstdint>
#include <thread>
#include <chrono>
#include <stdexcept>

namespace SmartDehum {

struct SensorReading {
    double temperature_c;
    double relative_humidity;
};

class Sht30Sensor {
public:
    explicit Sht30Sensor(const std::string& i2c_device = "/dev/i2c-1", uint8_t addr = 0x44)
        : device_path_(i2c_device), address_(addr), fd_(-1) {}

    ~Sht30Sensor() {
        close_device();
    }

    bool init() {
        fd_ = open(device_path_.c_str(), O_RDWR);
        if (fd_ < 0) {
            std::cerr << "[SHT30] 無法打開 I2C 設備: " << device_path_ << std::endl;
            return false;
        }

        if (ioctl(fd_, I2C_SLAVE, address_) < 0) {
            std::cerr << "[SHT30] 無法設定 I2C 從端設備位址: 0x" 
                      << std::hex << static_cast<int>(address_) << std::dec << std::endl;
            close_device();
            return false;
        }
        return true;
    }

    SensorReading read_measurement() {
        if (fd_ < 0 && !init()) {
            throw std::runtime_error("[SHT30] 設備未初始化或連線失敗");
        }

        // 1. 發送量測命令: 0x2C06 (高精度單次量測)
        uint8_t cmd[2] = {0x2C, 0x06};
        if (write(fd_, cmd, 2) != 2) {
            throw std::runtime_error("[SHT30] 寫入量測指令失敗");
        }

        // 2. 等待感測器內部 ADC 採樣完成 (約 20ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 3. 讀回 6 個位元組
        uint8_t data[6] = {0};
        if (read(fd_, data, 6) != 6) {
            throw std::runtime_error("[SHT30] 讀取感測數據位元組失敗");
        }

        // 4. 校驗 CRC8 (可選，確保雜訊環境下的資料正確性)
        if (!check_crc(data, 2, data[2]) || !check_crc(data + 3, 2, data[5])) {
            std::cerr << "[SHT30] 警告: CRC8 檢查未通過，忽略本次數值" << std::endl;
        }

        // 5. 轉換原始數據為工程單位
        uint16_t raw_temp = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        uint16_t raw_hum  = (static_cast<uint16_t>(data[3]) << 8) | data[4];

        double temp_c = -45.0 + (175.0 * static_cast<double>(raw_temp) / 65535.0);
        double rh_pct = 100.0 * static_cast<double>(raw_hum) / 65535.0;

        return {temp_c, rh_pct};
    }

private:
    std::string device_path_;
    uint8_t address_;
    int fd_;

    void close_device() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    // SHT30 標準 CRC8 演算法 (多項式: 0x31, 初始值: 0xFF)
    static bool check_crc(const uint8_t* data, size_t length, uint8_t checksum) {
        uint8_t crc = 0xFF;
        for (size_t i = 0; i < length; ++i) {
            crc ^= data[i];
            for (int bit = 8; bit > 0; --bit) {
                if (crc & 0x80) {
                    crc = (crc << 1) ^ 0x31;
                } else {
                    crc = (crc << 1);
                }
            }
        }
        return crc == checksum;
    }
};

} // namespace SmartDehum
