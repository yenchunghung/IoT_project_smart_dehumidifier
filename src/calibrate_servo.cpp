#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include "gpio_actuator.hpp"

int main(int argc, char* argv[]) {
    double test_angle = 15.0; // 預設測試角度
    int total_trials = 20;

    if (argc >= 2) {
        test_angle = std::stod(argv[1]);
    }
    if (argc >= 3) {
        total_trials = std::stoi(argv[2]);
    }

    std::cout << "==================================================" << std::endl;
    std::cout << "   SG90 舵機行程與開關可靠度自動化測試工具        " << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "▶ 目標測試下壓角度: " << test_angle << "°" << std::endl;
    std::cout << "▶ 測試次數: " << total_trials << " 次" << std::endl;
    std::cout << "▶ 判定機制: GPIO 23 光敏電阻自動比對按壓前後燈號變化" << std::endl;
    std::cout << "--------------------------------------------------" << std::endl;

    SmartDehum::GpioActuator actuator("/dev/gpiochip4", 23, 18);
    if (!actuator.init()) {
        std::cerr << "❌ GPIO 初始化失敗！" << std::endl;
        return 1;
    }

    int success_count = 0;

    for (int trial = 1; trial <= total_trials; ++trial) {
        // 1. 記錄按壓前的除濕機狀態 (ON 或 OFF)
        bool initial_state = actuator.is_dehumidifier_on();

        std::cout << "[" << std::setw(2) << trial << "/" << total_trials << "] "
                  << "初始狀態: " << (initial_state ? "🟢 ON " : "⚪ OFF") 
                  << " -> 執行下壓至 " << test_angle << "° ... " << std::flush;

        // 2. 觸發按壓 (待機 0° -> test_angle° 下壓 400ms -> 回彈 0° -> detach)
        actuator.trigger_power_button(0.0, test_angle);

        // 3. 等待除濕機控制板響應 (3 秒確保指示燈點亮/熄滅)
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));

        // 4. 檢查按壓後的除濕機狀態
        bool new_state = actuator.is_dehumidifier_on();

        // 5. 判定是否成功切換狀態
        if (new_state != initial_state) {
            success_count++;
            std::cout << "✅ 成功切換為 " << (new_state ? "🟢 ON" : "⚪ OFF") << std::endl;
        } else {
            std::cout << "❌ 失敗 (狀態未改變，行程可能過淺或按鍵未觸發)" << std::endl;
        }

        // 6. 每次測試之間稍作冷卻休息，保護除濕機內部微動開關
        std::this_thread::sleep_for(std::chrono::milliseconds(5000));
    }

    std::cout << "==================================================" << std::endl;
    double success_rate = (static_cast<double>(success_count) / total_trials) * 100.0;
    std::cout << "🎯 測試結果統計 (" << test_angle << "°):" << std::endl;
    std::cout << "   成功: " << success_count << " / " << total_trials 
              << " (" << std::fixed << std::setprecision(1) << success_rate << "%)" << std::endl;

    if (success_count == total_trials) {
        std::cout << "🎉 達成 100% 成功率！此角度為安全且有效的按壓設定。" << std::endl;
    } else {
        std::cout << "⚠️ 未達 100%，建議每次增加 2°~3° 重新測試。" << std::endl;
    }
    std::cout << "==================================================" << std::endl;

    return 0;
}
