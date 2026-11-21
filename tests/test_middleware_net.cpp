#include "scarlettx/middleware/bink2_abi.h"
#include "scarlettx/middleware/ffx_abi.h"
#include "scarlettx/net/httpclient_abi.h"
#include "scarlettx/net/xcurl_abi.h"
#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_middleware_net] FAILED: " #cond                 \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

size_t CurlWriteToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* dst = static_cast<std::string*>(userdata);
    const size_t total = size * nmemb;
    dst->append(ptr, total);
    return total;
}

}  // namespace

int main() {
    // 1. Bink2Scarlett.dll
    {
        BINK* bnk = BinkOpen("intro_scarlett.bk2", 0);
        SX_CHECK(bnk != nullptr);
        SX_CHECK(bnk->Width == 320 && bnk->Height == 180);
        SX_CHECK(BinkWait(bnk) == 0);
        SX_CHECK(BinkDoFrame(bnk) == 0);

        std::vector<uint8_t> rgba(static_cast<size_t>(bnk->Width) * bnk->Height * 4ull, 0);
        SX_CHECK(BinkCopyToBuffer(
                     bnk, rgba.data(), static_cast<int32_t>(bnk->Width * 4u),
                     bnk->Height, 0, 0, BINKSURFACE32RGBA) == 0);
        SX_CHECK(rgba[3] == 255);

        BinkNextFrame(bnk);
        SX_CHECK(bnk->FrameNum == 2);
        SX_CHECK(BinkDoFrame(bnk) == 0);
        BinkGoto(bnk, 10, 0);
        SX_CHECK(bnk->FrameNum == 10);
        BinkSetSoundOnOff(bnk, 0);
        BinkSetVolume(bnk, 0, 16384);
        SX_CHECK(BinkPause(bnk, 1) == 1);
        SX_CHECK(BinkWait(bnk) == 1);

        BINKSUMMARY sum{};
        BinkGetSummary(bnk, &sum);
        SX_CHECK(sum.Width == 320 && sum.Height == 180);
        SX_CHECK(sum.TotalPlayedFrames == 2);
        BinkClose(bnk);
    }

    // 2. amd_fidelityfx_loader_xbox.dll -> amd_fidelityfx_upscaler_xbox.dll
    {
        ffxQueryDescGetProviderVersion ver{};
        ver.header.type = FFX_API_DESC_TYPE_GET_PROVIDER_VERSION;
        SX_CHECK(ffxQuery(nullptr, &ver.header) == FFX_API_RETURN_OK);
        SX_CHECK(ver.versionId != 0 && ver.versionName != nullptr);

        uint32_t rw = 0, rh = 0;
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode res_q{};
        res_q.header.type = FFX_API_DESC_TYPE_UPSCALE_GET_RENDERRESOLUTION;
        res_q.displayWidth = 3840;
        res_q.displayHeight = 2160;
        res_q.qualityMode = FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;  // 2.0x
        res_q.pOutRenderWidth = &rw;
        res_q.pOutRenderHeight = &rh;
        SX_CHECK(ffxQuery(nullptr, &res_q.header) == FFX_API_RETURN_OK);
        SX_CHECK(rw == 1920 && rh == 1080);

        int32_t phases = 0;
        ffxQueryDescUpscaleGetJitterPhaseCount jp{};
        jp.header.type = FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_PHASE_COUNT;
        jp.renderWidth = 1920;
        jp.displayWidth = 3840;
        jp.pOutPhaseCount = &phases;
        SX_CHECK(ffxQuery(nullptr, &jp.header) == FFX_API_RETURN_OK);
        SX_CHECK(phases == 32);

        float jx = 0.0f, jy = 0.0f;
        ffxQueryDescUpscaleGetJitterOffset jo{};
        jo.header.type = FFX_API_DESC_TYPE_UPSCALE_GET_JITTER_OFFSET;
        jo.index = 0;
        jo.phaseCount = phases;
        jo.pOutX = &jx;
        jo.pOutY = &jy;
        SX_CHECK(ffxQuery(nullptr, &jo.header) == FFX_API_RETURN_OK);
        SX_CHECK(std::fabs(jx) <= 0.5f && std::fabs(jy) <= 0.5f);

        ffxContext ctx = nullptr;
        ffxCreateContextDescUpscale cdesc{};
        cdesc.header.type = FFX_API_DESC_TYPE_UPSCALE_CREATE;
        cdesc.maxRenderWidth = 64;
        cdesc.maxRenderHeight = 64;
        cdesc.maxUpscaleWidth = 128;
        cdesc.maxUpscaleHeight = 128;
        SX_CHECK(ffxCreateContext(&ctx, &cdesc.header, nullptr) == FFX_API_RETURN_OK);
        SX_CHECK(ctx != nullptr);

        ffxConfigureDescUpscale cfg{};
        cfg.header.type = FFX_API_DESC_TYPE_UPSCALE_CONFIGURE;
        cfg.sharpness = 0.75f;
        cfg.enableSharpening = 1;
        SX_CHECK(ffxConfigure(&ctx, &cfg.header) == FFX_API_RETURN_OK);

        std::vector<uint8_t> src(32 * 32 * 4, 0);
        for (uint32_t y = 0; y < 32; ++y) {
            for (uint32_t x = 0; x < 32; ++x) {
                const size_t i = (static_cast<size_t>(y) * 32 + x) * 4;
                src[i + 0] = static_cast<uint8_t>(x * 8);
                src[i + 1] = static_cast<uint8_t>(y * 8);
                src[i + 2] = 200;
                src[i + 3] = 255;
            }
        }
        std::vector<uint8_t> dst(64 * 64 * 4, 0);
        uint64_t hash = 0;
        ffxDispatchDescUpscale dd{};
        dd.header.type = FFX_API_DESC_TYPE_UPSCALE_DISPATCH;
        dd.colorRgba8 = src.data();
        dd.outputRgba8 = dst.data();
        dd.renderWidth = 32;
        dd.renderHeight = 32;
        dd.upscaleWidth = 64;
        dd.upscaleHeight = 64;
        dd.outFrameHash = &hash;
        SX_CHECK(ffxDispatch(&ctx, &dd.header) == FFX_API_RETURN_OK);
        SX_CHECK(hash != 0);
        SX_CHECK(dst[2] == 200 && dst[3] == 255);
        SX_CHECK(ffxDestroyContext(&ctx, nullptr) == FFX_API_RETURN_OK);
    }

    // 3. libHttpClient.GDK.dll
    {
        SX_CHECK(HCInitialize(nullptr) == SX_S_OK);
        SX_CHECK(HCIsInitialized() == 1);
        SX_CHECK(HCSettingsSetTraceLevel(3u) == SX_S_OK);
        uint32_t trace_lvl = 0;
        SX_CHECK(HCSettingsGetTraceLevel(&trace_lvl) == SX_S_OK && trace_lvl == 3u);
        const char* hc_ver = nullptr;
        SX_CHECK(HCGetLibVersion(&hc_ver) == SX_S_OK && hc_ver != nullptr);

        HCHttpCallClearMocks();
        const std::string mock_json = "{\"entitled\":true,\"title\":\"ScarlettX\"}";
        SX_CHECK(HCHttpCallRegisterMock(
                     "GET", "https://title.mgt.xboxlive.com/", 200,
                     reinterpret_cast<const uint8_t*>(mock_json.data()),
                     static_cast<uint32_t>(mock_json.size()),
                     "application/json") == SX_S_OK);

        HCCallHandle call = nullptr;
        SX_CHECK(HCHttpCallCreate(&call) == SX_S_OK);
        HCCallHandle dup = HCHttpCallDuplicateHandle(call);
        SX_CHECK(dup == call);
        SX_CHECK(HCHttpCallRequestSetUrl(call, "GET", "https://title.mgt.xboxlive.com/config") == SX_S_OK);
        SX_CHECK(HCHttpCallRequestSetHeader(call, "Accept", "application/json", 1) == SX_S_OK);
        SX_CHECK(HCHttpCallRequestSetRequestBodyString(call, "{\"ping\":1}") == SX_S_OK);
        SX_CHECK(HCHttpCallRequestSetRetryAllowed(call, 0u) == SX_S_OK);

        bool completed = false;
        XAsyncBlock async{};
        async.context = &completed;
        async.callback = [](XAsyncBlock* ab) {
            *static_cast<bool*>(ab->context) = true;
        };
        SX_CHECK(HCHttpCallPerformAsync(call, &async) == SX_S_OK);
        SX_CHECK(completed);

        uint32_t status = 0;
        SX_CHECK(HCHttpCallResponseGetStatusCode(call, &status) == SX_S_OK);
        SX_CHECK(status == 200);
        const char* resp_str = nullptr;
        SX_CHECK(HCHttpCallResponseGetResponseString(call, &resp_str) == SX_S_OK);
        SX_CHECK(std::string(resp_str) == mock_json);
        const char* ct = nullptr;
        SX_CHECK(HCHttpCallResponseGetHeader(call, "Content-Type", &ct) == SX_S_OK);
        SX_CHECK(ct && std::string(ct) == "application/json");
        uint32_t hdr_count = 0;
        SX_CHECK(HCHttpCallResponseGetNumHeaders(call, &hdr_count) == SX_S_OK && hdr_count >= 2u);
        const char* hname = nullptr;
        const char* hval = nullptr;
        SX_CHECK(HCHttpCallResponseGetHeaderAtIndex(call, 0, &hname, &hval) == SX_S_OK);
        SX_CHECK(hname && hval && std::string(hname) == "Content-Type");
        SX_CHECK(HCHttpCallCloseHandle(dup) == SX_S_OK);
        // Original handle must still remain alive until its own CloseHandle.
        SX_CHECK(HCHttpCallResponseGetStatusCode(call, &status) == SX_S_OK && status == 200);
        SX_CHECK(HCHttpCallCloseHandle(call) == SX_S_OK);
        HCCleanup();
    }

    // 4. XCurl.dll
    {
        SX_CHECK(curl_global_init(0) == CURLE_OK);
        SX_CHECK(curl_version() != nullptr);
        xcurl_clear_mocks();
        const std::string payload = "{\"telemetry\":\"accepted\"}";
        SX_CHECK(xcurl_register_mock_response(
                     "https://services.scarlettx.local/", 200,
                     "application/json", payload.data(), payload.size()) == CURLE_OK);

        CURL* curl = curl_easy_init();
        SX_CHECK(curl != nullptr);
        curl_slist* hdrs = curl_slist_append(nullptr, "X-Client: ScarlettX");
        std::string body_out;
        SX_CHECK(curl_easy_setopt(curl, CURLOPT_URL, "https://services.scarlettx.local/ping") == CURLE_OK);
        SX_CHECK(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs) == CURLE_OK);
        SX_CHECK(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CurlWriteToString) == CURLE_OK);
        SX_CHECK(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_out) == CURLE_OK);
        SX_CHECK(curl_easy_perform(curl) == CURLE_OK);

        long code = 0;
        int64_t dl = 0;
        SX_CHECK(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK);
        SX_CHECK(curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &dl) == CURLE_OK);
        SX_CHECK(code == 200);
        SX_CHECK(body_out == payload);
        SX_CHECK(dl == static_cast<int64_t>(payload.size()));

        // curl_easy_escape / unescape / duphandle & curl_multi_*
        char* escaped = curl_easy_escape(curl, "hello world/1+2", 0);
        SX_CHECK(escaped != nullptr && std::string(escaped) == "hello%20world%2F1%2B2");
        int32_t unesc_len = 0;
        char* unescaped = curl_easy_unescape(curl, escaped, 0, &unesc_len);
        SX_CHECK(unescaped != nullptr && unesc_len == 15 && std::string(unescaped) == "hello world/1+2");
        curl_free(escaped);
        curl_free(unescaped);

        CURL* dup_curl = curl_easy_duphandle(curl);
        SX_CHECK(dup_curl != nullptr && dup_curl != curl);
        std::string multi_out;
        SX_CHECK(curl_easy_setopt(dup_curl, CURLOPT_WRITEDATA, &multi_out) == CURLE_OK);
        CURLM* multi = curl_multi_init();
        SX_CHECK(multi != nullptr);
        SX_CHECK(curl_multi_add_handle(multi, dup_curl) == CURLM_OK);
        int32_t running = -1;
        SX_CHECK(curl_multi_perform(multi, &running) == CURLM_OK && running == 0);
        int32_t queued = -1;
        CURLMsg* msg = curl_multi_info_read(multi, &queued);
        SX_CHECK(msg != nullptr && msg->msg == CURLMSG_DONE && msg->easy_handle == dup_curl &&
                 msg->data.result == CURLE_OK && queued == 0);
        SX_CHECK(multi_out == payload);
        SX_CHECK(curl_multi_remove_handle(multi, dup_curl) == CURLM_OK);
        SX_CHECK(curl_multi_cleanup(multi) == CURLM_OK);
        curl_easy_cleanup(dup_curl);

        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
    }

    std::cout << "[PASS] test_middleware_net (Bink2Scarlett, FidelityFX, libHttpClient.GDK, XCurl)\n";
    return 0;
}
