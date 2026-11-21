#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "scarlettx/audio/xaudio2_abi.h"
#include "scarlettx/gpu/d3d12_xs_abi.h"
#include "scarlettx/io/dstorage_abi.h"
#include "scarlettx/loader/xvd_container.h"
#include "scarlettx/runtime/emulator.h"
#include "synthetic_games/scarlett_arena/arena_assets.h"

namespace {

int g_failures = 0;

#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": "       \
                      << #cond << "\n";                                         \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

std::vector<SCARLETT_BVH_TRIANGLE> BuildArenaStaticGeometry() {
    std::vector<SCARLETT_BVH_TRIANGLE> tris;
    auto push_tri = [&](float x0, float y0, float z0,
                        float x1, float y1, float z1,
                        float x2, float y2, float z2,
                        float r, float g, float b, float refl) {
        SCARLETT_BVH_TRIANGLE t{};
        t.v0[0] = x0; t.v0[1] = y0; t.v0[2] = z0;
        t.v1[0] = x1; t.v1[1] = y1; t.v1[2] = z1;
        t.v2[0] = x2; t.v2[1] = y2; t.v2[2] = z2;
        t.color[0] = r; t.color[1] = g; t.color[2] = b;
        t.reflectivity = refl;
        tris.push_back(t);
    };

    // Reflective arena floor
    push_tri(-4.5f, 0.0f, -4.5f,  4.5f, 0.0f, -4.5f,  4.5f, 0.0f, 4.5f, 0.28f, 0.31f, 0.38f, 0.45f);
    push_tri(-4.5f, 0.0f, -4.5f,  4.5f, 0.0f,  4.5f, -4.5f, 0.0f, 4.5f, 0.28f, 0.31f, 0.38f, 0.45f);

    // Back glossy arena wall
    push_tri(-4.0f, 0.0f, 3.0f,  4.0f, 0.0f, 3.0f,  4.0f, 3.2f, 3.0f, 0.14f, 0.19f, 0.28f, 0.55f);
    push_tri(-4.0f, 0.0f, 3.0f,  4.0f, 3.2f, 3.0f, -4.0f, 3.2f, 3.0f, 0.14f, 0.19f, 0.28f, 0.55f);

    // Left emerald monolith
    push_tri(-2.2f, 0.0f, 0.8f, -1.6f, 0.0f, 0.8f, -1.6f, 1.8f, 0.8f, 0.12f, 0.78f, 0.48f, 0.20f);
    push_tri(-2.2f, 0.0f, 0.8f, -1.6f, 1.8f, 0.8f, -2.2f, 1.8f, 0.8f, 0.12f, 0.78f, 0.48f, 0.20f);

    // Right crimson monolith
    push_tri(1.6f, 0.0f, 0.8f, 2.2f, 0.0f, 0.8f, 2.2f, 1.8f, 0.8f, 0.86f, 0.22f, 0.30f, 0.20f);
    push_tri(1.6f, 0.0f, 0.8f, 2.2f, 1.8f, 0.8f, 1.6f, 1.8f, 0.8f, 0.86f, 0.22f, 0.30f, 0.20f);

    return tris;
}

std::vector<uint8_t> BuildSyntheticXma2SfxPacket() {
    constexpr uint32_t kFrames = 640;
    XMA2_PACKET_HEADER hdr{};
    hdr.Magic = 0x32414D58u;  // 'XMA2'
    hdr.Channels = 1;
    hdr.SampleRateDiv100 = 480;  // 48000 Hz
    hdr.SampleCount = kFrames;
    hdr.InitialSample = 0;
    hdr.StepSize = 128;

    std::vector<uint8_t> pkt(sizeof(hdr) + kFrames, 0);
    std::memcpy(pkt.data(), &hdr, sizeof(hdr));
    int32_t prev = 0;
    for (uint32_t i = 0; i < kFrames; ++i) {
        const float s = std::sin(2.0f * 3.14159265f * 660.0f * static_cast<float>(i) / 48000.0f);
        const int32_t target = static_cast<int32_t>(s * 14000.0f);
        int32_t delta = (target - prev) / 128;
        if (delta > 127) delta = 127;
        if (delta < -128) delta = -128;
        prev += delta * 128;
        pkt[sizeof(hdr) + i] = static_cast<uint8_t>(static_cast<int8_t>(delta));
    }
    return pkt;
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

void SetEnv(const char* name, const std::string& val) {
#if defined(_WIN32)
    _putenv_s(name, val.c_str());
#else
    setenv(name, val.c_str(), 1);
#endif
}

}  // namespace

int main() {
    const std::filesystem::path bin_dir = SCARLETTX_BINARY_DIR;
    const std::filesystem::path pkg_dir = bin_dir / "packages" / "scarlett_arena";
    const std::filesystem::path xvd_path = pkg_dir / "arena_assets.xvd";
    const std::filesystem::path inproc_frame0 = bin_dir / "arena_inproc_frame0.ppm";
    const std::filesystem::path inproc_frame3 = bin_dir / "arena_inproc_frame3.ppm";
    const std::filesystem::path inproc_wav = bin_dir / "arena_inproc_audio.wav";
    const std::filesystem::path proc_frame0 = std::filesystem::absolute("arena_proc_frame0.ppm");
    const std::filesystem::path proc_frame3 = bin_dir / "arena_proc_frame3.ppm";

    // 1. Build synthetic XVD asset container with compressed arena geometry + XMA2 SFX.
    const auto static_tris = BuildArenaStaticGeometry();
    const auto xma2_sfx = BuildSyntheticXma2SfxPacket();

    scarlett_arena::ArenaAssetHeader hdr{};
    hdr.magic = scarlett_arena::kArenaBundleMagic;
    hdr.triangle_count = static_cast<uint32_t>(static_tris.size());
    hdr.xma2_bytes = static_cast<uint32_t>(xma2_sfx.size());
    hdr.uncompressed_size = static_cast<uint32_t>(
        sizeof(hdr) + static_tris.size() * sizeof(SCARLETT_BVH_TRIANGLE) + xma2_sfx.size());

    std::vector<uint8_t> raw_bundle(hdr.uncompressed_size, 0);
    std::memcpy(raw_bundle.data(), &hdr, sizeof(hdr));
    std::memcpy(raw_bundle.data() + sizeof(hdr),
                static_tris.data(),
                static_tris.size() * sizeof(SCARLETT_BVH_TRIANGLE));
    std::memcpy(raw_bundle.data() + sizeof(hdr) + static_tris.size() * sizeof(SCARLETT_BVH_TRIANGLE),
                xma2_sfx.data(),
                xma2_sfx.size());

    std::vector<uint8_t> compressed(raw_bundle.size() * 2 + 64, 0);
    uint32_t comp_size = 0;
    SX_CHECK(DStorageCompressBuffer(DSTORAGE_COMPRESSION_FORMAT_LZ_RLE,
                                    raw_bundle.data(),
                                    static_cast<uint32_t>(raw_bundle.size()),
                                    compressed.data(),
                                    static_cast<uint32_t>(compressed.size()),
                                    &comp_size) == 0);
    compressed.resize(comp_size);

    std::vector<uint8_t> xvd_image = scarlettx::XvdContainer::BuildSyntheticXvd(compressed);
    SX_CHECK(!xvd_image.empty());
    {
        std::ofstream out(xvd_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(xvd_image.data()),
                  static_cast<std::streamsize>(xvd_image.size()));
    }
    SX_CHECK(std::filesystem::exists(xvd_path));

    // 2. Boot scarlett_arena through the IN-PROCESS PE64 mapper with forced relocation.
    SetEnv("SCARLETTX_ARENA_XVD", xvd_path.string());
    SetEnv("SCARLETTX_ARENA_BUNDLE_SIZE", std::to_string(raw_bundle.size()));
    SetEnv("SCARLETTX_ARENA_LAST_FRAME", inproc_frame3.string());
    SetEnv("SCARLETTX_ARENA_WAV", inproc_wav.string());

    scarlettx::EmulatorConfig inproc_cfg{};
    inproc_cfg.headless = true;
    inproc_cfg.prefer_warp = true;
    inproc_cfg.in_process = true;
    inproc_cfg.force_relocate = true;
    inproc_cfg.in_process_entry = "ScarlettGuestMain";
    inproc_cfg.rt_dump_path = inproc_frame0.string();

    scarlettx::Emulator emu(inproc_cfg);
    auto st = emu.BootPackage(pkg_dir.string());
    if (!st.ok()) {
        std::cerr << "[FAIL] In-process boot failed: " << st.message << "\n";
    }
    SX_CHECK(st.ok());

    const auto f0_bytes = ReadFileBytes(inproc_frame0);
    const auto f3_bytes = ReadFileBytes(inproc_frame3);
    const auto wav_bytes = ReadFileBytes(inproc_wav);

    SX_CHECK(f0_bytes.size() > 160 * 120 * 3);
    SX_CHECK(f3_bytes.size() > 160 * 120 * 3);
    SX_CHECK(f0_bytes != f3_bytes);
    SX_CHECK(wav_bytes.size() > 44);
    if (wav_bytes.size() >= 12) {
        SX_CHECK(std::memcmp(wav_bytes.data(), "RIFF", 4) == 0);
        SX_CHECK(std::memcmp(wav_bytes.data() + 8, "WAVE", 4) == 0);
    }

    // 3. Boot scarlett_arena as a child process and verify byte-identical RT output.
    SetEnv("SCARLETTX_ARENA_LAST_FRAME", proc_frame3.string());
    SetEnv("SCARLETTX_ARENA_WAV", "");

    scarlettx::EmulatorConfig proc_cfg{};
    proc_cfg.headless = true;
    proc_cfg.prefer_warp = true;
    proc_cfg.in_process = false;
    // Relative: has to land in this process's working directory, not the guest's.
    proc_cfg.rt_dump_path = proc_frame0.filename().string();
    std::error_code ec;
    std::filesystem::remove(proc_frame0, ec);

    scarlettx::Emulator emu_proc(proc_cfg);
    auto st2 = emu_proc.BootPackage(pkg_dir.string());
    if (!st2.ok()) {
        std::cerr << "[FAIL] Child-process boot failed: " << st2.message << "\n";
    }
    SX_CHECK(st2.ok());

    const auto pf0_bytes = ReadFileBytes(proc_frame0);
    const auto pf3_bytes = ReadFileBytes(proc_frame3);
    SX_CHECK(pf0_bytes == f0_bytes);
    SX_CHECK(pf3_bytes == f3_bytes);

    if (g_failures == 0) {
        std::cout << "[PASS] test_scarlett_arena_e2e (in-process PE64 + child-process E2E)\n";
        return 0;
    }
    return 1;
}
