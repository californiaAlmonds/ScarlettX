#pragma once

#include <cstdint>
#include <string>

#include "scarlettx/core/types.h"

namespace scarlettx {

struct EmulatorConfig {
    bool headless = true;
    bool prefer_warp = false;
    bool enable_debug_layer = false;
    bool trace = false;
    bool stats = false;
    bool uncapped = false;
    bool in_process = false;
    bool force_relocate = false;
    uint32_t render_width = 320;
    uint32_t render_height = 240;
    uint32_t max_frames = 0;  // 0 = unlimited / controlled by guest
    DeviceType emulated_device = DeviceType::XboxScarlettAnaconda;
    RaytracingBackend rt_backend = RaytracingBackend::Auto;
    std::string shim_directory;
    std::string in_process_entry = "ScarlettGuestMain";
    std::string rt_dump_path;
    std::string raster_dump_path;
};

}  // namespace scarlettx
