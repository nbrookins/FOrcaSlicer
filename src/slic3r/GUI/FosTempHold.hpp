#pragma once

#include <memory>

namespace Slic3r {
namespace Gateway { class GatewayService; }
namespace GUI {

// FOS: on the Snapmaker U1 the print's G-code re-sets every head's target at each tool change, so a
// temperature set on the touchscreen or the Device page only lasts until that head prints again. This
// watcher keeps such a manual change for the rest of the print: a head target that is not one of the
// print file's temperatures for that head is remembered, and re-applied whenever the print sets that
// head back to a file temperature. Overrides are dropped when the print ends.
class FosTempHold
{
public:
    // Starts the background watcher once per app run (no-op afterwards or when disabled with
    // app config "fos_hold_manual_temps" = 0).
    static void start(std::shared_ptr<Gateway::GatewayService> gateway);
};

} // namespace GUI
} // namespace Slic3r
