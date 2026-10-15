#pragma once

#include <vector>
#include <deque>

namespace SmartDehum {

class FeatureBuilder {
public:
    struct EnvironmentSample {
        double temp;
        double rh;
        double abs_h;
        double dew_point;
        bool is_on;
        double ode_pred_5m;
    };

    static std::vector<double> build_features(
        const EnvironmentSample& curr,
        const std::deque<EnvironmentSample>& history) 
    {
        std::vector<double> f;
        f.reserve(15);

        // 1-4. 當前物理量 (歸一化)
        f.push_back(curr.temp / 40.0);           // [0]
        f.push_back(curr.rh / 100.0);            // [1]
        f.push_back(curr.abs_h / 30.0);          // [2]
        f.push_back(curr.dew_point / 35.0);      // [3]

        // 5. 當前除濕機開關狀態
        f.push_back(curr.is_on ? 1.0 : 0.0);     // [4]

        // 6. ODE 5分鐘物理基底
        f.push_back(curr.ode_pred_5m / 30.0);    // [5]

        // 7-10. 1輪前差值 (無歷史則補 0.0)
        if (!history.empty()) {
            const auto& prev = history.back();
            f.push_back((curr.abs_h - prev.abs_h) * 5.0); // [6]
            f.push_back((curr.temp - prev.temp) * 2.0);   // [7]
            f.push_back((curr.rh - prev.rh) * 0.1);       // [8]
            f.push_back(prev.is_on ? 1.0 : 0.0);          // [9]
        } else {
            f.push_back(0.0); // [6]
            f.push_back(0.0); // [7]
            f.push_back(0.0); // [8]
            f.push_back(0.0); // [9]
        }

        // 11-14. 3輪前差值 (無足夠歷史則補 0.0)
        if (history.size() >= 3) {
            const auto& prev3 = history[history.size() - 3];
            f.push_back((curr.abs_h - prev3.abs_h) * 2.0); // [10]
            f.push_back((curr.temp - prev3.temp) * 1.0);   // [11]
            f.push_back((curr.rh - prev3.rh) * 0.05);      // [12]
            f.push_back(prev3.is_on ? 1.0 : 0.0);          // [13]
        } else {
            f.push_back(0.0); // [10]
            f.push_back(0.0); // [11]
            f.push_back(0.0); // [12]
            f.push_back(0.0); // [13]
        }

        // 15. 外界回潮估計項
        f.push_back((curr.dew_point - curr.temp) / 10.0);           // [14]

        return f;
    }
};

} // namespace SmartDehum
