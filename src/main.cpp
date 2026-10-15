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
#include "thermodynamics.hpp"
#include "ode_predictor.hpp"
#include "sht30_sensor.hpp"
#include "mlp_predictor.hpp"
#include "feature_builder.hpp"
#include "http_server.hpp"
#include "DehumHardware.hpp"

std::atomic<bool> g_running{true};
std::atomic<bool> g_manual_trigger{false};

void signal_handler(int) { g_running = false; }

struct PendingSample {
    std::chrono::steady_clock::time_point timestamp;
    std::vector<double> features;
    double ode_base;
};

int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "==========================================================" << std::endl;
    std::cout << "  Smart Dehumidifier System (Auto Mode: Calibrated 27.0°) " << std::endl;
    std::cout << "==========================================================" << std::endl;

    // 啟動內建純 C++ HTTP Server (Port 8080)
    SmartDehum::SimpleHttpServer http_server(8080, []() {
        g_manual_trigger = true;
    });

    if (!http_server.start()) {
        std::cerr << "❌ [Internet error] Unable to activate in-build HTTP service in Port 8080" << std::endl;
        return 1;
    }
    std::cout << "🌐 [In-build internet] HTTP API service is ready in Port 8080" << std::endl;

    // 1. 初始化硬體
    SmartDehum::Sht30Sensor sensor("/dev/i2c-1", 0x44);
    if (!sensor.init()) {
        std::cerr << "❌ [Hardware error] Unable to initialize SHT30 I2C sensor" << std::endl;
        return 1;
    }

    // 使用 Linux 核心字元裝置驅動 (/dev/dehum_actuator)
    DehumHardware dehum;

    // 2. 初始化核心演算法
    SmartDehum::OdePredictor ode_model;
    SmartDehum::MlpNetwork mlp_model(0.02, 0.9);

    std::deque<SmartDehum::FeatureBuilder::EnvironmentSample> history;
    std::deque<PendingSample> pending_queue;

    // 控制參數
    constexpr double TARGET_RH_HIGH = 65.0; // 開機上限 (%)
    constexpr double TARGET_RH_LOW  = 60.0; // 關機下限 (%)
    constexpr int LOOP_INTERVAL_SEC = 5;    // 採樣週期 5 秒
    constexpr int PENDING_HORIZON_SEC = 300; // 5 分鐘學習延時隊列
    constexpr int MIN_RUN_TIME_SEC  = 60;  // 最小運轉保護 1 分鐘
    constexpr int MIN_REST_TIME_SEC = 60;  // 最小冷卻保護 1 分鐘

    // 校準後的最佳安全行程角度
    constexpr double SERVO_IDLE_ANGLE  = 0.0;
    constexpr double SERVO_PRESS_ANGLE = 27.0;

    auto last_state_change = std::chrono::steady_clock::now() - std::chrono::seconds(300);
    bool last_known_state = dehum.isLightOn();
    int failed_startup_count = 0;
    bool is_water_full = false;

    std::cout << "[System activated] Initial State: " << (last_known_state ? "🟢 ON" : "⚪ OFF") 
              << " | servo press angle: " << SERVO_PRESS_ANGLE << "°" << std::endl;

    while (g_running) {
        auto now = std::chrono::steady_clock::now();

        // 讀取實體感測器
        SmartDehum::SensorReading reading;
        try {
            reading = sensor.read_measurement();
        } catch (const std::exception& e) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        bool is_on = dehum.isLightOn();
        if (is_on != last_known_state) {
            last_known_state = is_on;
            last_state_change = now;
            if (is_on) {
                failed_startup_count = 0;
                is_water_full = false;
            }
        }

        double dew_point = SmartDehum::Thermodynamics::calculate_dew_point(reading.temperature_c, reading.relative_humidity);
        double abs_h = SmartDehum::Thermodynamics::calculate_absolute_humidity(reading.temperature_c, reading.relative_humidity);

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
        std::string action = "NONE";
        std::string status_msg = "Maintain current state";
        auto state_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_state_change).count();

        // Prioritize the manual trigger from the iOS monitor app
        if (g_manual_trigger.exchange(false)) {
            action = "MANUAL_TRIGGER";
            status_msg = "Manual trigger sent from iOS monitor app";
            std::cout << "\n>>> [Manual trigger] Sent from iOS, servo press 27.0° ! " << std::endl;
            
            dehum.triggerPress();
            last_state_change = now;
            
            // If current state is OFF
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!dehum.isLightOn()) {
                failed_startup_count++;
                if (failed_startup_count >= 3) is_water_full = true;
            } else {
                failed_startup_count = 0;
                is_water_full = false;
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

                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    if (!dehum.isLightOn()) {
                        failed_startup_count++;
                        std::cout << "⚠️ [Warning] Light undetected during startup, consecutive failing counts:" << failed_startup_count << "/3" << std::endl;
                        if (failed_startup_count >= 3) {
                            is_water_full = true;
                        }
                    } else {
                        failed_startup_count = 0;
                        is_water_full = false;
                    }
                } else {
                    status_msg = "Too humid, but operation protection remains " + std::to_string(MIN_REST_TIME_SEC - state_elapsed) + "s";
                }
            } else {
                status_msg = "Standby (Moderate humidity)";
            }
        }

        // 導出狀態至 JSON
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

        std::cout << "[Up-to-date] RH: " << std::fixed << std::setprecision(1) << reading.relative_humidity
                  << "% | State: " << (is_on ? "🟢 ON" : "⚪ OFF") 
                  << (is_water_full ? "[Full Water Warning]" : "")
                  << " | Decision: " << status_msg << std::endl;

        for (int i = 0; i < LOOP_INTERVAL_SEC && g_running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    http_server.stop();
    return 0;
}
