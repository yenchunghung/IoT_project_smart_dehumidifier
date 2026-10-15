#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <thread>
#include <chrono>

class DehumHardware {
private:
    const std::string dev_path = "/dev/dehum_actuator";

    char readRawOnce() {
        int fd = open(dev_path.c_str(), O_RDONLY);
        if (fd < 0) return ' ';
        char buf[8] = {0};
        ssize_t bytes = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        return (bytes > 0) ? buf[0] : ' ';
    }

public:
    // 讀取除濕機指示燈：true = 運轉中 (燈亮), false = 關閉 (燈熄)
    bool isLightOn() {
        constexpr int TOTAL_SAMPLES = 20;
        constexpr int THRESHOLD = 4;
        int on_count = 0;

        for (int i = 0; i < TOTAL_SAMPLES; ++i) {
            char val = readRawOnce();

            if (val == '0') {
                on_count++;
            }

            if (i < TOTAL_SAMPLES - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        return (on_count >= THRESHOLD);
    }

    // 觸發下壓 27° (900us) 並自動復位 (600us)
    bool triggerPress() {
        int fd = open(dev_path.c_str(), O_WRONLY);
        if (fd < 0) return false;

        const char cmd[] = "PRESS\n";
        ssize_t bytes = write(fd, cmd, sizeof(cmd) - 1);
        close(fd);

        return bytes > 0;
    }
};
