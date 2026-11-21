#define SCARLETTX_BUILDING_FFX_LOADER 1
#include "scarlettx/middleware/ffx_abi.h"

#include <cstring>

namespace {

bool IsUpscaleDescType(uint64_t descType) {
    return descType == 0 || ((descType & 0xFFFF0000ull) == FFX_API_DESC_TYPE_UPSCALE_CREATE);
}

}  // namespace

extern "C" {

ffxReturnCode_t ffxCreateContext(
    ffxContext* context,
    ffxApiHeader* desc,
    const void* memCb) {
    (void)memCb;
    return ffxUpscalerCreateContext(context, desc);
}

ffxReturnCode_t ffxDestroyContext(
    ffxContext* context,
    const void* memCb) {
    (void)memCb;
    return ffxUpscalerDestroyContext(context);
}

ffxReturnCode_t ffxConfigure(
    ffxContext* context,
    const ffxApiHeader* desc) {
    return ffxUpscalerConfigure(context, desc);
}

ffxReturnCode_t ffxQuery(
    ffxContext* context,
    ffxApiHeader* desc) {
    return ffxUpscalerQuery(context, desc);
}

ffxReturnCode_t ffxDispatch(
    ffxContext* context,
    const ffxApiHeader* desc) {
    return ffxUpscalerDispatch(context, desc);
}

ffxReturnCode_t ffxGetVersion(uint64_t* outVersionId, const char** outVersionName) {
    return ffxUpscalerGetVersion(outVersionId, outVersionName);
}

ffxReturnCode_t ffxGetProviderCount(uint64_t descType, uint32_t* outCount) {
    if (!outCount) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (IsUpscaleDescType(descType)) {
        *outCount = 1u;
        return FFX_API_RETURN_OK;
    }
    *outCount = 0u;
    return FFX_API_RETURN_NO_PROVIDER;
}

ffxReturnCode_t ffxGetProviderInfo(
    uint64_t descType,
    uint32_t providerIndex,
    ffxProviderInfo* outInfo) {
    if (!outInfo) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (!IsUpscaleDescType(descType)) {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    if (providerIndex != 0u) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    std::memset(outInfo, 0, sizeof(*outInfo));
    uint64_t ver_id = 0;
    const char* ver_name = nullptr;
    const ffxReturnCode_t rc = ffxUpscalerGetVersion(&ver_id, &ver_name);
    if (rc != FFX_API_RETURN_OK) {
        return rc;
    }

    outInfo->providerId = 0x5358465352333132ull;  // "SXFSR312"
    outInfo->versionId = ver_id;
    outInfo->effectType = FFX_API_DESC_TYPE_UPSCALE_CREATE;
    std::strncpy(outInfo->name, "amd_fidelityfx_upscaler_xbox", sizeof(outInfo->name) - 1);
    if (ver_name) {
        std::strncpy(outInfo->versionName, ver_name, sizeof(outInfo->versionName) - 1);
    }
    return FFX_API_RETURN_OK;
}

}  // extern "C"
