#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <deque>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <ctime>
#include <vector>

#include "thermodynamics.hpp"
#include "ode_predictor.hpp"
#include "mlp_predictor.hpp"
#include "feature_builder.hpp"
#include "http_server.hpp"
#include "DehumHardware.hpp"

std::atomic<bool> g_running{true};
std::atomic<bool> g_manual_trigger{false};

void signal_handler(int) { 
    g_running = false; 
}

struct PendingSample {
    std::chrono::steady_clock::time_point timestamp;
    std::vector<double> features;
    double ode_base;
};

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "==========================================" << std::endl;
    std::cout << "  Smart Dehumidifier v3.0 (Event-Driven)  " << std::endl;
    std::cout << "==========================================" << std::endl;

    // 啟動內建純 C++ HTTP Server (Port 8080)
    SmartDehum::SimpleHttpServer http_server(8080, []() {
        g_manual_trigger = true;
    });

    if (!http_server.start()) {
        std::cerr << "❌ [Internet error] Unable to activate in-build HTTP service in Port 8080" << std::endl;
        return 1;
    }
    std::cout << "🌐 [In-build internet] HTTP API service is ready on Port 8080" << std::endl;

    // 使用 Linux 核心字元裝置驅動 (/dev/dehum_actuator)
    DehumHardware dehum;

    // 初始化核心演算法
    SmartDehum::OdePredictor ode_model;
    SmartDehum::MlpNetwork mlp_model(0.02, 0.9);

    std::deque<SmartDehum::FeatureBuilder::EnvironmentSample> history;
    std::deque<PendingSample> pending_queue;

    // 控制參數
    constexpr double TARGET_RH_HIGH = 65.0; // 開機上限 (%)
    constexpr double TARGET_RH_LOW  = 60.0; // 關機下限 (%)
    constexpr int DECISION_EVENT_COUNT = 5; // 核心每秒1次推播，每累積 5 次執行一次完整決策
    constexpr int PENDING_HORIZON_SEC = 300; // 5 分鐘學習延時隊列
    constexpr int MIN_RUN_TIME_SEC  = 60;  // 最小運轉保護 1 分鐘
    constexpr int MIN_REST_TIME_SEC = 60;  // 最小冷卻保護 1 分鐘

    auto last_state_change = std::chrono::steady_clock::now() - std::chrono::seconds(300);
    bool last_known_state = false;
    int failed_startup_count = 0;
    bool is_water_full = false;
    int event_counter = 0;

    // 非同步滿水檢測狀態旗標 (不使用 sleep_for)
    bool waiting_startup_verify = false;
    std::chrono::steady_clock::time_point startup_verify_deadline;

    std::cout << "[System activated] Initial State: " << (last_known_state ? "🟢 ON" : "⚪ OFF") << std::endl;
    std::cout << "[Event Loop] 進入非同步事件監聽迴圈 (0% CPU Idle)..." << std::endl;

    while (g_running) {
        // 核心阻塞等待：每秒被核心定時器喚醒一次 (0% CPU 消耗)
        if (!dehum.waitForEvent(2500)) {
            if (g_running) {
                std::cout << "⚠️ [Timeout] 等待核心推播超時 (>2.5s)，確認驅動狀態中..." << std::endl;
            }
            continue;
        }

        auto now = std::chrono::steady_clock::now();

        // 讀取核心推播後的感測數據
        SmartDehum::SensorReading reading;
        dehum.getEnvironment(reading);
        bool is_on = dehum.isLightOn();

        if (is_on != last_known_state) {
            last_known_state = is_on;
            last_state_change = now;
            if (is_on) {
                failed_startup_count = 0;
                is_water_full = false;
            }
        }

        // 非同步開機檢驗（延遲 2 秒後由核心事件喚醒判定，零 sleep 阻塞）
        if (waiting_startup_verify && now >= startup_verify_deadline) {
            waiting_startup_verify = false;
            if (!is_on) {
                failed_startup_count++;
                std::cout << "⚠️ [Warning] 開機後 2 秒未檢測到指示燈 (累計失敗 " 
                          << failed_startup_count << " 次)" << std::endl;
                if (failed_startup_count >= 3) {
                    is_water_full = true;
                    std::cout << "🚨 [Water Full] 連續 3 次開機無燈號反應，標記水箱已滿並鎖定自動開機！" << std::endl;
                }
            } else {
                failed_startup_count = 0;
                is_water_full = false;
                std::cout << "✅ [Startup Success] 成功檢測到指示燈亮起，除濕機已正常啟動。" << std::endl;
            }
        }

        double dew_point = SmartDehum::Thermodynamics::calculate_dew_point(reading.temperature_c, reading.relative_humidity);
        double abs_h = SmartDehum::Thermodynamics::calculate_absolute_humidity(reading.temperature_c, reading.relative_humidity);

        // 每累積 5 次事件（約 5 秒），執行一次 ODE/MLP 推論、在線訓練與控制決策
        event_counter++;
        std::string action = "NONE";
        std::string status_msg = "Maintain current state";
        double pred_abs_h = abs_h;
        double pred_rh = reading.relative_humidity;

        if (event_counter >= DECISION_EVENT_COUNT) {
            event_counter = 0;

            // 純物理 ODE 5 分鐘預測
            ode_model.update_ambient_reference(abs_h);
            double ode_5m = ode_model.predict(abs_h, is_on, 5.0);

            // MLP 殘差推論
            SmartDehum::FeatureBuilder::EnvironmentSample curr_sample{
                reading.temperature_c, reading.relative_humidity, abs_h, dew_point, is_on, ode_5m
            };
            auto feats = SmartDehum::FeatureBuilder::build_features(curr_sample, history);
            double residual = mlp_model.forward(feats);
            double pred_abs_h = ode_5m + residual;
            double pred_rh = SmartDehum::Thermodynamics::calculate_rh_from_absolute(reading.temperature_c, pred_abs_h);

            // 在線學習反向傳播更新
            while (!pending_queue.empty()) {
                auto& oldest = pending_queue.front();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - oldest.timestamp).count();
                if (elapsed >= PENDING_HORIZON_SEC) {
                    mlp_model.train_step(oldest.features, abs_h - oldest.ode_base);
                    pending_queue.pop_front();
                } else {
                    break;
                }
            }
            pending_queue.push_back({now, feats, ode_5m});
            history.push_back(curr_sample);
            if (history.size() > 20) history.pop_front();

            // 閉迴路控制與狀態機
            auto state_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_state_change).count();

            // Prioritize the manual trigger from the iOS monitor app
            if (g_manual_trigger.exchange(false)) {
                action = "MANUAL_TRIGGER";
                status_msg = "Manual trigger sent from iOS monitor app";
                std::cout << "\n>>> [Manual trigger] Sent from iOS, servo press 27.0° ! " << std::endl;
            
                dehum.triggerPress();
                last_state_change = now;
            
                // 若原為關機狀態，註冊非同步 2 秒驗證開機成功與滿水狀態
                if (!is_on) {
                    waiting_startup_verify = true;
                    startup_verify_deadline = now + std::chrono::seconds(2);
                }
            }
            else if (is_on) {
                if (reading.relative_humidity <= TARGET_RH_LOW || pred_rh <= (TARGET_RH_LOW - 1.0)) {
                    if (state_elapsed >= MIN_RUN_TIME_SEC) {
                        std::cout << "\n>>> [Auto Shutdown] Moderate humidity, shutting down automatically!" << std::endl;
                        dehum.triggerPress();
                        last_state_change = now;
                        last_known_state = false;
                        action = "AUTO_SHUTDOWN";
                        status_msg = "Moderate humidity, shutting down automatically!";
                    } else {
                        status_msg = "Moderate humidity, but operation protection remains " + std::to_string(MIN_RUN_TIME_SEC - state_elapsed) + "s";
                    }
                } else {
                    status_msg = "Dehumidifying (Predicted RH : " + std::to_string((int)pred_rh) + "%)";
                }
            } else {
                // If full water is possible, stop auto startup and warning
                if (is_water_full) {
                    status_msg = "Possibly full water! No light for 3 consecutive detection, check the reservoir";
                } else if (reading.relative_humidity >= TARGET_RH_HIGH) {
                    if (state_elapsed >= MIN_REST_TIME_SEC) {
                        std::cout << "\n>>> [Auto Startup] Too humid (" << reading.relative_humidity 
                                  << "%), starting up automatically!" << std::endl;
                        dehum.triggerPress();
                        last_state_change = now;
                        last_known_state = true;
                        action = "AUTO_STARTUP";
                        status_msg = "Too humid, starting up automatically!";

                        // 註冊非同步 2 秒開機檢驗
                        waiting_startup_verify = true;
                        startup_verify_deadline = now + std::chrono::seconds(2);
                    } else {
                        status_msg = "Too humid, but operation protection remains " + std::to_string(MIN_REST_TIME_SEC - state_elapsed) + "s";
                    }
                } else {
                    status_msg = "Standby (Moderate humidity)";
                }
            }

            // 控制台輸出週期日誌
            std::cout << "[Event Step] RH: " << std::fixed << std::setprecision(1) << reading.relative_humidity
                      << "% | Temp: " << reading.temperature_c << "°C"
                      << " | State: " << (is_on ? "🟢 ON" : "⚪ OFF")
                      << (is_water_full ? " ⚠️[Full Water Warning]" : "")
                      << " | Decision: " << status_msg << std::endl;

        }

        // 每次收到核心推播，都以最新資料即時更新 HTTP JSON（毫秒級反映給 iOS 監控端）
        std::ostringstream json_oss;
        json_oss << "{\n"
                 << "  \"temp\": " << std::fixed << std::setprecision(1) << reading.temperature_c << ",\n"
                 << "  \"rh\": " << reading.relative_humidity << ",\n"
                 << "  \"abs_h\": " << std::setprecision(2) << abs_h << ",\n"
                 << "  \"dew_point\": " << std::setprecision(1) << dew_point << ",\n"
                 << "  \"is_on\": " << (is_on ? "true" : "false") << ",\n"
                 << "  \"is_water_full\": " << (is_water_full ? "true" : "false") << ",\n"
                 << "  \"failed_startup_count\": " << failed_startup_count << ",\n"
                 << "  \"pred_abs_h_5m\": " << std::setprecision(2) << pred_abs_h << ",\n"
                 << "  \"pred_rh_5m\": " << std::setprecision(1) << pred_rh << ",\n"
                 << "  \"action_required\": \"" << action << "\",\n"
                 << "  \"reason\": \"" << status_msg << "\"\n"
                 << "}\n";

        http_server.update_status_json(json_oss.str());
    }

    std::cout << "\n🛑 正在停止 HTTP Server..." << std::endl;
    http_server.stop();
    return 0;
}
