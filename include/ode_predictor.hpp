#pragma once

#include <algorithm>

namespace SmartDehum {

class OdePredictor {
public:
    struct Params {
        double room_volume_m3 = 30.0;    // 房間體積 (m³)
        double dehum_rate_g_min = 1.2;    // 工況淨除濕率 (g/min)
        double leak_rate_min = 0.005;     // 回潮/滲透率 (1/min)
        double ambient_abs_h = 18.5;      // 外界環境含水量基準 (g/min)
    };

    // 預設建構子，直接使用 Params 的預設成員值
    OdePredictor() : params_(Params{}) {}

    // 支援自訂參數的建構子
    explicit OdePredictor(const Params& params) : params_(params) {}

    // 濕度微分方程: dw/dt
    double moisture_derivative(double current_w, bool is_on) const {
        double dehum_term = is_on ? (params_.dehum_rate_g_min / params_.room_volume_m3) : 0.0;
        double leak_term = params_.leak_rate_min * (params_.ambient_abs_h - current_w);
        return -dehum_term + leak_term;
    }

    // 四階 Runge-Kutta (RK4) 數值積分
    double predict(double initial_w, bool is_on, double horizon_mins, double dt_mins = 0.5) const {
        double w = initial_w;
        double t = 0.0;

        while (t < horizon_mins) {
            double h = std::min(dt_mins, horizon_mins - t);

            double k1 = moisture_derivative(w, is_on);
            double k2 = moisture_derivative(w + 0.5 * h * k1, is_on);
            double k3 = moisture_derivative(w + 0.5 * h * k2, is_on);
            double k4 = moisture_derivative(w + h * k3, is_on);

            w += (h / 6.0) * (k1 + 2.0 * k2 + 2.0 * k3 + k4);
            t += h;
        }
        return w;
    }

    void update_ambient_reference(double ambient_abs_h) {
        params_.ambient_abs_h = ambient_abs_h;
    }

private:
    Params params_;
};

} // namespace SmartDehum
