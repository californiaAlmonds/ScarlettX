#pragma once

#include <cstdint>

#if defined(_WIN32)
#if defined(SCARLETTX_BUILDING_BINK2)
#define SCARLETTX_BINK2_API __declspec(dllexport)
#else
#define SCARLETTX_BINK2_API __declspec(dllimport)
#endif
#else
#define SCARLETTX_BINK2_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BINK {
    uint32_t Width;
    uint32_t Height;
    uint32_t Frames;
    uint32_t FrameNum;       // 1-based current frame
    uint32_t LastFrameNum;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t ReadError;
    uint32_t OpenFlags;
    uint32_t Paused;
    uint32_t SoundOn;
    int32_t Volume;
    uint64_t InternalId;
} BINK;

typedef struct BINKSUMMARY {
    uint32_t Width;
    uint32_t Height;
    uint32_t TotalTime;
    uint32_t FileFrameRate;
    uint32_t FileFrameRateDiv;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t TotalOpenTime;
    uint32_t TotalFrames;
    uint32_t TotalPlayedFrames;
    uint32_t SkippedFrames;
} BINKSUMMARY;

// Surface pixel format flags matching Bink2 conventions
enum {
    BINKSURFACE32RGBA = 6,
    BINKSURFACE32BGRA = 3,
    BINKSURFACE24 = 1,
    BINKCOPYALL = 0x80000000u
};

SCARLETTX_BINK2_API BINK* BinkOpen(const char* name, uint32_t flags);
SCARLETTX_BINK2_API void BinkClose(BINK* bnk);
SCARLETTX_BINK2_API int32_t BinkDoFrame(BINK* bnk);
SCARLETTX_BINK2_API void BinkNextFrame(BINK* bnk);
SCARLETTX_BINK2_API int32_t BinkWait(BINK* bnk);
SCARLETTX_BINK2_API int32_t BinkCopyToBuffer(
    BINK* bnk,
    void* dest,
    int32_t dest_pitch,
    uint32_t dest_height,
    uint32_t dest_x,
    uint32_t dest_y,
    uint32_t flags);
SCARLETTX_BINK2_API int32_t BinkPause(BINK* bnk, int32_t pause);
SCARLETTX_BINK2_API void BinkGoto(BINK* bnk, uint32_t frame, int32_t flags);
SCARLETTX_BINK2_API void BinkSetSoundOnOff(BINK* bnk, int32_t onoff);
SCARLETTX_BINK2_API void BinkSetVolume(BINK* bnk, uint32_t track_id, int32_t volume);
SCARLETTX_BINK2_API void BinkGetSummary(BINK* bnk, BINKSUMMARY* sum);
SCARLETTX_BINK2_API const char* BinkGetError(void);

// Background IO control flags
enum {
    BINKBGIOSUSPEND = 1,
    BINKBGIORESUME = 2,
    BINKBGIOFORCE = 0x80000000u
};

typedef void* (*BINKMEMALLOC)(uint64_t bytes);
typedef void (*BINKMEMFREE)(void* ptr);

typedef struct BINK_OPEN_OPTIONS {
    uint64_t FileOffset;
    int64_t TotTracks;
    int32_t* TrackNums;
    uint32_t MaxBufferSize;
    uint32_t Reserved;
} BINK_OPEN_OPTIONS;

typedef struct BINKFRAMEPLANESET {
    void* YPlane;
    uint32_t YPitch;
    void* cRPlane;
    uint32_t cRPitch;
    void* cBPlane;
    uint32_t cBPitch;
    void* APlane;
    uint32_t APitch;
} BINKFRAMEPLANESET;

typedef struct BINKFRAMEBUFFERS {
    int32_t TotalFrames;
    uint32_t YABufferWidth;
    uint32_t YABufferHeight;
    uint32_t cRcBBufferWidth;
    uint32_t cRcBBufferHeight;
    uint32_t FrameNum;
    BINKFRAMEPLANESET Frames[2];
} BINKFRAMEBUFFERS;

typedef struct BINKREALTIME {
    uint32_t FrameNum;
    uint32_t FrameRate;
    uint32_t FrameRateDiv;
    uint32_t Frames;
    uint32_t FramesTime;
    uint32_t FramesVideoDecompTime;
    uint32_t FramesAudioDecompTime;
    uint32_t FramesReadTime;
    uint32_t FramesIdleReadTime;
    uint32_t FramesThreadReadTime;
    uint32_t FramesBlitTime;
    uint32_t ReadBufferSize;
    uint32_t ReadBufferUsed;
    uint32_t FramesDataRate;
} BINKREALTIME;

SCARLETTX_BINK2_API BINK* BinkOpenWithOptions(
    const char* name,
    const BINK_OPEN_OPTIONS* opts,
    uint32_t flags);
SCARLETTX_BINK2_API void BinkSetMemory(BINKMEMALLOC alloc_fn, BINKMEMFREE free_fn);
SCARLETTX_BINK2_API int32_t BinkControlBackgroundIO(BINK* bnk, uint32_t control);
SCARLETTX_BINK2_API int32_t BinkDoFrameAsync(BINK* bnk, uint32_t thread_one, uint32_t thread_two);
SCARLETTX_BINK2_API int32_t BinkDoFrameAsyncWait(BINK* bnk, int32_t us);
SCARLETTX_BINK2_API void BinkRegisterFrameBuffers(BINK* bnk, BINKFRAMEBUFFERS* set);
SCARLETTX_BINK2_API void BinkGetFrameBuffersInfo(BINK* bnk, BINKFRAMEBUFFERS* set);
SCARLETTX_BINK2_API void BinkSetFrameRate(uint32_t forcerate, uint32_t forceratediv);
SCARLETTX_BINK2_API void BinkGetRealtime(BINK* bnk, BINKREALTIME* run, uint32_t frames);
SCARLETTX_BINK2_API int32_t BinkShouldSkip(BINK* bnk);
SCARLETTX_BINK2_API void BinkService(BINK* bnk);

#ifdef __cplusplus
}
#endif
