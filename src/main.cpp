#include <iostream>
#include <string>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/rt_bvh.h"
#include "scarlettx/runtime/emulator.h"

namespace {

void PrintUsage() {
    std::cout << "ScarlettX — Xbox Series X|S (Scarlett) HLE Emulator\n"
              << "Usage:\n"
              << "  scarlettx inspect <package_dir>\n"
              << "  scarlettx boot <package_dir> [--headless] [--in-process] [--gpu|--warp] [--trace] [--stats] [--uncapped] [--force-relocate] [--frames N] [--rt-dump path.ppm] [--raster-dump path.ppm]\n"
              << "  scarlettx diag\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::string cmd = argv[1];
    if (cmd == "diag") {
        scarlettx::D3D12Backend backend;
        auto st = backend.Initialize(true, false);
        if (!st.ok()) {
            std::cerr << "[diag] D3D12 init failed: " << st.message << "\n";
            return 1;
        }
        const auto& info = backend.AdapterInfo();
        std::cout << "[diag] Adapter: " << info.description
                  << " (WARP=" << (info.is_warp ? "yes" : "no")
                  << ", Device5=" << (info.supports_device5 ? "yes" : "no")
                  << ", DXR Tier=" << info.raytracing_tier << ")\n";
        return 0;
    }

    if (cmd == "inspect" && argc >= 3) {
        scarlettx::Emulator emu;
        scarlettx::PackageInspectionReport rep;
        auto st = emu.InspectPackage(argv[2], rep);
        if (!st.ok()) {
            std::cerr << "[inspect] Error: " << st.message << "\n";
            return 1;
        }
        std::cout << "Title: " << rep.config.display_name << " (" << rep.config.title_id << ")\n"
                  << "Identity: " << rep.config.identity_name << " v" << rep.config.version << "\n"
                  << "Scarlett Native: " << (rep.config.IsNativeScarlett() ? "YES (XboxGen9)" : "NO") << "\n"
                  << "Executable: " << rep.executable_path << " (PE64=" << (rep.main_pe.is_pe64 ? "yes" : "no") << ")\n"
                  << "Imports (" << rep.main_pe.imports.size() << " modules):\n";
        for (const auto& mod : rep.main_pe.imports) {
            std::cout << "  - " << mod.dll_name
                      << (mod.is_scarlett_hle ? " [HLE -> " + mod.redirected_target + "]" : "")
                      << " (" << mod.symbols.size() << " symbols)\n";
        }
        return 0;
    }

    if (cmd == "boot" && argc >= 3) {
        scarlettx::EmulatorConfig cfg;
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--headless") {
                cfg.headless = true;
            } else if (arg == "--in-process") {
                cfg.in_process = true;
            } else if (arg == "--gpu") {
                cfg.prefer_warp = false;
            } else if (arg == "--warp") {
                cfg.prefer_warp = true;
            } else if (arg == "--trace") {
                cfg.trace = true;
            } else if (arg == "--stats") {
                cfg.stats = true;
            } else if (arg == "--uncapped") {
                cfg.uncapped = true;
            } else if (arg == "--force-relocate") {
                cfg.force_relocate = true;
            } else if (arg == "--frames" && i + 1 < argc) {
                cfg.max_frames = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--rt-dump" && i + 1 < argc) {
                cfg.rt_dump_path = argv[++i];
            } else if (arg == "--raster-dump" && i + 1 < argc) {
                cfg.raster_dump_path = argv[++i];
            }
        }
        scarlettx::Emulator emu(cfg);
        auto st = emu.BootPackage(argv[2]);
        if (!st.ok()) {
            std::cerr << "[boot] Error: " << st.message << "\n";
            return 1;
        }
        std::cout << "[boot] Guest exited cleanly.\n";
        return 0;
    }

    PrintUsage();
    return 1;
}
