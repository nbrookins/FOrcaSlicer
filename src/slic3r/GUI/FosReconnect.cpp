#include "FosReconnect.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/GatewayService.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Slic3r {
namespace GUI {

namespace {

using json = nlohmann::json;

std::mutex                             g_mutex;
std::weak_ptr<Gateway::GatewayService> g_gateway;
std::atomic<bool>                      g_stop{false};

#ifndef _WIN32
constexpr int kMqttPort  = 1884; // the U1's LAN connection port
constexpr int kBatch     = 32;   // sockets open at once during a sweep (GUI apps get 256 descriptors)
constexpr int kMaxHosts  = 1024; // sweep at most a /22

std::shared_ptr<Gateway::GatewayService> current_gateway()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_gateway.lock();
}

std::string json_string(const json &obj, const char *key)
{
    if (!obj.is_object())
        return {};
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}
bool json_true(const json &obj, const char *key)
{
    if (!obj.is_object())
        return false;
    const auto it = obj.find(key);
    return it != obj.end() && ((it->is_boolean() && it->get<bool>()) || (it->is_number() && it->get<double>() != 0.));
}

// "40:fd:f3:da:b7:38", "40-FD-F3-DA-B7-38", "40fdf3dab738" and arp's "40:fd:f3:da:b7:38" / "0:1a:..." compare equal.
std::string normalize_mac(const std::string &mac)
{
    std::vector<std::string> parts;
    std::string              cur;
    for (char c : mac) {
        if (c == ':' || c == '-') {
            parts.push_back(cur);
            cur.clear();
        } else if (std::isxdigit(static_cast<unsigned char>(c)))
            cur += char(std::tolower(static_cast<unsigned char>(c)));
    }
    parts.push_back(cur);
    if (parts.size() == 1 && cur.size() == 12) {
        parts.clear();
        for (size_t i = 0; i < 12; i += 2)
            parts.push_back(cur.substr(i, 2));
    }
    std::string out;
    for (const std::string &p : parts) {
        if (p.empty() || p.size() > 2)
            return {};
        out += (out.empty() ? "" : ":") + std::string(p.size() == 1 ? "0" : "") + p;
    }
    return out;
}

std::vector<std::string> ips_for_mac(const std::string &mac)
{
    std::vector<std::string> found;
    const std::string        want = normalize_mac(mac);
    if (want.empty())
        return found;
    FILE *pipe = popen("/usr/sbin/arp -an", "r");
    if (pipe == nullptr)
        return found;
    char line[512];
    while (fgets(line, sizeof(line), pipe) != nullptr) {
        // "? (192.168.68.50) at 40:fd:f3:da:b7:38 on en1 ifscope [ethernet]"
        const std::string l(line);
        const size_t      open = l.find('('), close = l.find(')'), at = l.find(" at ");
        if (open == std::string::npos || close == std::string::npos || at == std::string::npos || close < open)
            continue;
        const size_t mac_end = l.find(' ', at + 4);
        if (normalize_mac(l.substr(at + 4, mac_end == std::string::npos ? std::string::npos : mac_end - at - 4)) == want)
            found.push_back(l.substr(open + 1, close - open - 1));
    }
    pclose(pipe);
    return found;
}

bool reachable(const std::string &ip, int timeout_ms)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(kMqttPort);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
        return false;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int err = connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0 ? 0 : errno;
    if (err == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        if (poll(&p, 1, timeout_ms) == 1) {
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        } else
            err = ETIMEDOUT;
    }
    close(fd);
    return err == 0;
}

// Touch every host of the local IPv4 network that contains `ip` (from the interface netmask, at most
// kMaxHosts) on the printer port, so the ARP table learns the printer's current address. Does nothing
// when `ip` is not on a local network (VPN, another site).
void sweep_local_network(const std::string &ip)
{
    in_addr target{};
    if (inet_pton(AF_INET, ip.c_str(), &target) != 1)
        return;
    uint32_t net = 0, mask = 0;
    ifaddrs *ifs = nullptr;
    if (getifaddrs(&ifs) != 0)
        return;
    for (ifaddrs *i = ifs; i != nullptr; i = i->ifa_next) {
        if (i->ifa_addr == nullptr || i->ifa_netmask == nullptr || i->ifa_addr->sa_family != AF_INET)
            continue;
        const uint32_t a = ntohl(reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr.s_addr);
        const uint32_t m = ntohl(reinterpret_cast<sockaddr_in *>(i->ifa_netmask)->sin_addr.s_addr);
        if (m != 0 && m != 0xffffffffu && (a & m) == (ntohl(target.s_addr) & m)) {
            net  = a & m;
            mask = m;
            break;
        }
    }
    freeifaddrs(ifs);
    if (mask == 0)
        return;
    const uint32_t hosts = std::min<uint32_t>(~mask - 1, kMaxHosts);
    for (uint32_t first = 1; first <= hosts && !g_stop.load(); first += kBatch) {
        std::vector<int> fds;
        for (uint32_t h = first; h < first + kBatch && h <= hosts; ++h) {
            sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_port        = htons(kMqttPort);
            addr.sin_addr.s_addr = htonl(net + h);
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                break;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
            fds.push_back(fd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (int fd : fds)
            close(fd);
    }
}

void run()
{
    using namespace std::chrono;
    auto      next_sweep   = steady_clock::now();
    auto      next_connect = steady_clock::now();
    minutes   sweep_gap(2);
    while (!g_stop.load()) {
        for (int i = 0; i < 80 && !g_stop.load(); ++i) // 20 s
            std::this_thread::sleep_for(milliseconds(250));
        if (g_stop.load())
            break;
        try {
            const std::shared_ptr<Gateway::GatewayService> gateway = current_gateway();
            if (!gateway || !gateway->is_connected())
                continue;
            const auto current = gateway->request_sync("query.device.current", json::object(), seconds(5));
            if (current.error || json_true(current.value, "connected")) {
                sweep_gap = minutes(2);
                continue;
            }
            const auto list = gateway->request_sync("query.device.list", json::object(), seconds(5));
            if (list.error || !list.value.is_object())
                continue;
            const auto devices = list.value.find("devices");
            if (devices == list.value.end() || !devices->is_array())
                continue;
            for (const json &dev : *devices) {
                if (json_string(dev, "type") != "LAN" || !json_true(dev, "unresponsive"))
                    continue;
                const std::string sn = json_string(dev, "sn"), mac = json_string(dev, "mac"), saved_ip = json_string(dev, "ip");
                if (sn.empty() || mac.empty() || steady_clock::now() < next_connect)
                    continue;
                auto pick = [&]() -> std::string {
                    for (const std::string &ip : ips_for_mac(mac))
                        if (ip != saved_ip && reachable(ip, 1500))
                            return ip;
                    return {};
                };
                std::string ip = pick();
                if (ip.empty() && !saved_ip.empty() && steady_clock::now() >= next_sweep) {
                    sweep_local_network(saved_ip);
                    ip         = pick();
                    next_sweep = steady_clock::now() + sweep_gap;
                    sweep_gap  = std::min<minutes>(sweep_gap * 2, minutes(10)); // printer probably off: back off
                }
                if (ip.empty())
                    continue;
                const std::string name = json_string(dev, "name");
                BOOST_LOG_TRIVIAL(warning) << "[FOS] reconnect: " << (name.empty() ? sn : name) << " moved from " << saved_ip << " to " << ip
                                           << "; connecting there";
                const auto result = gateway->request_sync("action.device.connect", {{"type", "LAN"}, {"sn", sn}, {"ip", ip}}, seconds(20));
                BOOST_LOG_TRIVIAL(warning) << "[FOS] reconnect: " << (result.error ? "failed: " + result.error.message : std::string("connected"));
                if (result.error)
                    next_connect = steady_clock::now() + minutes(1);
                else
                    sweep_gap = minutes(2);
                break;
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "[FOS] reconnect: " << e.what();
        }
    }
}
#endif

} // namespace

void FosReconnect::start(std::shared_ptr<Gateway::GatewayService> gateway)
{
#ifndef _WIN32
    static bool started = false;
    if (!gateway)
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_gateway = gateway;
    }
    AppConfig *cfg = wxGetApp().app_config;
    if (started || (cfg != nullptr && cfg->has("fos_auto_reconnect") && !cfg->get_bool("fos_auto_reconnect")))
        return;
    started = true;
    std::thread(run).detach();
#endif
}

void FosReconnect::stop() { g_stop.store(true); }

} // namespace GUI
} // namespace Slic3r
