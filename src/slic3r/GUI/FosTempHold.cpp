#include "FosTempHold.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/GatewayService.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <mutex>
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
constexpr double kMinActive   = 180.;    // below: standby / prep targets of the U1 start macros (70-170 C)
constexpr double kMaxTemp     = 300.;
constexpr double kTolerance   = 0.5;
constexpr long   kFooterBytes = 1048576; // the config block sits in the file's last 1 MiB (Moonraker reads the same)
constexpr int    kFetchTries  = 3;

std::mutex                               g_mutex;
std::weak_ptr<Gateway::GatewayService>   g_gateway;
std::atomic<bool>                        g_stop{false};

std::shared_ptr<Gateway::GatewayService> current_gateway()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_gateway.lock();
}

const char *heater_name(int head) { return head == 0 ? "extruder" : head == 1 ? "extruder1" : head == 2 ? "extruder2" : "extruder3"; }

const json *child(const json &obj, const char *key)
{
    if (!obj.is_object())
        return nullptr;
    const auto it = obj.find(key);
    return it == obj.end() || it->is_null() ? nullptr : &*it;
}
std::string child_string(const json &obj, const char *key)
{
    const json *v = child(obj, key);
    return v != nullptr && v->is_string() ? v->get<std::string>() : std::string();
}
std::optional<double> child_number(const json &obj, const char *key)
{
    const json *v = child(obj, key);
    return v != nullptr && v->is_number() ? std::optional<double>(v->get<double>()) : std::nullopt;
}

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

// Per-filament temperatures the print file itself sets: printing, first layer, idle, prime-tower interface,
// and printing + standby delta. Filament i prints on head extruder_map_table[i].
struct FileTemps
{
    std::vector<std::vector<double>> per_filament;
    bool                             calibration = false;
};

std::optional<FileTemps> fetch_file_temps(const std::string &ip, const std::string &filename)
{
    std::string path;
    for (size_t start = 0; start <= filename.size();) {
        const size_t slash = filename.find('/', start);
        path += (path.empty() ? "" : "/") + Http::url_encode(filename.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    std::string footer;
    bool        ok = false;
    Http::get("http://" + ip + ":7125/server/files/gcodes/" + path)
        .header("Range", "bytes=-" + std::to_string(kFooterBytes))
        .size_limit(2 * kFooterBytes)
        .timeout_max(20)
        .on_complete([&](std::string body, unsigned status) {
            ok     = status == 206 || (status == 200 && body.size() <= size_t(2 * kFooterBytes));
            footer = std::move(body);
        })
        .on_error([&](std::string, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: cannot read " << filename << " (" << status << " " << error << ")";
        })
        .perform_sync();
    if (!ok)
        return std::nullopt;
    const std::vector<double> print = parse_list(footer, "nozzle_temperature");
    if (print.empty())
        return std::nullopt;
    const std::vector<double> first     = parse_list(footer, "nozzle_temperature_initial_layer");
    const std::vector<double> idle      = parse_list(footer, "idle_temperature");
    const std::vector<double> interface = parse_list(footer, "filament_tower_interface_print_temp");
    const std::vector<double> delta     = parse_list(footer, "standby_temperature_delta");
    FileTemps out;
    out.per_filament.resize(print.size());
    for (size_t f = 0; f < print.size(); ++f) {
        auto &t = out.per_filament[f];
        t.push_back(print[f]);
        if (f < first.size())
            t.push_back(first[f]);
        if (f < idle.size())
            t.push_back(idle[f]);
        if (f < interface.size())
            t.push_back(interface[f]);
        if (!delta.empty())
            for (double base : {print[f], f < first.size() ? first[f] : print[f]})
                t.push_back(base + delta.front());
    }
    // A temperature tower changes temperatures on purpose; never hold during one.
    out.calibration = footer.find("\n; calib_mode") != std::string::npos || footer.find("Temperature tower") != std::string::npos;
    return out;
}

bool contains(const std::vector<double> &values, double value)
{
    for (double v : values)
        if (std::abs(v - value) < kTolerance)
            return true;
    return false;
}

struct HeadState
{
    std::optional<double> last;      // last seen target
    bool                  armed = false; // seen at a file temperature during this print
    std::optional<double> hold;      // the user's temperature
    std::optional<double> replaced;  // the file temperature the hold last replaced
};

void run()
{
    using namespace std::chrono;
    std::string                       filename;
    std::optional<FileTemps>          file;
    int                               fetch_failures = 0;
    std::array<HeadState, kHeads>     heads;

    auto reset = [&]() {
        filename.clear();
        file.reset();
        fetch_failures = 0;
        heads = {};
    };

    while (!g_stop.load()) {
        for (int i = 0; i < 8 && !g_stop.load(); ++i)
            std::this_thread::sleep_for(milliseconds(250));
        if (g_stop.load())
            break;
        try {
            const std::shared_ptr<Gateway::GatewayService> gateway = current_gateway();
            if (!gateway || !gateway->is_connected()) {
                reset();
                continue;
            }
            const Gateway::GatewayService::ApiResult snapshot = gateway->get_device();
            if (snapshot.error)
                continue;
            const json *data = child(snapshot.value, "data");
            if (data == nullptr)
                data = &snapshot.value;
            const json *objects = child(*data, "objects");
            const json *stats   = objects != nullptr ? child(*objects, "print_stats") : nullptr;
            const std::string state = stats != nullptr ? child_string(*stats, "state") : std::string();
            if (state != "printing" && state != "paused") {
                if (!filename.empty())
                    BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: print ended, manual head temperatures released";
                reset();
                continue;
            }
            const json       *connection = child(*data, "connection");
            const std::string ip         = connection != nullptr ? child_string(*connection, "ip") : std::string();
            const std::string name       = child_string(*stats, "filename");
            if (ip.empty() || name.empty())
                continue;
            if (name != filename) {
                reset();
                filename = name;
            }
            if (!file) {
                if (fetch_failures >= kFetchTries)
                    continue;
                file = fetch_file_temps(ip, name);
                if (!file) {
                    if (++fetch_failures == kFetchTries)
                        BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: no temperatures in " << name << ", not watching this print";
                    continue;
                }
                BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: watching " << name << (file->calibration ? " (calibration print, holds off)" : "");
            }
            if (file->calibration)
                continue;

            // Head -> file temperatures, from the current filament mapping.
            std::array<std::vector<double>, kHeads> head_temps;
            const json *task = child(*objects, "print_task_config");
            const json *map  = task != nullptr ? child(*task, "extruder_map_table") : nullptr;
            for (size_t f = 0; f < file->per_filament.size(); ++f) {
                int head = int(f);
                if (map != nullptr && map->is_array() && f < map->size() && (*map)[f].is_number_integer())
                    head = (*map)[f].get<int>();
                if (head >= 0 && head < kHeads)
                    head_temps[head].insert(head_temps[head].end(), file->per_filament[f].begin(), file->per_filament[f].end());
            }

            for (int h = 0; h < kHeads; ++h) {
                const json *heater = child(*objects, heater_name(h));
                const std::optional<double> target = heater != nullptr ? child_number(*heater, "target") : std::nullopt;
                if (!target)
                    continue;
                HeadState  &hs        = heads[h];
                const bool  file_temp = *target >= kMinActive && contains(head_temps[h], *target);
                const bool  changed   = !hs.last || std::abs(*hs.last - *target) >= kTolerance;
                const std::optional<double> previous = hs.last;
                hs.last = *target;
                if (file_temp)
                    hs.armed = true;
                if (!changed)
                    continue;

                if (hs.hold) {
                    if (*target < kTolerance) { // heater switched off: release
                        BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: head " << h + 1 << " switched off, hold released";
                        hs.hold.reset();
                        continue;
                    }
                    if (file_temp && previous && std::abs(*previous - *hs.hold) < kTolerance && hs.replaced &&
                        std::abs(*target - *hs.replaced) < kTolerance) {
                        // Straight from the held value back to the value it replaced: the user undid it.
                        BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: head " << h + 1 << " set back to " << *target << " C, hold released";
                        hs.hold.reset();
                        continue;
                    }
                    if (file_temp) {
                        hs.replaced = *target;
                        std::ostringstream script;
                        script << "SET_HEATER_TEMPERATURE HEATER=" << heater_name(h) << " TARGET=" << *hs.hold;
                        const auto result = gateway->request_sync("printer.gcode.script", {{"script", script.str()}}, seconds(10));
                        hs.last = *hs.hold;
                        BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: print set head " << h + 1 << " to " << *target << " C; restored "
                                                   << *hs.hold << " C" << (result.error ? " (failed: " + result.error.message + ")" : "");
                        continue;
                    }
                }
                // A new manual temperature: only while printing, on an armed head that was at a file (or held)
                // temperature, to a hot value the file never uses.
                const bool from_printing = previous && *previous >= kMinActive &&
                                           (contains(head_temps[h], *previous) || (hs.hold && std::abs(*previous - *hs.hold) < kTolerance));
                if (state == "printing" && hs.armed && from_printing && *target >= kMinActive && *target <= kMaxTemp && !file_temp) {
                    if (!hs.hold)
                        hs.replaced = *previous;
                    hs.hold = *target;
                    BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: head " << h + 1 << " manually set to " << *target
                                               << " C, holding it for this print";
                }
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "[FOS] temp-hold: " << e.what();
        }
    }
}

} // namespace

void FosTempHold::start(std::shared_ptr<Gateway::GatewayService> gateway)
{
    static bool started = false;
    if (!gateway)
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_gateway = gateway;
    }
    AppConfig *cfg = wxGetApp().app_config;
    if (started || (cfg != nullptr && cfg->has("fos_hold_manual_temps") && !cfg->get_bool("fos_hold_manual_temps")))
        return;
    started = true;
    std::thread(run).detach();
}

void FosTempHold::stop() { g_stop.store(true); }

} // namespace GUI
} // namespace Slic3r
