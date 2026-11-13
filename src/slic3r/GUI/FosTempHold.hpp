#pragma once

#include <memory>

namespace Slic3r {
namespace Gateway { class GatewayService; }
namespace GUI {

// FOS: on the Snapmaker U1 the print's G-code re-sets every head's target at each tool change, so a
// temperature set on the touchscreen or the Device page only lasts until that head prints again. This
// watcher keeps such a manual change for the rest of the print: while a printing head sits at one of the
// print file's temperatures and its target changes to a value the file never uses, that value is held and
// re-applied whenever the print sets the head back to a file temperature. Setting the head to 0 or back to
// the temperature the hold replaced releases it; holds end with the print.
class FosTempHold
{
public:
    // Starts the watcher once per app run (later calls only switch it to `gateway`, e.g. after the GUI is
    // recreated). Disabled with app config "fos_hold_manual_temps" = 0.
    static void start(std::shared_ptr<Gateway::GatewayService> gateway);
    // Ends the watcher thread (app exit).
    static void stop();
};

} // namespace GUI
} // namespace Slic3r
