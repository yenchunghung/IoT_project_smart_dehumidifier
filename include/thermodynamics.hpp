#pragma once

#include <cmath>
#include <algorithm>

namespace SmartDehum {

class Thermodynamics {
public:
    // Magnus-Tetens 公式計算露點溫度 (°C)
    static double calculate_dew_point(double temp_c, double rh_pct) {
        constexpr double a = 17.27;
        constexpr double b = 237.7;
        rh_pct = std::clamp(rh_pct, 1.0, 100.0);
        double alpha = ((a * temp_c) / (b + temp_c)) + std::log(rh_pct / 100.0);
        return (b * alpha) / (a - alpha);
    }

    // 計算飽和水汽壓 (hPa / mbar)
    static double calculate_saturation_vapor_pressure(double temp_c) {
        return 6.112 * std::exp((17.67 * temp_c) / (temp_c + 243.5));
    }

    // 計算空氣絕對含水量 (g/m³)
    static double calculate_absolute_humidity(double temp_c, double rh_pct) {
        double es = calculate_saturation_vapor_pressure(temp_c);
        double e = es * (rh_pct / 100.0);
        constexpr double mw = 18.01528; // 水分子量 g/mol
        constexpr double r = 8.31446;   // 理想氣體常數 J/(mol·K)
        double temp_k = temp_c + 273.15;
        return (e * 100.0 * mw) / (r * temp_k);
    }

    // 由預測的水氣 (g/m³) 與溫度反推相對濕度 RH (%)
    static double calculate_rh_from_absolute(double temp_c, double abs_h_g_m3) {
        double es = calculate_saturation_vapor_pressure(temp_c);
        constexpr double mw = 18.01528;
        constexpr double r = 8.31446;
        double temp_k = temp_c + 273.15;
        double e = (abs_h_g_m3 * r * temp_k) / (100.0 * mw);
        double rh = (e / es) * 100.0;
        return std::clamp(rh, 0.0, 100.0);
    }
};

} // namespace SmartDehum
