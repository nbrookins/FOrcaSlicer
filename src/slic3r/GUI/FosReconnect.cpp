#include "FosReconnect.hpp"

#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/GatewayService.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Slic3r {
namespace GUI {

namespace {

using json = nlohmann::json;

constexpr int kMqttPort = 1884; // the U1's LAN connection port

#ifndef _WIN32
// "40:fd:f3:da:b7:38" and arp's "40:fd:f3:da:b7:38" / "0:1a:..." compare equal.
std::string normalize_mac(const std::string &mac)
{
    std::string out;
    size_t      start = 0;
    while (start <= mac.size()) {
        const size_t end = mac.find(':', start);
        const std::string part = mac.substr(start, end == std::string::npos ? std::string::npos : end - start);
        char buf[4];
        snprintf(buf, sizeof(buf), "%02lx", std::strtoul(part.c_str(), nullptr, 16));
        out += (out.empty() ? "" : ":") + std::string(buf);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return out;
}

std::string ip_for_mac(const std::string &mac)
{
    const std::string want = normalize_mac(mac);
    FILE             *pipe = popen("/usr/sbin/arp -an", "r");
    if (pipe == nullptr)
        return {};
    std::string found;
    char        line[512];
    while (found.empty() && fgets(line, sizeof(line), pipe) != nullptr) {
        // "? (192.168.68.50) at 40:fd:f3:da:b7:38 on en1 ifscope [ethernet]"
        const std::string l(line);
        const size_t      open = l.find('('), close = l.find(')'), at = l.find(" at ");
        if (open == std::string::npos || close == std::string::npos || at == std::string::npos)
            continue;
        const size_t mac_end = l.find(' ', at + 4);
        if (normalize_mac(l.substr(at + 4, mac_end - at - 4)) == want)
            found = l.substr(open + 1, close - open - 1);
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

// Touch every host of ip's /24 on the printer port so the ARP table learns the printer's current address.
void sweep_subnet(const std::string &ip)
{
    const size_t dot = ip.rfind('.');
    if (dot == std::string::npos)
        return;
    const std::string prefix = ip.substr(0, dot + 1);
    std::vector<int>  fds;
    for (int host = 1; host < 255; ++host) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(kMqttPort);
        if (inet_pton(AF_INET, (prefix + std::to_string(host)).c_str(), &addr.sin_addr) != 1)
            continue;
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
        fds.push_back(fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    for (int fd : fds)
        close(fd);
}

void run(std::shared_ptr<Gateway::GatewayService> gateway)
{
    using namespace std::chrono;
    auto last_sweep = steady_clock::now() - minutes(10);
    for (;;) {
        std::this_thread::sleep_for(seconds(20));
        if (!gateway || !gateway->is_connected())
            continue;
        const auto current = gateway->request_sync("query.device.current", json::object(), seconds(5));
        if (current.error || current.value.value("connected", false))
            continue;
        const auto list = gateway->request_sync("query.device.list", json::object(), seconds(5));
        if (list.error)
            continue;
        const json devices = list.value.value("devices", json::array());
        for (const json &dev : devices) {
            if (!dev.is_object() || dev.value("type", "") != "LAN" || !dev.value("unresponsive", false))
                continue;
            const std::string sn = dev.value("sn", ""), mac = dev.value("mac", ""), saved_ip = dev.value("ip", "");
            if (sn.empty() || mac.empty())
                continue;
            std::string ip = ip_for_mac(mac);
            if ((ip.empty() || ip == saved_ip) && steady_clock::now() - last_sweep > minutes(2) && !saved_ip.empty()) {
                last_sweep = steady_clock::now();
                sweep_subnet(saved_ip);
                ip = ip_for_mac(mac);
            }
            if (ip.empty() || ip == saved_ip || !reachable(ip, 1500))
                continue;
            BOOST_LOG_TRIVIAL(warning) << "[FOS] reconnect: " << dev.value("name", sn) << " moved from " << saved_ip << " to " << ip
                                       << "; connecting there";
            const auto result = gateway->request_sync("action.device.connect", {{"type", "LAN"}, {"sn", sn}, {"ip", ip}}, seconds(20));
            BOOST_LOG_TRIVIAL(warning) << "[FOS] reconnect: " << (result.error ? "failed: " + result.error.message : std::string("connected"));
            break;
        }
    }
}
#endif

} // namespace

void FosReconnect::start(std::shared_ptr<Gateway::GatewayService> gateway)
{
#ifndef _WIN32
    static bool started = false;
    AppConfig  *cfg     = wxGetApp().app_config;
    if (started || !gateway || (cfg != nullptr && cfg->has("fos_auto_reconnect") && !cfg->get_bool("fos_auto_reconnect")))
        return;
    started = true;
    std::thread(run, std::move(gateway)).detach();
#endif
}

} // namespace GUI
} // namespace Slic3r
