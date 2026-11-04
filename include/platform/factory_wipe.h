#pragma once

#ifdef ARDUINO
namespace platform {
// Factory wipe right before a reboot: erases the whole NVS partition (configuration,
// TLS identity, HMI preferences, Wi-Fi/PHY data), the LittleFS partition (debug log)
// and the core dump. Nothing may use NVS or LittleFS afterwards; reboot immediately.
void wipeAllStorage();
}  // namespace platform
#endif
