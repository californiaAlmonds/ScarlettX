#!/usr/bin/env python3
"""Generate ScarlettX's Status dashboard (variant E: purple GitHub contribution graph).

Inspects the actual C++ headers, shim exports, and CTest targets in the repo so
the reported API counts and subsystem progress reflect real implemented code.
Outputs:
  - docs/status.svg   (fixed 900px GitHub-dark purple contribution graph SVG)
  - docs/status.html  (responsive standalone HTML matching the same design DNA)
  - README.md         (auto-updated status section between SCARLETTX_STATUS markers)
"""

from __future__ import annotations

import argparse
import html
import math
import pathlib
import re
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Design DNA: ScarlettX Status, variant E (purple)
BG = "#0d1117"
BORDER = "#30363d"
BORDER_STRONG = "#3d444d"
BORDER_MUTED = "#21262d"
FG = "#e6edf3"
FG_MUTED = "#9198a1"
FG_SOFT = "#d1d7e0"
EMPTY_FILL = "#151b23"
EMPTY_STROKE = "rgba(240,246,252,0.08)"
LV0 = "#3c1e70"  # < 40%
LV1 = "#6e40c9"  # 40-59%
LV2 = "#a371f7"  # 60-79%
LV3 = "#d2a8ff"  # >= 80%
WARN = "#d29922"
BADGE_LABEL = "#555555"
BADGE_FN = "#a371f7"
BADGE_EXPORTS = "#007ec6"
BADGE_TESTS = "#3b8a1f"
BADGE_TARGET = "#6e40c9"


@dataclass
class Subsystem:
    name: str
    short_name: str
    category: str
    implemented: int
    target: int
    notes: str

    @property
    def ratio(self) -> float:
        if self.target <= 0:
            return 0.0
        return min(1.0, self.implemented / self.target)

    @property
    def pct(self) -> float:
        return self.ratio * 100.0


def level_color(ratio: float, implemented: int) -> str:
    if implemented <= 0:
        return EMPTY_FILL
    if ratio >= 0.80:
        return LV3
    if ratio >= 0.60:
        return LV2
    if ratio >= 0.40:
        return LV1
    return LV0


def level_class(ratio: float, implemented: int) -> str:
    if implemented <= 0:
        return "empty"
    if ratio >= 0.80:
        return "lv3"
    if ratio >= 0.60:
        return "lv2"
    if ratio >= 0.40:
        return "lv1"
    return "lv0"


def count_exported_symbols(header: pathlib.Path, macro: str, prefix: str) -> int:
    if not header.exists():
        return 0
    text = header.read_text(encoding="utf-8", errors="ignore")
    pat = re.compile(
        rf"{re.escape(macro)}\b[^;]*?\b({prefix}[A-Za-z0-9_]*)\s*\(",
        re.MULTILINE | re.DOTALL,
    )
    return len(set(pat.findall(text)))


def read_ctest_results(build_dir: pathlib.Path) -> tuple[int, list[str]] | None:
    """Passed count and failed test names of the build tree's last CTest run, if it has one."""
    log = build_dir / "Testing/Temporary/LastTest.log"
    if not log.exists():
        return None
    text = log.read_text(encoding="utf-8", errors="ignore")
    results = re.findall(r"^\d+/\d+ Test: (\S+)$.*?^Test (Passed|Failed)\.$", text, re.MULTILINE | re.DOTALL)
    if not results:
        return None
    failed = [name for name, outcome in results if outcome == "Failed"]
    return len(results) - len(failed), failed


def tests_label(test_count: int, results: tuple[int, list[str]] | None) -> str:
    if results is None:
        return f"{test_count} defined, not run"
    passed, failed = results
    return f"{passed}/{passed + len(failed)} passing"


def tests_markdown(test_count: int, results: tuple[int, list[str]] | None) -> str:
    if results is None:
        return f"`{test_count}` defined, not run"
    passed, failed = results
    text = f"`{passed}/{passed + len(failed)}` passing"
    if failed:
        text += " (failing: " + ", ".join(f"`{name}`" for name in failed) + ")"
    return text


MAIN_MENU_FPS_CAPTION = "Max FPS: 1189<br>Average FPS: 1082<br>Minimum FPS: 264"
IN_GAME_FPS_CAPTION = "Max FPS: 247<br>Average FPS: 188<br>Minimum FPS: 91"

SCREENSHOTS = [
    ("docs/image_00_01.png", "Main menu", "Concept Destruction main menu running in ScarlettX", MAIN_MENU_FPS_CAPTION),
    ("docs/image_00_02.png", "In game", "Concept Destruction gameplay running in ScarlettX", IN_GAME_FPS_CAPTION),
]


def collect_metrics() -> tuple[list[Subsystem], int, int]:
    xgr_hdr = ROOT / "include/scarlettx/gdk/xgameruntime_abi.h"
    d3d_hdr = ROOT / "include/scarlettx/gpu/d3d12_xs_abi.h"
    xg_hdr = ROOT / "include/scarlettx/gpu/xg_xs_abi.h"
    acp_hdr = ROOT / "include/scarlettx/audio/acphal_abi.h"
    ds_hdr = ROOT / "include/scarlettx/io/dstorage_abi.h"
    xa_hdr = ROOT / "include/scarlettx/audio/xaudio2_abi.h"
    gi_hdr = ROOT / "include/scarlettx/input/gameinput_abi.h"
    bink_hdr = ROOT / "include/scarlettx/middleware/bink2_abi.h"
    ffx_hdr = ROOT / "include/scarlettx/middleware/ffx_abi.h"
    hc_hdr = ROOT / "include/scarlettx/net/httpclient_abi.h"
    curl_hdr = ROOT / "include/scarlettx/net/xcurl_abi.h"
    cmake_txt = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8", errors="ignore")

    test_count = (
        len(re.findall(r"\bscarlettx_add_test\(", cmake_txt))
        + len(re.findall(r"\badd_test\(", cmake_txt))
        - 1
    )

    xruntime_cnt = (
        count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XGameRuntime")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XGameGet")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XError")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "InitializeApiImpl")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "UninitializeApiImpl")
        + count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "QueryApiImpl")
    )
    xtask_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XTaskQueue")
    xasync_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XAsync")
    xsystem_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XSystem")
    xuser_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XUser")
    xgamesave_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XGameSave")
    xmem_cnt = count_exported_symbols(xgr_hdr, "SCARLETTX_XGR_API", "XMem") + count_exported_symbols(
        xgr_hdr, "SCARLETTX_XGR_API", "XThread"
    )

    d3d_cnt = (
        count_exported_symbols(d3d_hdr, "SCARLETTX_D3D12_API", "D3D12")
        + count_exported_symbols(xg_hdr, "SCARLETTX_XG_API", "XG")
    )
    bvh_cnt = count_exported_symbols(d3d_hdr, "SCARLETTX_BVH_API", "XboxBvh")
    ds_cnt = count_exported_symbols(ds_hdr, "SCARLETTX_DSTORAGE_API", "DStorage")
    xa_cnt = (
        count_exported_symbols(xa_hdr, "SCARLETTX_XAUDIO2_API", "XAudio2")
        + count_exported_symbols(xa_hdr, "SCARLETTX_XAUDIO2_API", "Xma2")
        + count_exported_symbols(acp_hdr, "SCARLETTX_ACP_API", "Apu")
        + count_exported_symbols(acp_hdr, "SCARLETTX_ACP_API", "AcpHal")
    )
    gi_cnt = count_exported_symbols(gi_hdr, "SCARLETTX_GAMEINPUT_API", "GameInput")
    bink_cnt = count_exported_symbols(bink_hdr, "SCARLETTX_BINK2_API", "Bink")
    ffx_cnt = count_exported_symbols(ffx_hdr, "SCARLETTX_FFX_LOADER_API", "ffx") + count_exported_symbols(
        ffx_hdr, "SCARLETTX_FFX_UPSCALER_API", "ffx"
    )
    hc_cnt = count_exported_symbols(hc_hdr, "SCARLETTX_HC_API", "HC")
    curl_cnt = count_exported_symbols(curl_hdr, "SCARLETTX_XCURL_API", "curl_")

    total_exports = (
        xruntime_cnt
        + xtask_cnt
        + xasync_cnt
        + xsystem_cnt
        + xuser_cnt
        + xgamesave_cnt
        + xmem_cnt
        + d3d_cnt
        + bvh_cnt
        + ds_cnt
        + xa_cnt
        + gi_cnt
        + bink_cnt
        + ffx_cnt
        + hc_cnt
        + curl_cnt
    )

    loader_features = sum(
        1
        for rel in [
            "src/loader/game_config.cpp",
            "src/loader/pe_loader.cpp",
            "src/loader/pe_mapper.cpp",
            "src/loader/stub_thunk.cpp",
            "src/loader/package_vfs.cpp",
            "src/loader/xvd_container.cpp",
        ]
        if (ROOT / rel).exists()
    )

    shader_ir_ready = 1 if (ROOT / "src/gpu/shader_ir.cpp").exists() else 0

    subsystems = [
        Subsystem(
            name="d3d12_xs → PC D3D12 & PSOs",
            short_name="d3d12",
            category="Graphics",
            implemented=d3d_cnt + shader_ir_ready,
            target=46,
            notes=(
                f"{d3d_cnt} exports + DXIL/DXBC container parser & Xbox-chunk stripper; "
                "root signatures, descriptor heaps, graphics/compute/software-DXR PSOs, pipeline cache, WARP or hardware adapter"
            ),
        ),
        Subsystem(
            name="XTaskQueue & XAsync",
            short_name="XTask",
            category="xgameruntime",
            implemented=xtask_cnt + xasync_cnt,
            target=24,
            notes=f"{xtask_cnt} XTaskQueue + {xasync_cnt} XAsync exports (manual/threadpool/delayed/monitors/cancel)",
        ),
        Subsystem(
            name="XGameSave (persistent)",
            short_name="Save",
            category="xgameruntime",
            implemented=xgamesave_cnt,
            target=22,
            notes=f"{xgamesave_cnt} exports; disk-backed containers, sync/async blob R/W, quota, delete",
        ),
        Subsystem(
            name="XUser & Identity",
            short_name="XUser",
            category="xgameruntime",
            implemented=xuser_cnt,
            target=24,
            notes=f"{xuser_cnt} exports; synthetic Xbox Live profile, XUID, gamertag components, privileges",
        ),
        Subsystem(
            name="XSystem & Runtime",
            short_name="XSys",
            category="xgameruntime",
            implemented=xruntime_cnt + xsystem_cnt,
            target=18,
            notes=f"{xruntime_cnt + xsystem_cnt} exports; Scarlett Anaconda device info, runtime/analytics, handle validation",
        ),
        Subsystem(
            name="XMem & Threading",
            short_name="XMem",
            category="xgameruntime",
            implemented=xmem_cnt,
            target=16,
            notes=f"{xmem_cnt} exports; default/custom alloc hooks, VirtualAlloc/Free, time-sensitive thread state",
        ),
        Subsystem(
            name="DXR / XboxBvhBuilder",
            short_name="DXR",
            category="Graphics",
            implemented=bvh_cnt + 4,
            target=14,
            notes=(
                f"{bvh_cnt} BVH exports + DXR AS prebuild/build/destroy, BLAS update, single-ray trace, "
                "deterministic software BVH fallback (hardware DXR requires capable GPU)"
            ),
        ),
        Subsystem(
            name="DirectStorage & XVD",
            short_name="DStor",
            category="Storage",
            implemented=ds_cnt + (1 if (ROOT / "src/loader/xvd_container.cpp").exists() else 0),
            target=24,
            notes=(
                f"{ds_cnt} DStorage exports + unencrypted XVD parser/PackageVfs mounting; "
                "async worker queues, fence/status arrays, cancellation, synthetic LZ-RLE test codec"
            ),
        ),
        Subsystem(
            name="Audio (XAudio2 / AcpHal)",
            short_name="Audio",
            category="Media",
            implemented=xa_cnt,
            target=32,
            notes=(
                f"{xa_cnt} exports (XAudio2_9 ordinal #1 + AcpHal.dll APU/SHAPE shims); "
                "headless PCM mixer, WAV writer, fail-closed real-title COM guard, synthetic XMA2 test decoder"
            ),
        ),
        Subsystem(
            name="Input (GameInput)",
            short_name="Input",
            category="Input",
            implemented=gi_cnt,
            target=16,
            notes=(
                f"{gi_cnt} exports; virtual Xbox Series controller & keyboard, reading history "
                "(Current/Next/Previous), state injection, 4-motor rumble state (stored, not sent to the host controller)"
            ),
        ),
        Subsystem(
            name="Middleware (Bink2 / FSR)",
            short_name="Midw",
            category="Middleware",
            implemented=bink_cnt + ffx_cnt,
            target=34,
            notes=(
                f"{bink_cnt} Bink2Scarlett + {ffx_cnt} amd_fidelityfx_loader/upscaler_xbox exports; "
                "deterministic offline HLE shims (synthetic RGBA/BGRA frames, bilinear upscale & hash)"
            ),
        ),
        Subsystem(
            name="Networking (HTTP / XCurl)",
            short_name="Net",
            category="Networking",
            implemented=hc_cnt + curl_cnt,
            target=40,
            notes=(
                f"{hc_cnt} libHttpClient.GDK + {curl_cnt} XCurl exports; "
                "deterministic offline mock registry, async callbacks, headers, fallback responses"
            ),
        ),
        Subsystem(
            name="PE64 Loader & Auto-Stubs",
            short_name="PE64",
            category="Core",
            implemented=loader_features,
            target=6,
            notes="PE32+ mapping, DIR64/HIGHLOW relocs, named/ordinal IAT, API-set redirection, x64 auto-stub thunks",
        ),
        Subsystem(
            name="Xbox Unified Memory & KernelX",
            short_name="Kern",
            category="Platform",
            implemented=0,
            target=14,
            notes="MEM_GRAPHICS, 64 KB / 4 MB physical-virtual aliasing, full Game OS kernel services",
        ),
        Subsystem(
            name="RDNA2 XS2 Shader Lifting",
            short_name="XS2",
            category="Graphics",
            implemented=0,
            target=16,
            notes="Precompiled RDNA2/Scarlett GPU microcode lifting to PC DXIL or SPIR-V",
        ),
        Subsystem(
            name="Retail XVC/XVD Crypto",
            short_name="Crypt",
            category="Storage",
            implemented=0,
            target=12,
            notes="Encrypted & digitally signed retail .xvc/.xvd package decryption and verification",
        ),
        Subsystem(
            name="Production Codecs (XMA2/GDeflate/Bink)",
            short_name="Codec",
            category="Media",
            implemented=0,
            target=16,
            notes="Native SHAPE XMA2 transform audio, hardware GDeflate/BCPack, production Bink2 & GPU FSR",
        ),
        Subsystem(
            name="Hardware DXR & Vulkan Backend",
            short_name="Vulk",
            category="Graphics",
            implemented=0,
            target=14,
            notes="Hardware GPU ray tracing (DXR Tier 1.1) and cross-platform Vulkan / vkd3d-proton backend",
        ),
        Subsystem(
            name="Live Xbox Services (XSAPI/WinRT)",
            short_name="XSAPI",
            category="Platform",
            implemented=0,
            target=16,
            notes="Full Xbl* Xbox Live services, WinRT runtime activation, achievements, multiplayer",
        ),
    ]

    return subsystems, total_exports, test_count


def collect_concept_destruction_metrics() -> list[Subsystem]:
    """Verified real-title bring-up progress for Concept Destruction (Scarlett Gen9 Unity IL2CPP)."""
    return [
        Subsystem(
            name="PE64 Mapping & Imports",
            short_name="PE64",
            category="Loader",
            implemented=4,
            target=4,
            notes="Manifest parsed, PE64 mapped (405 imports), static Xbox DLLs resolved, G:\\ package mount",
        ),
        Subsystem(
            name="xgameruntime & QueryApiImpl",
            short_name="XGR",
            category="Runtime",
            implemented=6,
            target=6,
            notes="Every QueryApiImpl table the title requests resolves (task queues, XNetworking, XGame, XSystem, XUser, XGameSave); XUserGetTokenAndSignatureAsync still E_NOTIMPL",
        ),
        Subsystem(
            name="d3d12_xs Device & Resources",
            short_name="Dev",
            category="Graphics",
            implemented=7,
            target=8,
            notes="Device, queues, fences, descriptor heaps, root sigs, PSOs, 1080p RTs, CreateHeap/CreatePlacedResource, guest texture memory; one device slot unresolved",
        ),
        Subsystem(
            name="xg_xs & AcpHal Shims",
            short_name="XG/APU",
            category="Platform",
            implemented=3,
            target=4,
            notes="Console texture layout (4KB_S addressing) checked against the console xg_xs.dll, AcpHal shape-context allocation; rest of AcpHal unverified",
        ),
        Subsystem(
            name="d3d12_xs CmdList & Uploads",
            short_name="CmdL",
            category="Graphics",
            implemented=8,
            target=10,
            notes="Command lists run through gameplay with no Close failure or device removal; slot 83 handled as a texture-region copy (name unverified), slot 67 still E_NOTIMPL",
        ),
        Subsystem(
            name="Presentation & First Frame",
            short_name="Pres",
            category="Graphics",
            implemented=4,
            target=5,
            notes="PresentX window on its own thread, real 1920x1080 frames; on a hardware GPU about 105 fps at 2,400 draws per frame; the title's frame interval is ignored (uncapped) and Alt+Enter fullscreen freezes the picture",
        ),
        Subsystem(
            name="Audio & Input (Runtime)",
            short_name="Aud/In",
            category="Media",
            implemented=3,
            target=4,
            notes="Guest-facing XAudio2 objects forwarding to the host XAudio2 2.9, sound audible in play; GameInput polling verified for A, B, D-pad, left stick, right trigger, and with a real Xbox controller; rumble is not forwarded to the controller",
        ),
        Subsystem(
            name="In-Game & Playable",
            short_name="Game",
            category="Gameplay",
            implemented=5,
            target=5,
            notes="Logos, intro cutscene, menus, events with driving and collisions through to the results screen, at full speed on a hardware GPU, with keyboard or an Xbox controller; a championship in progress continues after a restart",
        ),
    ]


def _badge_width(text: str) -> int:
    # Approximate Verdana 11px width + 12px horizontal padding
    return max(26, int(round(len(text) * 6.4)) + 14)


def render_svg(
    subsystems: list[Subsystem],
    badges: list[tuple[str, str, str, bool]],
    heading_prefix: str = "Status",
    callout_lead: str = "Not started:",
    callout_line1: str = "Xbox unified memory/KernelX, RDNA2 XS2 lifting, retail XVC/XVD crypto, native XMA2/GDeflate/Bink,",
    callout_line2: str = "hardware DXR/Vulkan, and live XSAPI/WinRT. Synthetic offline shims do not imply retail compatibility.",
) -> str:
    W = 900
    PAD_X = 36
    PAD_Y = 30
    INNER_W = W - 2 * PAD_X  # 828px

    total_impl = sum(s.implemented for s in subsystems)
    total_target = sum(s.target for s in subsystems)
    overall_pct = (total_impl / total_target * 100.0) if total_target else 0.0

    # Contribution graph geometry (7 rows, column-by-column, 14px squares, 3px gaps, min 2 cols)
    ROWS = 7
    CELL = 14
    CELL_GAP = 3
    GROUP_GAP = 10
    BOX_PAD_X = 22
    BOX_PAD_TOP = 20
    BOX_PAD_BOT = 16
    LABEL_H = 14
    LABEL_GAP = 6

    groups: list[tuple[Subsystem, int, int]] = []
    for s in subsystems:
        cols = max(2, math.ceil(s.target / ROWS))
        gw = cols * CELL + (cols - 1) * CELL_GAP
        groups.append((s, cols, gw))

    # If the sum of group widths exceeds the inner graph box width (784px), scale cell size slightly
    # so every square remains crisp and centered without overflowing the 900px SVG.
    avail_graph_w = INNER_W - 2 * BOX_PAD_X
    raw_graph_w = sum(gw for _, _, gw in groups) + GROUP_GAP * max(0, len(groups) - 1)
    if raw_graph_w > avail_graph_w:
        scale = avail_graph_w / raw_graph_w
        CELL = max(7, int(math.floor(CELL * scale)))
        CELL_GAP = max(2, int(math.floor(CELL_GAP * scale)))
        GROUP_GAP = max(5, int(math.floor(GROUP_GAP * scale)))
        groups = []
        for s in subsystems:
            cols = max(2, math.ceil(s.target / ROWS))
            gw = cols * CELL + (cols - 1) * CELL_GAP
            groups.append((s, cols, gw))
        raw_graph_w = sum(gw for _, _, gw in groups) + GROUP_GAP * max(0, len(groups) - 1)

    grid_h = ROWS * CELL + (ROWS - 1) * CELL_GAP
    legend_gap = 14
    legend_h = 16
    graph_box_h = BOX_PAD_TOP + LABEL_H + LABEL_GAP + grid_h + legend_gap + legend_h + BOX_PAD_BOT

    # Vertical flow (16px section spacing)
    y_heading_baseline = PAD_Y + 26
    y_heading_rule = y_heading_baseline + 10  # ~8px gap above 1px underline
    y_badges = y_heading_rule + 16
    y_graph_box = y_badges + 20 + 16
    y_list = y_graph_box + graph_box_h + 16

    sorted_subs = sorted(subsystems, key=lambda s: (-s.ratio, -s.implemented, s.name.lower()))
    list_rows_per_col = math.ceil(len(sorted_subs) / 2)
    ROW_H = 30
    list_h = list_rows_per_col * ROW_H

    has_callout = bool(callout_line1)
    callout_h = 44 if has_callout else 0
    y_callout = y_list + list_h + (16 if has_callout else 0)
    H = (y_callout + callout_h if has_callout else y_list + list_h) + PAD_Y

    lines: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" '
        f'aria-label="{html.escape(heading_prefix)}: {overall_pct:.1f}%">',
        "  <style>",
        "    text { font-family: 'Mona Sans', -apple-system, 'Segoe UI', 'Noto Sans', Helvetica, Arial, sans-serif; }",
        f"    .h2 {{ font-size: 26px; font-weight: 600; fill: {FG}; }}",
        "    .badge-txt { font-family: Verdana, 'DejaVu Sans', Geneva, sans-serif; font-size: 11px; fill: #ffffff; }",
        f"    .badge-txt-dark {{ font-family: Verdana, 'DejaVu Sans', Geneva, sans-serif; font-size: 11px; fill: {BG}; }}",
        f"    .g-label {{ font-size: 10px; fill: {FG_MUTED}; }}",
        f"    .legend-txt {{ font-size: 12px; fill: {FG_MUTED}; }}",
        f"    .r-name {{ font-size: 13px; fill: {FG}; }}",
        f"    .r-count {{ font-size: 13px; fill: {FG_MUTED}; font-variant-numeric: tabular-nums; }}",
        f"    .r-pct {{ font-size: 13px; font-weight: 600; fill: {FG}; font-variant-numeric: tabular-nums; }}",
        f"    .callout-txt {{ font-size: 13px; fill: {FG_SOFT}; }}",
        f"    .callout-lead {{ font-size: 13px; font-weight: 600; fill: {WARN}; }}",
        "  </style>",
        f'  <rect width="{W}" height="{H}" fill="{BG}"/>',
        "",
        "  <!-- 1. Heading -->",
        f'  <text class="h2" x="{PAD_X}" y="{y_heading_baseline}">{html.escape(heading_prefix)}: {overall_pct:.1f}%</text>',
        f'  <line x1="{PAD_X}" y1="{y_heading_rule}" x2="{W - PAD_X}" y2="{y_heading_rule}" stroke="{BORDER_STRONG}" stroke-width="1"/>',
        "",
        "  <!-- 2. Badge row -->",
    ]

    bx = PAD_X
    for idx, (k, v, v_bg, dark_v) in enumerate(badges):
        kw = _badge_width(k)
        vw = _badge_width(v)
        bw = kw + vw
        clip_id = f"bclip{idx}"
        lines.append(f'  <clipPath id="{clip_id}"><rect x="{bx}" y="{y_badges}" width="{bw}" height="20" rx="3"/></clipPath>')
        lines.append(f'  <g clip-path="url(#{clip_id})">')
        lines.append(f'    <rect x="{bx}" y="{y_badges}" width="{kw}" height="20" fill="{BADGE_LABEL}"/>')
        lines.append(f'    <rect x="{bx + kw}" y="{y_badges}" width="{vw}" height="20" fill="{v_bg}"/>')
        lines.append(
            f'    <text class="badge-txt" x="{bx + kw / 2:.1f}" y="{y_badges + 14}" text-anchor="middle">{html.escape(k)}</text>'
        )
        v_cls = "badge-txt-dark" if dark_v else "badge-txt"
        lines.append(
            f'    <text class="{v_cls}" x="{bx + kw + vw / 2:.1f}" y="{y_badges + 14}" text-anchor="middle">{html.escape(v)}</text>'
        )
        lines.append("  </g>")
        bx += bw + 6

    lines.append("")
    lines.append("  <!-- 3. Contribution graph box -->")
    lines.append(
        f'  <rect x="{PAD_X}" y="{y_graph_box}" width="{INNER_W}" height="{graph_box_h}" rx="6" '
        f'fill="{BG}" stroke="{BORDER}" stroke-width="1"/>'
    )

    gx = PAD_X + BOX_PAD_X + max(0.0, (avail_graph_w - raw_graph_w) / 2.0)
    gy_label = y_graph_box + BOX_PAD_TOP + 10
    gy_cells = y_graph_box + BOX_PAD_TOP + LABEL_H + LABEL_GAP

    for s, cols, gw in groups:
        fill_col = level_color(s.ratio, s.implemented)
        lines.append(f'  <text class="g-label" x="{gx:.1f}" y="{gy_label}">{html.escape(s.short_name)}</text>')
        for i in range(cols * ROWS):
            col = i // ROWS
            row = i % ROWS
            cx = gx + col * (CELL + CELL_GAP)
            cy = gy_cells + row * (CELL + CELL_GAP)
            if i < s.implemented:
                lines.append(f'  <rect x="{cx:.1f}" y="{cy:.1f}" width="{CELL}" height="{CELL}" rx="3" fill="{fill_col}"/>')
            elif i < s.target:
                lines.append(
                    f'  <rect x="{cx + 0.5:.1f}" y="{cy + 0.5:.1f}" width="{CELL - 1}" height="{CELL - 1}" rx="3" '
                    f'fill="{EMPTY_FILL}" stroke="{EMPTY_STROKE}" stroke-width="1"/>'
                )
        gx += gw + GROUP_GAP

    # Legend (right-aligned)
    ly = gy_cells + grid_h + legend_gap + 11
    rx_end = PAD_X + INNER_W - BOX_PAD_X
    lines.append(f'  <text class="legend-txt" x="{rx_end}" y="{ly}" text-anchor="end">Done</text>')
    sw_colors = [LV3, LV2, LV1, LV0, EMPTY_FILL]
    sx = rx_end - 36 - 11
    for col in sw_colors:
        if col == EMPTY_FILL:
            lines.append(
                f'  <rect x="{sx + 0.5:.1f}" y="{ly - 9.5:.1f}" width="10" height="10" rx="2" '
                f'fill="{EMPTY_FILL}" stroke="{EMPTY_STROKE}" stroke-width="1"/>'
            )
        else:
            lines.append(f'  <rect x="{sx:.1f}" y="{ly - 10:.1f}" width="11" height="11" rx="2" fill="{col}"/>')
        sx -= 15
    lines.append(f'  <text class="legend-txt" x="{sx + 11}" y="{ly}" text-anchor="end">Not yet</text>')

    lines.append("")
    lines.append("  <!-- 4. Two-column subsystem list -->")
    COL_GAP = 40
    LIST_PAD = 4
    col_w = (INNER_W - 2 * LIST_PAD - COL_GAP) / 2.0
    for idx, s in enumerate(sorted_subs):
        col_i = idx % 2
        row_i = idx // 2
        x0 = PAD_X + LIST_PAD + col_i * (col_w + COL_GAP)
        y_top = y_list + row_i * ROW_H
        y_mid = y_top + 19
        dot_fill = level_color(s.ratio, s.implemented)
        if s.implemented == 0:
            lines.append(
                f'  <rect x="{x0 + 0.5:.1f}" y="{y_top + 9.5:.1f}" width="10" height="10" rx="2" '
                f'fill="{EMPTY_FILL}" stroke="{EMPTY_STROKE}" stroke-width="1"/>'
            )
        else:
            lines.append(f'  <rect x="{x0:.1f}" y="{y_top + 9:.1f}" width="11" height="11" rx="2" fill="{dot_fill}"/>')
        lines.append(f'  <text class="r-name" x="{x0 + 21:.1f}" y="{y_mid}">{html.escape(s.name)}</text>')
        lines.append(
            f'  <text class="r-count" x="{x0 + col_w - 48:.1f}" y="{y_mid}" text-anchor="end">'
            f'{s.implemented} of {s.target}</text>'
        )
        lines.append(
            f'  <text class="r-pct" x="{x0 + col_w:.1f}" y="{y_mid}" text-anchor="end">{round(s.pct)}%</text>'
        )
        lines.append(
            f'  <line x1="{x0:.1f}" y1="{y_top + ROW_H}" x2="{x0 + col_w:.1f}" y2="{y_top + ROW_H}" '
            f'stroke="{BORDER_MUTED}" stroke-width="1"/>'
        )

    if has_callout:
        lines.append("")
        lines.append("  <!-- 5. Warning callout -->")
        lines.append(f'  <rect x="{PAD_X}" y="{y_callout}" width="4" height="{callout_h}" fill="{WARN}"/>')
        ix = PAD_X + 20
        iy = y_callout + 10
        lines.append(
            f'  <g transform="translate({ix},{iy})">'
            f'<path d="M8 1.5 15 14H1z" fill="none" stroke="{WARN}" stroke-width="1.5" stroke-linejoin="round"/>'
            f'<path d="M8 6v3.5M8 11.6v.4" stroke="{WARN}" stroke-width="1.5" stroke-linecap="round"/>'
            f"</g>"
        )
        tx = ix + 26
        lines.append(
            f'  <text class="callout-txt" x="{tx}" y="{y_callout + 19}">'
            f'<tspan class="callout-lead">{html.escape(callout_lead)}</tspan> '
            f'{html.escape(callout_line1)}</text>'
        )
        if callout_line2:
            lines.append(
                f'  <text class="callout-txt" x="{tx}" y="{y_callout + 36}">'
                f'{html.escape(callout_line2)}</text>'
            )

    lines.append("</svg>")
    return "\n".join(lines) + "\n"


def render_html(subsystems: list[Subsystem], total_exports: int, tests_text: str, tests_ok: bool) -> str:
    total_impl = sum(s.implemented for s in subsystems)
    total_target = sum(s.target for s in subsystems)
    overall_pct = (total_impl / total_target * 100.0) if total_target else 0.0

    js_rows = []
    for s in subsystems:
        js_rows.append(
            f"    [{s.short_name!r}, {s.name!r}, {s.implemented}, {s.target}],"
        )
    js_array = "\n".join(js_rows)

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ScarlettX — Status</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Mona+Sans:wght@400;500;600;700&display=swap">
<style>
  :root {{
    --bg: {BG};
    --border: {BORDER};
    --border-strong: {BORDER_STRONG};
    --border-muted: {BORDER_MUTED};
    --fg: {FG};
    --fg-muted: {FG_MUTED};
    --fg-soft: {FG_SOFT};
    --empty: {EMPTY_FILL};
    --empty-ring: rgba(240, 246, 252, 0.08);
    --lv0: {LV0};
    --lv1: {LV1};
    --lv2: {LV2};
    --lv3: {LV3};
    --warn: {WARN};
    --badge-label: {BADGE_LABEL};
    --badge-exports: {BADGE_EXPORTS};
    --badge-tests: {BADGE_TESTS};
    --badge-target: {BADGE_TARGET};
    --cell: 14px;
    --cell-gap: 3px;
    --radius: 6px;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0;
    background: var(--bg);
    color: var(--fg);
    font-family: 'Mona Sans', -apple-system, 'Segoe UI', 'Noto Sans', Helvetica, Arial, sans-serif;
  }}
  .status {{
    width: 900px;
    max-width: 100%;
    margin: 0 auto;
    padding: 30px 36px;
    display: flex;
    flex-direction: column;
    gap: 16px;
  }}
  h2 {{
    margin: 0;
    font-size: 26px;
    font-weight: 600;
    line-height: 1.25;
    padding-bottom: 8px;
    border-bottom: 1px solid var(--border-strong);
  }}
  .badges {{ display: flex; flex-wrap: wrap; gap: 6px; font-family: Verdana, 'DejaVu Sans', Geneva, sans-serif; font-size: 11px; line-height: 20px; }}
  .badge {{ display: flex; border-radius: 3px; overflow: hidden; }}
  .badge span {{ padding: 0 6px; color: #fff; }}
  .badge .k {{ background: var(--badge-label); }}
  .badge .v-fn {{ background: var(--lv2); color: var(--bg); }}
  .badge .v-exports {{ background: var(--badge-exports); }}
  .badge .v-tests {{ background: var(--badge-tests); }}
  .badge .v-tests-warn {{ background: var(--warn); }}
  .badge .v-target {{ background: var(--badge-target); }}
  .graph-box {{
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 20px 22px 16px;
    display: flex;
    flex-direction: column;
    gap: 14px;
    overflow-x: auto;
  }}
  .graph {{ display: flex; gap: 10px; justify-content: center; }}
  .group {{ display: flex; flex-direction: column; gap: 6px; }}
  .group-label {{ font-size: 11px; color: var(--fg-muted); white-space: nowrap; }}
  .cells {{
    display: grid;
    grid-template-rows: repeat(7, var(--cell));
    grid-auto-flow: column;
    grid-auto-columns: var(--cell);
    gap: var(--cell-gap);
  }}
  .cell {{ display: block; width: var(--cell); height: var(--cell); border-radius: 3px; }}
  .cell.empty, .sw.empty, .dot.empty {{ background: var(--empty); outline: 1px solid var(--empty-ring); outline-offset: -1px; }}
  .cell.pad {{ background: transparent; }}
  .lv0 {{ background: var(--lv0); }}
  .lv1 {{ background: var(--lv1); }}
  .lv2 {{ background: var(--lv2); }}
  .lv3 {{ background: var(--lv3); }}
  .legend {{ display: flex; justify-content: flex-end; align-items: center; gap: 4px; font-size: 12px; color: var(--fg-muted); }}
  .sw {{ display: block; width: 11px; height: 11px; border-radius: 2px; }}
  .list {{ display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 0 40px; padding: 0 4px; }}
  .row {{ display: flex; align-items: center; gap: 10px; height: 30px; border-bottom: 1px solid var(--border-muted); font-size: 13px; }}
  .dot {{ width: 11px; height: 11px; border-radius: 2px; flex-shrink: 0; }}
  .row .name {{ flex: 1; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }}
  .row .count {{ color: var(--fg-muted); font-variant-numeric: tabular-nums; }}
  .row .pct {{ width: 40px; text-align: right; font-weight: 600; font-variant-numeric: tabular-nums; }}
  .callout {{ display: flex; gap: 10px; padding: 8px 16px; border-left: 4px solid var(--warn); color: var(--fg-soft); font-size: 13px; }}
  .callout svg {{ flex-shrink: 0; margin-top: 2px; }}
  .callout strong {{ color: var(--warn); }}
  @media (max-width: 640px) {{
    .status {{ padding: 20px 16px; }}
    .graph {{ justify-content: flex-start; }}
    .list {{ grid-template-columns: minmax(0, 1fr); }}
  }}
</style>
</head>
<body>
<main class="status">
  <h2>Status: {overall_pct:.1f}%</h2>
  <div class="badges">
    <span class="badge"><span class="k">functions</span><span class="v-fn">{total_impl}/{total_target}</span></span>
    <span class="badge"><span class="k">exports</span><span class="v-exports">{total_exports}</span></span>
    <span class="badge"><span class="k">tests</span><span class="{'v-tests' if tests_ok else 'v-tests-warn'}">{tests_text}</span></span>
    <span class="badge"><span class="k">target</span><span class="v-target">Xbox Series X|S (Gen9)</span></span>
  </div>
  <section class="graph-box" aria-label="Implemented functions per subsystem, one square per function">
    <div class="graph" id="graph"></div>
    <div class="legend">Not yet
      <span class="sw empty"></span><span class="sw lv0"></span><span class="sw lv1"></span><span class="sw lv2"></span><span class="sw lv3"></span>
    Done</div>
  </section>
  <div class="list" id="list"></div>
  <div class="callout">
    <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true"><path d="M8 1.5 15 14H1z" fill="none" stroke="{WARN}" stroke-width="1.5" stroke-linejoin="round"/><path d="M8 6v3.5M8 11.6v.4" stroke="{WARN}" stroke-width="1.5" stroke-linecap="round"/></svg>
    <span><strong>Not started:</strong> Xbox unified memory/KernelX, RDNA2 XS2 lifting, retail XVC/XVD crypto, native XMA2/GDeflate/Bink, hardware DXR/Vulkan, and live XSAPI/WinRT.</span>
  </div>
</main>
<script>
  const SUBSYSTEMS = [
{js_array}
  ];
  const level = p => p >= 0.8 ? 'lv3' : p >= 0.6 ? 'lv2' : p >= 0.4 ? 'lv1' : 'lv0';
  const el = (tag, cls, text) => {{ const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }};
  const graph = document.getElementById('graph');
  for (const [label, , done, total] of SUBSYSTEMS) {{
    const g = el('div', 'group');
    g.append(el('span', 'group-label', label));
    const cells = el('div', 'cells');
    const cols = Math.max(2, Math.ceil(total / 7));
    const lv = level(done / total);
    for (let i = 0; i < cols * 7; i++) {{
      cells.append(el('span', 'cell ' + (i < done ? lv : i < total ? 'empty' : 'pad')));
    }}
    g.append(cells);
    graph.append(g);
  }}
  const list = document.getElementById('list');
  [...SUBSYSTEMS].sort((a, b) => b[2] / b[3] - a[2] / a[3]).forEach(([, name, done, total]) => {{
    const p = done / total;
    const row = el('div', 'row');
    row.append(el('span', 'dot ' + (done === 0 ? 'empty' : level(p))));
    row.append(el('span', 'name', name));
    row.append(el('span', 'count', `${{done}} of ${{total}}`));
    row.append(el('span', 'pct', `${{Math.round(p * 100)}}%`));
    list.append(row);
  }});
</script>
</body>
</html>
"""


def render_markdown_status(
    subsystems: list[Subsystem],
    cd_subsystems: list[Subsystem],
    total_exports: int,
    tests_md: str,
) -> str:
    total_impl = sum(s.implemented for s in subsystems)
    total_target = sum(s.target for s in subsystems)
    overall_pct = (total_impl / total_target * 100.0) if total_target else 0.0

    cd_impl = sum(s.implemented for s in cd_subsystems)
    cd_target = sum(s.target for s in cd_subsystems)
    cd_pct = (cd_impl / cd_target * 100.0) if cd_target else 0.0

    sorted_subs = sorted(subsystems, key=lambda s: (-s.ratio, -s.implemented, s.name.lower()))
    rows = [
        "<!-- SCARLETTX_STATUS_START -->",
        "## Status",
        "",
        "![ScarlettX Status](docs/status.svg)",
        "",
        "![Status of the game Concept Destruction](docs/status_concept_destruction.svg)",
        "",
    ]
    shots = [shot for shot in SCREENSHOTS if (ROOT / shot[0]).exists()]
    if shots:
        rows += [
            "| " + " | ".join(title for _, title, _, _ in shots) + " |",
            "|" + " :---: |" * len(shots),
            "| " + " | ".join(f"![{alt}]({path})" for path, _, alt, _ in shots) + " |",
        ]
        if any(caption for _, _, _, caption in shots):
            rows.append("| " + " | ".join(caption for _, _, _, caption in shots) + " |")
        rows.append("")
    rows += [
        f"**Emulator Status:** `{overall_pct:.1f}%` ({total_impl} of {total_target} tracked functions, including unimplemented `0%` subsystems) · "
        f"**Concept Destruction:** `{cd_pct:.1f}%` ({cd_impl} of {cd_target} bring-up milestones) · "
        f"**Exports:** `{total_exports}` · "
        f"**Tests:** {tests_md} · "
        "**Target:** `Xbox Series X|S (Gen9)`",
        "",
        "| Subsystem | Category | Progress | Coverage | Details |",
        "| :--- | :--- | :---: | :---: | :--- |",
    ]
    for s in sorted_subs:
        rows.append(
            f"| **{s.name}** | `{s.category}` | `{s.implemented} of {s.target}` | `{round(s.pct)}%` | {s.notes} |"
        )
    rows.append("")
    rows.append(
        "> **Not started (0%):** Xbox unified memory (`MEM_GRAPHICS`) & full KernelX, precompiled RDNA2 `XS2` ISA lifting, "
        "encrypted retail XVC/XVD decryption, production GDeflate/XMA2/Bink decoding, hardware DXR & Vulkan backend, "
        "and live XSAPI/WinRT services. Passing synthetic tests do not imply commercial retail title execution."
    )
    rows.append("")
    rows.append(
        "_Regenerate with `python tools/generate_status.py --build-dir <build tree>` after adding new HLE APIs or subsystems; "
        "the test numbers are read from that tree's last `ctest` run._"
    )
    rows.append("<!-- SCARLETTX_STATUS_END -->")
    return "\n".join(rows)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the ScarlettX status dashboard and README status section.")
    parser.add_argument(
        "--build-dir",
        type=pathlib.Path,
        default=ROOT / "build",
        help="build tree whose last CTest run supplies the test results (default: <repo>/build)",
    )
    args = parser.parse_args()

    subsystems, total_exports, test_count = collect_metrics()
    cd_subsystems = collect_concept_destruction_metrics()
    test_results = read_ctest_results(args.build_dir)
    tests_text = tests_label(test_count, test_results)
    tests_ok = test_results is not None and not test_results[1]

    total_impl = sum(s.implemented for s in subsystems)
    total_target = sum(s.target for s in subsystems)
    main_badges = [
        ("functions", f"{total_impl}/{total_target}", BADGE_FN, True),
        ("exports", str(total_exports), BADGE_EXPORTS, False),
        ("tests", tests_text, BADGE_TESTS if tests_ok else WARN, not tests_ok),
        ("target", "Xbox Series X|S (Gen9)", BADGE_TARGET, False),
    ]
    svg = render_svg(
        subsystems,
        badges=main_badges,
        heading_prefix="Status",
        callout_lead="Not started:",
        callout_line1="Xbox unified memory/KernelX, RDNA2 XS2 lifting, retail XVC/XVD crypto, native XMA2/GDeflate/Bink,",
        callout_line2="hardware DXR/Vulkan, and live XSAPI/WinRT. Synthetic offline shims do not imply retail compatibility.",
    )

    cd_impl = sum(s.implemented for s in cd_subsystems)
    cd_target = sum(s.target for s in cd_subsystems)
    cd_badges = [
        ("milestones", f"{cd_impl}/{cd_target}", BADGE_FN, True),
        ("phase", "In-game", BADGE_EXPORTS, False),
        ("state", "Full speed on GPU, with sound", BADGE_TARGET, False),
        ("target", "Concept Destruction (Gen9)", BADGE_TESTS, False),
    ]
    cd_svg = render_svg(
        cd_subsystems,
        badges=cd_badges,
        heading_prefix="Status of the game Concept Destruction",
        callout_lead="Open issues:",
        callout_line1="Alt+Enter fullscreen freezes the picture; frame pacing is uncapped; rumble is not sent",
        callout_line2="to the controller; CmdList slot 67 is unimplemented.",
    )

    html_doc = render_html(subsystems, total_exports, tests_text, tests_ok)

    out_svg = ROOT / "docs/status.svg"
    out_cd_svg = ROOT / "docs/status_concept_destruction.svg"
    out_html = ROOT / "docs/status.html"
    out_svg.parent.mkdir(parents=True, exist_ok=True)
    out_svg.write_text(svg, encoding="utf-8")
    out_cd_svg.write_text(cd_svg, encoding="utf-8")
    out_html.write_text(html_doc, encoding="utf-8")

    md_block = render_markdown_status(
        subsystems, cd_subsystems, total_exports, tests_markdown(test_count, test_results)
    )
    readme_path = ROOT / "README.md"
    readme = readme_path.read_text(encoding="utf-8")

    marker_pat = re.compile(
        r"<!-- SCARLETTX_STATUS_START -->.*?<!-- SCARLETTX_STATUS_END -->",
        re.DOTALL,
    )
    if marker_pat.search(readme):
        new_readme = marker_pat.sub(md_block, readme)
    else:
        anchor = "## Architecture Overview"
        if anchor in readme:
            new_readme = readme.replace(anchor, md_block + "\n\n" + anchor, 1)
        else:
            new_readme = readme + "\n\n" + md_block + "\n"

    readme_path.write_text(new_readme, encoding="utf-8")
    print(
        f"[generate_status] Wrote {out_svg.relative_to(ROOT)}, {out_cd_svg.relative_to(ROOT)}, "
        f"{out_html.relative_to(ROOT)}, and updated README.md (exports={total_exports}, tests: {tests_text})"
    )


if __name__ == "__main__":
    main()
