#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <unistd.h>

namespace SmartDehum {

class StatusExporter {
public:
    static void export_json(const std::string& filepath,
                            double temp, double rh, double abs_h, double dew_point,
                            bool is_on, double pred_abs_h_5m, double pred_rh_5m,
                            const std::string& action_required,
                            const std::string& reason) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2);
        ss << "{\n";
        ss << "  \"temp\": " << temp << ",\n";
        ss << "  \"rh\": " << rh << ",\n";
        ss << "  \"abs_h\": " << abs_h << ",\n";
        ss << "  \"dew_point\": " << dew_point << ",\n";
        ss << "  \"is_on\": " << (is_on ? "true" : "false") << ",\n";
        ss << "  \"pred_abs_h_5m\": " << pred_abs_h_5m << ",\n";
        ss << "  \"pred_rh_5m\": " << pred_rh_5m << ",\n";
        ss << "  \"action_required\": \"" << action_required << "\",\n";
        ss << "  \"reason\": \"" << reason << "\"\n";
        ss << "}\n";

        // 寫入暫存檔後原子替換，避免 iOS 端讀取到半寫入狀態
        std::string tmp_path = filepath + ".tmp";
        std::ofstream ofs(tmp_path);
        if (ofs.is_open()) {
            ofs << ss.str();
            ofs.close();
            rename(tmp_path.c_str(), filepath.c_str());
        }
    }
};

} // namespace SmartDehum
