#pragma once

#include <memory>

namespace Slic3r {
namespace Gateway { class GatewayService; }
namespace GUI {

// FOS: the printer's DHCP address changes, and the connection gateway keeps retrying the saved address
// forever. When the saved LAN printer is unresponsive, this watchdog finds it again by its MAC address and
// asks the gateway to connect there (macOS / unix only). A printer the user disconnected stays reachable,
// so it is left alone.
class FosReconnect
{
public:
    // Starts the watchdog once per app run (later calls only switch it to `gateway`). Disabled with app
    // config "fos_auto_reconnect" = 0.
    static void start(std::shared_ptr<Gateway::GatewayService> gateway);
    // Ends the watchdog thread (app exit).
    static void stop();
};

} // namespace GUI
} // namespace Slic3r
