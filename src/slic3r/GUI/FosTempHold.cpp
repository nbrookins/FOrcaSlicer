#include "FosTempHold.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/GatewayService.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {

using json = nlohmann::json;

constexpr int    kHeads       = 4;
constexpr double kMinActive   = 180.;  // below: standby / prep targets of the U1 start macros (70-170 C)
constexpr double kMaxTemp     = 300.;
constexpr double kTolerance   = 0.5;
constexpr long   kFooterBytes = 1048576; // the config block sits in the file's last 1 MiB (Moonraker reads the same)

const char *heater_name(int head) { return head == 0 ? "extruder" : head == 1 ? "extruder1" : head == 2 ? "extruder2" : "extruder3"; }

std::vector<double> parse_list(const std::string &footer, const std::string &key)
{
    std::vector<double> out;
    const std::string   tag = "\n; " + key + " = ";
    const size_t        pos = footer.rfind(tag);
    if (pos == std::string::npos)
        return out;
    const size_t       start = pos + tag.size();
    std::istringstream in(footer.substr(start, footer.find('\n', start) - start));
    for (std::string item; std::getline(in, item, ',');)
        out.push_back(std::atof(item.c_str()));
    return out;
}

// The print file's temperatures per physical head (both layer-1 and later-layer values).
std::optional<std::array<std::vector<double>, kHeads>> fetch_file_temps(const std::string &ip, const std::string &filename,
                                                                         const std::vector<int> &map_table)
{
    std::string footer;
    bool        ok = false;
    Http::get("http://" + ip + ":7125/server/files/gcodes/" + Http::url_encode(filename))
        .header("Range", "bytes=-" + std::to_string(kFooterBytes))
        .timeout_max(20)
        .on_complete([&](std::string body, unsigned status) {
            ok     = status == 200 || status == 206;
            footer = std::move(body);
        })
        .on_error([&](std::string, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: cannot read " << filename << " (" << status << " " << error << ")";
        })
        .perform_sync();
    if (!ok)
        return std::nullopt;
    std::array<std::vector<double>, kHeads> temps;
    for (const char *key : {"nozzle_temperature", "nozzle_temperature_initial_layer"}) {
        const std::vector<double> values = parse_list(footer, key);
        for (size_t filament = 0; filament < values.size(); ++filament) {
            const int head = filament < map_table.size() ? map_table[filament] : int(filament);
            if (head >= 0 && head < kHeads && values[filament] > 0.)
                temps[head].push_back(values[filament]);
        }
    }
    bool any = false;
    for (const auto &t : temps)
        any = any || !t.empty();
    if (!any)
        return std::nullopt;
    return temps;
}

bool contains(const std::vector<double> &values, double value)
{
    for (double v : values)
        if (std::abs(v - value) < kTolerance)
            return true;
    return false;
}

void run(std::shared_ptr<Gateway::GatewayService> gateway)
{
    using namespace std::chrono;
    std::string                                             filename;
    std::optional<std::array<std::vector<double>, kHeads>> file_temps;
    std::array<std::optional<double>, kHeads>              hold;

    auto reset = [&]() {
        filename.clear();
        file_temps.reset();
        for (auto &h : hold)
            h.reset();
    };

    for (;;) {
        std::this_thread::sleep_for(seconds(2));
        if (!gateway || !gateway->is_connected()) {
            reset();
            continue;
        }
        const Gateway::GatewayService::ApiResult snapshot = gateway->get_device();
        if (snapshot.error)
            continue;
        const json &data    = snapshot.value.contains("data") ? snapshot.value["data"] : snapshot.value;
        const json  objects = data.value("objects", json::object());
        const json  stats   = objects.value("print_stats", json::object());
        const std::string state = stats.value("state", "");
        if (state != "printing" && state != "paused") {
            if (!filename.empty())
                BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: print ended, manual head temperatures released";
            reset();
            continue;
        }
        const std::string ip = data.value("connection", json::object()).value("ip", "");
        const std::string file = stats.value("filename", "");
        if (ip.empty() || file.empty())
            continue;
        if (file != filename) {
            reset();
            std::vector<int> map_table;
            const json       map = objects.value("print_task_config", json::object()).value("extruder_map_table", json::array());
            for (const json &m : map)
                map_table.push_back(m.is_number_integer() ? m.get<int>() : -1);
            file_temps = fetch_file_temps(ip, file, map_table);
            if (!file_temps)
                continue; // retried next poll
            filename = file;
            BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: watching " << file;
        }
        for (int head = 0; head < kHeads; ++head) {
            const json heater = objects.value(heater_name(head), json::object());
            if (!heater.contains("target") || !heater["target"].is_number())
                continue;
            const double target = heater["target"].get<double>();
            if (target < kMinActive || target > kMaxTemp)
                continue;
            if (!contains((*file_temps)[head], target)) {
                if (!hold[head] || std::abs(*hold[head] - target) >= kTolerance) {
                    hold[head] = target;
                    BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: head " << head + 1 << " manually set to " << target
                                               << " C, holding it for this print";
                }
            } else if (hold[head] && std::abs(*hold[head] - target) >= kTolerance) {
                std::ostringstream script;
                script << "SET_HEATER_TEMPERATURE HEATER=" << heater_name(head) << " TARGET=" << *hold[head];
                const auto result = gateway->request_sync("printer.gcode.script", {{"script", script.str()}}, seconds(10));
                BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: print set head " << head + 1 << " to " << target << " C; restored "
                                           << *hold[head] << " C" << (result.error ? " (failed: " + result.error.message + ")" : "");
            }
        }
    }
}

} // namespace

void FosTempHold::start(std::shared_ptr<Gateway::GatewayService> gateway)
{
    static bool started = false;
    AppConfig  *cfg     = wxGetApp().app_config;
    if (started || !gateway || (cfg != nullptr && cfg->has("fos_hold_manual_temps") && !cfg->get_bool("fos_hold_manual_temps")))
        return;
    started = true;
    std::thread(run, std::move(gateway)).detach();
}

} // namespace GUI
} // namespace Slic3r
