#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_KERNELX)
#    define SCARLETTX_KERNELX_API __declspec(dllexport)
#  else
#    define SCARLETTX_KERNELX_API __declspec(dllimport)
#  endif
#  define SCARLETTX_KERNELX_CALL __stdcall
#else
#  define SCARLETTX_KERNELX_API __attribute__((visibility("default")))
#  define SCARLETTX_KERNELX_CALL
#endif

// Standard Windows & Xbox Game OS (KernelX) virtual/physical memory constants.
#ifndef MEM_COMMIT
#  define MEM_COMMIT 0x00001000u
#endif
#ifndef MEM_RESERVE
#  define MEM_RESERVE 0x00002000u
#endif
#ifndef MEM_DECOMMIT
#  define MEM_DECOMMIT 0x00004000u
#endif
#ifndef MEM_RELEASE
#  define MEM_RELEASE 0x00008000u
#endif
#ifndef MEM_RESET
#  define MEM_RESET 0x00080000u
#endif
#ifndef MEM_TOP_DOWN
#  define MEM_TOP_DOWN 0x00100000u
#endif
#ifndef MEM_GRAPHICS
#  define MEM_GRAPHICS 0x10000000u
#endif
#ifndef MEM_LARGE_PAGES
#  define MEM_LARGE_PAGES 0x20000000u
#endif
#ifndef MEM_TITLE
#  define MEM_TITLE 0x40000000u
#endif
#ifndef MEM_4MB_PAGES
#  define MEM_4MB_PAGES 0x80000000u
#endif

#ifndef PAGE_NOACCESS
#  define PAGE_NOACCESS 0x00000001u
#endif
#ifndef PAGE_READONLY
#  define PAGE_READONLY 0x00000002u
#endif
#ifndef PAGE_READWRITE
#  define PAGE_READWRITE 0x00000004u
#endif
#ifndef PAGE_WRITECOPY
#  define PAGE_WRITECOPY 0x00000008u
#endif
#ifndef PAGE_EXECUTE
#  define PAGE_EXECUTE 0x00000010u
#endif
#ifndef PAGE_EXECUTE_READ
#  define PAGE_EXECUTE_READ 0x00000020u
#endif
#ifndef PAGE_EXECUTE_READWRITE
#  define PAGE_EXECUTE_READWRITE 0x00000040u
#endif
#ifndef PAGE_GUARD
#  define PAGE_GUARD 0x00000100u
#endif
#ifndef PAGE_NOCACHE
#  define PAGE_NOCACHE 0x00000200u
#endif
#ifndef PAGE_WRITECOMBINE
#  define PAGE_WRITECOMBINE 0x00000400u
#endif
#ifndef PAGE_GPU_COHERENT
#  define PAGE_GPU_COHERENT 0x00010000u
#endif
#ifndef PAGE_GPU_READONLY
#  define PAGE_GPU_READONLY 0x00020000u
#endif

// KernelX GPU tile mapping flags (matching scarlettx::GpuTileMappingFlags).
#define KERNELX_TILE_MAP_NONE 0x00000000u
#define KERNELX_TILE_MAP_NULL 0x00000001u
#define KERNELX_TILE_MAP_READONLY 0x00000002u
#define KERNELX_TILE_MAP_REUSE_SINGLE_TILE 0x00000004u

// KernelX GPU tile pool creation flags (matching scarlettx::GpuTilePoolFlags).
#define KERNELX_TILE_POOL_NONE 0x00000000u
#define KERNELX_TILE_POOL_GRAPHICS 0x00000001u
#define KERNELX_TILE_POOL_LARGE_PAGES 0x00000002u
#define KERNELX_TILE_POOL_WRITE_COMBINED 0x00000004u

#ifdef __cplusplus
extern "C" {
#endif

typedef enum KERNELX_CONSOLE_TYPE {
    CONSOLE_TYPE_UNKNOWN = 0,
    CONSOLE_TYPE_XBOX_ONE = 1,
    CONSOLE_TYPE_XBOX_ONE_X = 2,
    CONSOLE_TYPE_XBOX_SCARLETT_LOCKHART = 3,
    CONSOLE_TYPE_XBOX_SCARLETT_ANACONDA = 4,
    CONSOLE_TYPE_XBOX_SCARLETT_DEVKIT = 5,
} KERNELX_CONSOLE_TYPE;

typedef struct TITLEMEMORYSTATUS {
    uint32_t dwLength;
    uint32_t dwReserved;
    uint64_t ullTotalMem;
    uint64_t ullAvailMem;
    uint64_t ullLegacyUsed;
    uint64_t ullLegacyPeak;
    uint64_t ullLegacyAvail;
    uint64_t ullTitleUsed;
    uint64_t ullTitleAvail;
} TITLEMEMORYSTATUS;

typedef struct TOOLINGMEMORYSTATUS {
    uint32_t dwLength;
    uint32_t dwReserved;
    uint64_t ullTotalMem;
    uint64_t ullAvailMem;
    uint64_t ullUsed;
    uint64_t ullPeak;
} TOOLINGMEMORYSTATUS;

typedef struct KERNELX_OS_VERSION {
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint16_t BuildNumber;
    uint16_t Revision;
} KERNELX_OS_VERSION;

// ---------------------------------------------------------------------------
// Scarlett Game OS (kernelx.dll) Memory & System Services
// ---------------------------------------------------------------------------
SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL TitleMemoryStatus(
    TITLEMEMORYSTATUS* lpBuffer);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL ToolingMemoryStatus(
    TOOLINGMEMORYSTATUS* lpBuffer);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL AllocateTitlePhysicalPages(
    void* hProcess,
    uint32_t flAllocationType,
    uint64_t* pNumberOfPages,
    uint64_t* pPageArray);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL FreeTitlePhysicalPages(
    void* hProcess,
    uint64_t numberOfPages,
    const uint64_t* pPageArray);

SCARLETTX_KERNELX_API void* SCARLETTX_KERNELX_CALL MapTitlePhysicalPages(
    void* virtualAddress,
    uint64_t numberOfPages,
    uint32_t flAllocationType,
    uint32_t flProtect,
    const uint64_t* pPageArray);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL UnmapTitlePhysicalPages(
    void* virtualAddress,
    uint64_t numberOfPages);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL QueryTitlePhysicalPages(
    const void* virtualAddress,
    uint64_t* outPhysicalId,
    uint64_t* outSizeBytes,
    uint32_t* outFlags);

SCARLETTX_KERNELX_API void* SCARLETTX_KERNELX_CALL VirtualAllocFromApp(
    void* baseAddress,
    size_t size,
    uint32_t allocationType,
    uint32_t protection);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL VirtualProtectFromApp(
    void* address,
    size_t size,
    uint32_t newProtection,
    uint32_t* oldProtection);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXVirtualFree(
    void* address,
    size_t size,
    uint32_t freeType);

SCARLETTX_KERNELX_API uint32_t SCARLETTX_KERNELX_CALL GetConsoleType(void);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL GetSystemOSVersion(
    KERNELX_OS_VERSION* outVersion);

// ---------------------------------------------------------------------------
// Scarlett GPU Tiled / Reserved Resource Pool C ABI
// ---------------------------------------------------------------------------
SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXCreateTilePool(
    uint32_t tileCount,
    uint32_t flags,
    uint64_t* outPoolId);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXDestroyTilePool(
    uint64_t poolId);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXReserveTiledResource(
    uint32_t widthTiles,
    uint32_t heightTiles,
    uint64_t* outResourceId,
    void** outVirtualBase);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXReleaseTiledResource(
    uint64_t resourceId);

SCARLETTX_KERNELX_API int32_t SCARLETTX_KERNELX_CALL KernelXMapTileRange(
    uint64_t resourceId,
    uint32_t tileX,
    uint32_t tileY,
    uint64_t poolId,
    uint32_t poolTileOffset,
    uint32_t tileCount,
    uint32_t flags);

#ifdef __cplusplus
}
#endif
