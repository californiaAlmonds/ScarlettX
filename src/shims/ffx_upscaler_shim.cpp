#define SCARLETTX_BUILDING_FFX_UPSCALER 1
#include "scarlettx/middleware/ffx_abi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

struct FfxUpscalerContextState {
    uint32_t max_render_w = 0;
    uint32_t max_render_h = 0;
    uint32_t max_upscale_w = 0;
    uint32_t max_upscale_h = 0;
    float sharpness = 0.0f;
    bool enable_sharpening = false;
    uint64_t dispatch_count = 0;
};

std::mutex g_mutex;
std::unordered_map<void*, std::unique_ptr<FfxUpscalerContextState>> g_contexts;

float QualityModeToRatio(uint32_t mode) {
    switch (mode) {
        case FFX_UPSCALE_QUALITY_MODE_NATIVEAA:          return 1.0f;
        case FFX_UPSCALE_QUALITY_MODE_QUALITY:           return 1.5f;
        case FFX_UPSCALE_QUALITY_MODE_BALANCED:          return 1.7f;
        case FFX_UPSCALE_QUALITY_MODE_PERFORMANCE:       return 2.0f;
        case FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE: return 3.0f;
        default:                                         return 1.5f;
    }
}

float Halton(int32_t index, int32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    int32_t i = index;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

uint64_t Fnv1aBytes(const uint8_t* data, size_t size) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace

extern "C" {

ffxReturnCode_t ffxUpscalerCreateContext(ffxContext* context, ffxApiHeader* desc) {
    if (!context || !desc || desc->type != FFX_API_DESC_TYPE_UPSCALE_CREATE) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    const auto* cd = reinterpret_cast<const ffxCreateContextDescUpscale*>(desc);
    if (cd->maxRenderWidth == 0 || cd->maxRenderHeight == 0 ||
        cd->maxUpscaleWidth == 0 || cd->maxUpscaleHeight == 0) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    auto state = std::make_unique<FfxUpscalerContextState>();
    state->max_render_w = cd->maxRenderWidth;
    state->max_render_h = cd->maxRenderHeight;
    state->max_upscale_w = cd->maxUpscaleWidth;
    state->max_upscale_h = cd->maxUpscaleHeight;
    void* raw = state.get();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_contexts[raw] = std::move(state);
    }
    *context = raw;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxUpscalerDestroyContext(ffxContext* context) {
    if (!context || !*context) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t erased = g_contexts.erase(*context);
    *context = nullptr;
    return erased > 0 ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_PARAMETER;
}

ffxReturnCode_t ffxUpscalerConfigure(ffxContext* context, const ffxApiHeader* desc) {
    if (!context || !*context || !desc) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_contexts.find(*context);
    if (it == g_contexts.end()) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (desc->type == FFX_API_DESC_TYPE_UPSCALE_CONFIGURE) {
        const auto* cfg = reinterpret_cast<const ffxConfigureDescUpscale*>(desc);
        it->second->sharpness = std::clamp(cfg->sharpness, 0.0f, 1.0f);
        it->second->enable_sharpening = (cfg->enableSharpening != 0);
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

ffxReturnCode_t ffxUpscalerQuery(ffxContext* context, ffxApiHeader* desc) {
    (void)context;
    if (!desc) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    switch (desc->type) {
        case FFX_API_DESC_TYPE_GET_PROVIDER_VERSION: {
            auto* q = reinterpret_cast<ffxQueryDescGetProviderVersion*>(desc);
            q->versionId = 0x00030102ull;  // FSR 3.1.2 HLE
            q->versionName = "ScarlettX FidelityFX Upscaler 3.1.2-HLE";
            return FFX_API_RETURN_OK;
        }
        case FFX_API_DESC_TYPE_UPSCALE_GET_UPSCALERATIO: {
            auto* q = reinterpret_cast<ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode*>(desc);
            if (!q->pOutUpscaleRatio) return FFX_API_RETURN_ERROR_PARAMETER;
            *q->pOutUpscaleRatio = QualityModeToRatio(q->qualityMode);
            return FFX_API_RETURN_OK;
        }
        case FFX_API_DESC_TYPE_UPSCALE_GET_RENDERRESOLUTION: {
            auto* q = reinterpret_cast<ffxQueryDescUpscaleGetRenderResolutionFromQualityMode*>(desc);
            if (!q->pOutRenderWidth || !q->pOutRenderHeight ||
                q->displayWidth == 0 || q->displayHeight == 0) {
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            const float ratio = QualityModeToRatio(q->qualityMode);
            *q->pOutRenderWidth = std::max(1u, static_cast<uint32_t>(std::round(q->displayWidth / ratio)));
            *q->pOutRenderHeight = std::max(1u, static_cast<uint32_t>(std::round(q->displayHeight / ratio)));
            return FFX_API_RETURN_OK;
        }
        case FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT: {
            auto* q = reinterpret_cast<ffxQueryDescUpscaleGetJitterPhaseCount*>(desc);
            if (!q->pOutPhaseCount || q->renderWidth <= 0 || q->displayWidth <= 0) {
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            const float ratio = static_cast<float>(q->displayWidth) / static_cast<float>(q->renderWidth);
            *q->pOutPhaseCount = static_cast<int32_t>(std::ceil(8.0f * ratio * ratio));
            return FFX_API_RETURN_OK;
        }
        case FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET: {
            auto* q = reinterpret_cast<ffxQueryDescUpscaleGetJitterOffset*>(desc);
            if (!q->pOutX || !q->pOutY || q->phaseCount <= 0) {
                return FFX_API_RETURN_ERROR_PARAMETER;
            }
            const int32_t idx = (q->index % q->phaseCount) + 1;
            *q->pOutX = Halton(idx, 2) - 0.5f;
            *q->pOutY = Halton(idx, 3) - 0.5f;
            return FFX_API_RETURN_OK;
        }
        default:
            return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

ffxReturnCode_t ffxUpscalerDispatch(ffxContext* context, const ffxApiHeader* desc) {
    if (!context || !*context || !desc || desc->type != FFX_API_DESC_TYPE_UPSCALE_DISPATCH) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    const auto* d = reinterpret_cast<const ffxDispatchDescUpscale*>(desc);
    if (!d->colorRgba8 || !d->outputRgba8 ||
        d->renderWidth == 0 || d->renderHeight == 0 ||
        d->upscaleWidth == 0 || d->upscaleHeight == 0) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_contexts.find(*context);
        if (it == g_contexts.end()) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        ++it->second->dispatch_count;
    }

    // Deterministic bilinear upscale from renderWidth x renderHeight to upscaleWidth x upscaleHeight
    for (uint32_t y = 0; y < d->upscaleHeight; ++y) {
        const float fy = (d->upscaleHeight > 1)
                             ? (static_cast<float>(y) * static_cast<float>(d->renderHeight - 1) /
                                static_cast<float>(d->upscaleHeight - 1))
                             : 0.0f;
        const uint32_t y0 = static_cast<uint32_t>(std::floor(fy));
        const uint32_t y1 = std::min(y0 + 1u, d->renderHeight - 1u);
        const float wy = fy - static_cast<float>(y0);

        for (uint32_t x = 0; x < d->upscaleWidth; ++x) {
            const float fx = (d->upscaleWidth > 1)
                                 ? (static_cast<float>(x) * static_cast<float>(d->renderWidth - 1) /
                                    static_cast<float>(d->upscaleWidth - 1))
                                 : 0.0f;
            const uint32_t x0 = static_cast<uint32_t>(std::floor(fx));
            const uint32_t x1 = std::min(x0 + 1u, d->renderWidth - 1u);
            const float wx = fx - static_cast<float>(x0);

            const uint8_t* p00 = d->colorRgba8 + (static_cast<size_t>(y0) * d->renderWidth + x0) * 4ull;
            const uint8_t* p10 = d->colorRgba8 + (static_cast<size_t>(y0) * d->renderWidth + x1) * 4ull;
            const uint8_t* p01 = d->colorRgba8 + (static_cast<size_t>(y1) * d->renderWidth + x0) * 4ull;
            const uint8_t* p11 = d->colorRgba8 + (static_cast<size_t>(y1) * d->renderWidth + x1) * 4ull;

            uint8_t* dst = d->outputRgba8 + (static_cast<size_t>(y) * d->upscaleWidth + x) * 4ull;
            for (int c = 0; c < 4; ++c) {
                const float top = p00[c] * (1.0f - wx) + p10[c] * wx;
                const float bot = p01[c] * (1.0f - wx) + p11[c] * wx;
                const float val = top * (1.0f - wy) + bot * wy;
                dst[c] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(val)), 0, 255));
            }
        }
    }

    if (d->outFrameHash) {
        *d->outFrameHash = Fnv1aBytes(
            d->outputRgba8,
            static_cast<size_t>(d->upscaleWidth) * d->upscaleHeight * 4ull);
    }
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxUpscalerGetVersion(uint64_t* outVersionId, const char** outVersionName) {
    if (!outVersionId && !outVersionName) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (outVersionId) {
        *outVersionId = 0x00030102ull;  // FSR 3.1.2 HLE
    }
    if (outVersionName) {
        *outVersionName = "ScarlettX FidelityFX Upscaler 3.1.2-HLE";
    }
    return FFX_API_RETURN_OK;
}

}  // extern "C"
