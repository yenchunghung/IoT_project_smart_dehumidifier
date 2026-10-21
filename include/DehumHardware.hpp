#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <string>
#include <iostream>
#include <stdexcept>

#include "dehum_event.h"

namespace SmartDehum {

struct SensorReading{
    double temperature_c = 0.0;
    double relative_humidity = 0.0;
};

}

class DehumHardware {
private:
    const std::string dev_path;
    int dev_fd = -1;
    int epoll_fd = -1;

    bool cached_light_state = false;
    double cached_temp = 0.0;
    double cached_hum = 0.0;
    uint64_t cached_timestamp_ns = 0;

public:
    DehumHardware(const std::string& actuator_path = "/dev/dehum_actuator")
        : dev_path(actuator_path) {

        // 1. 開啟核心字元裝置 (非阻塞模式，交由 epoll 管理)
        dev_fd = open(dev_path.c_str(), O_RDWR | O_NONBLOCK);
        if (dev_fd < 0) {
            throw std::runtime_error("❌ [硬體錯誤] 無法開啟 " + dev_path);
        }

        // 2. 建立 epoll 實例
        epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            close(dev_fd);
            throw std::runtime_error("❌ [系統錯誤] epoll_create1 失敗");
        }

        // 3. 註冊監聽可讀事件 (EPOLLIN)
        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = dev_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, dev_fd, &ev) < 0) {
            close(epoll_fd);
            close(dev_fd);
            throw std::runtime_error("❌ [系統錯誤] epoll_ctl 註冊事件失敗");
        }

        std::cout << "✅ [硬體抽象層] 模式 B 事件驅動引擎就緒 (epoll 監聽: " << dev_path << ")" << std::endl;
    }

    ~DehumHardware() {
        if (epoll_fd >= 0) close(epoll_fd);
        if (dev_fd >= 0) close(dev_fd);
    }

    // 等待核心推送新事件 (以 timeout_ms 阻塞等待，0% CPU 消耗)
    bool waitForEvent(int timeout_ms = 2500) {
        struct epoll_event events[1];
        int nfds = epoll_wait(epoll_fd, events, 1, timeout_ms);
        if (nfds <= 0) {
            return false; // 超時或收到訊號
        }

        if (events[0].data.fd == dev_fd && (events[0].events & EPOLLIN)) {
            struct dehum_event_packet packet{};
            ssize_t bytes = read(dev_fd, &packet, sizeof(packet));
            if (bytes == sizeof(packet)) {
                cached_timestamp_ns = packet.timestamp_ns;
                cached_temp = static_cast<double>(packet.temperature_mc) / 1000.0;
                cached_hum = static_cast<double>(packet.humidity_mp) / 1000.0;
                cached_light_state = (packet.light_state != 0);
                return true;
            }
        }
        return false;
    }

    // 讀取除濕機指示燈：true = 運轉中 (燈亮), false = 關閉 (燈熄)
    bool isLightOn() const {
        return cached_light_state;
    }

    // 觸發下壓 27° (900us) 並自動復位 (600us)
    bool triggerPress() {
        if (dev_fd < 0) return false;

        const char cmd[] = "PRESS\n";
        ssize_t bytes = write(dev_fd, cmd, sizeof(cmd) - 1);

        return bytes > 0;
    }

    // 讀取溫溼度環境數值
    bool getEnvironment(double& temperature, double& humidity) const {
        temperature = cached_temp;
        humidity = cached_hum;
        return true;
    }

    // 支援直接讀入 SmartDehum::SensorReading 物件
    bool getEnvironment(SmartDehum::SensorReading& reading) const {
        reading.temperature_c = cached_temp;
        reading.relative_humidity = cached_hum;
        return true;
    }

    uint64_t getTimestampNs() const {
        return cached_timestamp_ns;
    }
};
