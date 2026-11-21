#pragma once

#include <cstddef>
#include <cstdint>
#include <cwchar>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_XGAMERUNTIME)
#    define SCARLETTX_XGR_API __declspec(dllexport)
#  else
#    define SCARLETTX_XGR_API __declspec(dllimport)
#  endif
#  define SCARLETTX_GDK_CALLBACK __stdcall
#else
#  define SCARLETTX_XGR_API
#  define SCARLETTX_GDK_CALLBACK
#endif

extern "C" {

// Standard GDK HRESULT values (matches Windows / Xbox GDK ABI).
using XGameResult = int32_t;

constexpr XGameResult XGR_S_OK = 0x00000000;
constexpr XGameResult XGR_E_FAIL = static_cast<XGameResult>(0x80004005u);
constexpr XGameResult XGR_E_INVALIDARG = static_cast<XGameResult>(0x80070057u);
constexpr XGameResult XGR_E_POINTER = static_cast<XGameResult>(0x80004003u);
constexpr XGameResult XGR_E_OUTOFMEMORY = static_cast<XGameResult>(0x8007000Eu);
constexpr XGameResult XGR_E_HANDLE = static_cast<XGameResult>(0x80070006u);
constexpr XGameResult XGR_E_ABORT = static_cast<XGameResult>(0x80004004u);
constexpr XGameResult XGR_E_PENDING = static_cast<XGameResult>(0x8000000Au);
constexpr XGameResult XGR_E_NOT_SUFFICIENT_BUFFER = static_cast<XGameResult>(0x8007007Au);
constexpr XGameResult XGR_E_NOTFOUND = static_cast<XGameResult>(0x80070490u);
constexpr XGameResult XGR_E_UNEXPECTED = static_cast<XGameResult>(0x8000FFFFu);
constexpr XGameResult XGR_E_NOT_SUPPORTED = static_cast<XGameResult>(0x80070032u);
constexpr XGameResult XGR_E_NO_TASK_QUEUE = static_cast<XGameResult>(0x800701ABu);
constexpr XGameResult XGR_E_GAMERUNTIME_NOT_INITIALIZED = static_cast<XGameResult>(0x89240100u);
constexpr XGameResult XGR_E_GS_USER_NOT_REGISTERED = static_cast<XGameResult>(0x80830001u);
constexpr XGameResult XGR_E_GS_CONTAINER_NOT_IN_SYNC = static_cast<XGameResult>(0x80830002u);
constexpr XGameResult XGR_E_GS_BLOB_NOT_FOUND = static_cast<XGameResult>(0x80830008u);
constexpr XGameResult XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL = static_cast<XGameResult>(0x80830007u);
constexpr XGameResult XGR_E_GS_QUOTA_EXCEEDED = static_cast<XGameResult>(0x80830006u);

// ---------------------------------------------------------------------------
// XGameRuntimeInit.h
// ---------------------------------------------------------------------------
enum XGameRuntimeFeature : uint32_t {
    XGameRuntimeFeature_XTaskQueue = 0,
    XGameRuntimeFeature_XAsync = 1,
    XGameRuntimeFeature_XUser = 2,
    XGameRuntimeFeature_XGameSave = 3,
    XGameRuntimeFeature_XSystem = 4,
    XGameRuntimeFeature_XMem = 5,
    XGameRuntimeFeature_XThread = 6,
};

struct XGameRuntimeOptions {
    uint32_t gameConfigSource;
    const char* gameConfig;
};

SCARLETTX_XGR_API XGameResult XGameRuntimeInitialize();
SCARLETTX_XGR_API XGameResult XGameRuntimeInitializeWithOptions(
    const XGameRuntimeOptions* options);
SCARLETTX_XGR_API void XGameRuntimeUninitialize();
SCARLETTX_XGR_API XGameResult XGameRuntimeIsFeatureAvailable(
    XGameRuntimeFeature feature,
    bool* isAvailable);

// ---------------------------------------------------------------------------
// XTaskQueue.h
// ---------------------------------------------------------------------------
typedef struct XTaskQueueObject* XTaskQueueHandle;

enum XTaskQueuePort : uint32_t {
    XTaskQueuePort_Work = 0,
    XTaskQueuePort_Completion = 1,
};

typedef struct XTaskQueuePortObject* XTaskQueuePortHandle;

// Values match the public XTaskQueue.h; guests pass them as raw integers.
enum XTaskQueueDispatchMode : uint32_t {
    XTaskQueueDispatchMode_Manual = 0,
    XTaskQueueDispatchMode_ThreadPool = 1,
    XTaskQueueDispatchMode_SerializedThreadPool = 2,
    XTaskQueueDispatchMode_Immediate = 3,
};

struct XTaskQueueRegistrationToken {
    uint64_t token;
};

typedef void(SCARLETTX_GDK_CALLBACK XTaskQueueCallback)(void* context, bool canceled);
typedef void(SCARLETTX_GDK_CALLBACK XTaskQueueTerminatedCallback)(void* context);
typedef void(SCARLETTX_GDK_CALLBACK XTaskQueueMonitorCallback)(
    void* context,
    XTaskQueueHandle queue,
    XTaskQueuePort port);

SCARLETTX_XGR_API XGameResult XTaskQueueCreate(
    XTaskQueueDispatchMode workDispatchMode,
    XTaskQueueDispatchMode completionDispatchMode,
    XTaskQueueHandle* queue);
SCARLETTX_XGR_API XGameResult XTaskQueueCreateComposite(
    XTaskQueuePortHandle workPort,
    XTaskQueuePortHandle completionPort,
    XTaskQueueHandle* queue);
SCARLETTX_XGR_API XGameResult XTaskQueueGetPort(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    XTaskQueuePortHandle* portHandle);
SCARLETTX_XGR_API XGameResult XTaskQueueDuplicateHandle(
    XTaskQueueHandle queueHandle,
    XTaskQueueHandle* duplicatedHandle);
SCARLETTX_XGR_API void XTaskQueueCloseHandle(XTaskQueueHandle queue);
SCARLETTX_XGR_API XGameResult XTaskQueueSubmitCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* callbackContext,
    XTaskQueueCallback* callback);
SCARLETTX_XGR_API XGameResult XTaskQueueSubmitDelayedCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t delayMs,
    void* callbackContext,
    XTaskQueueCallback* callback);
SCARLETTX_XGR_API bool XTaskQueueDispatch(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t timeoutInMs);
SCARLETTX_XGR_API XGameResult XTaskQueueTerminate(
    XTaskQueueHandle queue,
    bool wait,
    void* callbackContext,
    XTaskQueueTerminatedCallback* callback);
// Returns false (and a null handle) when the process queue was set to null.
// On success the caller owns the returned handle and must close it.
SCARLETTX_XGR_API bool XTaskQueueGetCurrentProcessTaskQueue(
    XTaskQueueHandle* queue);
SCARLETTX_XGR_API void XTaskQueueSetCurrentProcessTaskQueue(
    XTaskQueueHandle queue);
SCARLETTX_XGR_API XGameResult XTaskQueueRegisterMonitor(
    XTaskQueueHandle queue,
    void* callbackContext,
    XTaskQueueMonitorCallback* callback,
    XTaskQueueRegistrationToken* token);
SCARLETTX_XGR_API void XTaskQueueUnregisterMonitor(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token);
// waitHandle is a Win32 waitable HANDLE. The callback is submitted to the port
// each time the handle is signaled, until the waiter is unregistered.
SCARLETTX_XGR_API XGameResult XTaskQueueRegisterWaiter(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* waitHandle,
    void* callbackContext,
    XTaskQueueCallback* callback,
    XTaskQueueRegistrationToken* token);
SCARLETTX_XGR_API void XTaskQueueUnregisterWaiter(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token);

// ---------------------------------------------------------------------------
// XAsync.h
// ---------------------------------------------------------------------------
struct XAsyncBlock;
typedef void(SCARLETTX_GDK_CALLBACK XAsyncCompletionRoutine)(XAsyncBlock* asyncBlock);
typedef XGameResult(SCARLETTX_GDK_CALLBACK XAsyncWork)(XAsyncBlock* asyncBlock);

struct XAsyncBlock {
    XTaskQueueHandle queue;
    void* context;
    XAsyncCompletionRoutine* callback;
    unsigned char internal[sizeof(void*) * 4];
};

SCARLETTX_XGR_API XGameResult XAsyncGetStatus(XAsyncBlock* asyncBlock, bool wait);
SCARLETTX_XGR_API XGameResult XAsyncGetResultSize(XAsyncBlock* asyncBlock, size_t* bufferSize);
SCARLETTX_XGR_API void XAsyncCancel(XAsyncBlock* asyncBlock);
SCARLETTX_XGR_API XGameResult XAsyncRun(XAsyncBlock* asyncBlock, XAsyncWork* work);

// ---------------------------------------------------------------------------
// XAsyncProvider.h
// ---------------------------------------------------------------------------
enum XAsyncOp : uint32_t {
    XAsyncOp_Begin = 0,
    XAsyncOp_DoWork = 1,
    XAsyncOp_GetResult = 2,
    XAsyncOp_Cancel = 3,
    XAsyncOp_Cleanup = 4,
};

struct XAsyncProviderData {
    XAsyncBlock* async;
    size_t bufferSize;
    void* buffer;
    void* context;
};

typedef XGameResult(SCARLETTX_GDK_CALLBACK XAsyncProvider)(
    XAsyncOp op,
    const XAsyncProviderData* data);

SCARLETTX_XGR_API XGameResult XAsyncBegin(
    XAsyncBlock* asyncBlock,
    void* context,
    const void* identity,
    const char* identityName,
    XAsyncProvider* provider);
SCARLETTX_XGR_API XGameResult XAsyncSchedule(XAsyncBlock* asyncBlock, uint32_t delayInMs);
SCARLETTX_XGR_API void XAsyncComplete(
    XAsyncBlock* asyncBlock,
    XGameResult result,
    size_t requiredBufferSize);
SCARLETTX_XGR_API XGameResult XAsyncGetResult(
    XAsyncBlock* asyncBlock,
    const void* identity,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed);

// ---------------------------------------------------------------------------
// XSystem.h
// ---------------------------------------------------------------------------
struct XVersion {
    union {
        struct {
            uint16_t major;
            uint16_t minor;
            uint16_t build;
            uint16_t revision;
        };
        uint64_t Value;
    };
};

struct XSystemAnalyticsInfo {
    XVersion osVersion;
    XVersion hostingOsVersion;
    char family[64];
    char form[64];
};

struct XSystemRuntimeInfo {
    XVersion runtimeVersion;
    XVersion availableVersion;
};

SCARLETTX_XGR_API uint32_t XSystemGetDeviceType();
SCARLETTX_XGR_API XSystemAnalyticsInfo XSystemGetAnalyticsInfo();
SCARLETTX_XGR_API XSystemRuntimeInfo XSystemGetRuntimeInfo();
SCARLETTX_XGR_API XGameResult XSystemGetAppSpecificDeviceId(
    size_t appSpecificDeviceIdSize,
    char* appSpecificDeviceId,
    size_t* appSpecificDeviceIdUsed);
SCARLETTX_XGR_API XGameResult XSystemGetConsoleId(
    size_t consoleIdSize,
    char* consoleId,
    size_t* consoleIdUsed);
SCARLETTX_XGR_API XGameResult XSystemGetXboxLiveSandboxId(
    size_t sandboxIdSize,
    char* sandboxId,
    size_t* sandboxIdUsed);
SCARLETTX_XGR_API bool XSystemIsHandleValid(void* handle);

// ---------------------------------------------------------------------------
// XUser.h
// ---------------------------------------------------------------------------
typedef struct XUser* XUserHandle;

struct XUserLocalId {
    uint64_t value;
};

enum XUserAddOptions : uint32_t {
    XUserAddOptions_None = 0x00,
    XUserAddOptions_AddDefaultUserSilently = 0x01,
    XUserAddOptions_AllowGuests = 0x02,
    XUserAddOptions_AddDefaultUserAllowingUI = 0x04,
};

enum XUserGamertagComponent : uint32_t {
    XUserGamertagComponent_Classic = 0,
    XUserGamertagComponent_Modern = 1,
    XUserGamertagComponent_ModernSuffix = 2,
    XUserGamertagComponent_UniqueModern = 3,
};

enum XUserState : uint32_t {
    XUserState_SignedIn = 0,
    XUserState_SigningOut = 1,
    XUserState_SignedOut = 2,
};

enum XUserAgeGroup : uint32_t {
    XUserAgeGroup_Unknown = 0,
    XUserAgeGroup_Child = 1,
    XUserAgeGroup_Teen = 2,
    XUserAgeGroup_Adult = 3,
};

enum XUserPrivilegeOptions : uint32_t {
    XUserPrivilegeOptions_None = 0x00,
    XUserPrivilegeOptions_AllUsers = 0x01,
};

enum XUserPrivilege : uint32_t {
    XUserPrivilege_CrossPlay = 185,
    XUserPrivilege_Clubs = 188,
    XUserPrivilege_Sessions = 189,
    XUserPrivilege_Broadcast = 190,
    XUserPrivilege_ManageProfilePrivacy = 196,
    XUserPrivilege_GameDvr = 198,
    XUserPrivilege_MultiplayerParties = 203,
    XUserPrivilege_CloudManageSession = 207,
    XUserPrivilege_CloudJoinSession = 208,
    XUserPrivilege_CloudSavedGames = 209,
    XUserPrivilege_SocialNetworkSharing = 220,
    XUserPrivilege_UserGeneratedContent = 247,
    XUserPrivilege_Communications = 252,
    XUserPrivilege_Multiplayer = 254,
    XUserPrivilege_AddFriends = 255,
};

enum XUserPrivilegeDenyReason : uint32_t {
    XUserPrivilegeDenyReason_None = 0,
    XUserPrivilegeDenyReason_PurchaseRequired = 1,
    XUserPrivilegeDenyReason_Restricted = 2,
    XUserPrivilegeDenyReason_Banned = 3,
    XUserPrivilegeDenyReason_Unknown = 0xFFFFFFFFu,
};

enum XUserGamerPictureSize : uint32_t {
    XUserGamerPictureSize_Small = 0,
    XUserGamerPictureSize_Medium = 1,
    XUserGamerPictureSize_Large = 2,
    XUserGamerPictureSize_ExtraLarge = 3,
};

enum XUserGetTokenAndSignatureOptions : uint32_t {
    XUserGetTokenAndSignatureOptions_None = 0x00,
    XUserGetTokenAndSignatureOptions_ForceRefresh = 0x01,
    XUserGetTokenAndSignatureOptions_AllUsers = 0x02,
};

enum XUserChangeEvent : uint32_t {
    XUserChangeEvent_SignedInLocally = 0,
    XUserChangeEvent_SigningOut = 1,
    XUserChangeEvent_SignedOut = 2,
    XUserChangeEvent_Gamertag = 3,
    XUserChangeEvent_GamerPicture = 4,
    XUserChangeEvent_Privileges = 5,
};

struct XUserGetTokenAndSignatureHttpHeader {
    const char* name;
    const char* value;
};

struct XUserGetTokenAndSignatureData {
    size_t tokenSize;
    size_t signatureSize;
    const char* token;
    const char* signature;
};

struct XUserGetTokenAndSignatureUtf16HttpHeader {
    const wchar_t* name;
    const wchar_t* value;
};

struct XUserGetTokenAndSignatureUtf16Data {
    size_t tokenCount;
    size_t signatureCount;
    const wchar_t* token;
    const wchar_t* signature;
};

typedef struct XUserSignOutDeferral* XUserSignOutDeferralHandle;

typedef void(SCARLETTX_GDK_CALLBACK XUserChangeEventCallback)(
    void* context,
    XUserLocalId userLocalId,
    XUserChangeEvent event);

SCARLETTX_XGR_API XGameResult XUserAddAsync(
    XUserAddOptions options,
    XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XUserAddResult(
    XAsyncBlock* async,
    XUserHandle* newUser);
SCARLETTX_XGR_API XGameResult XUserDuplicateHandle(
    XUserHandle handle,
    XUserHandle* duplicatedHandle);
SCARLETTX_XGR_API void XUserCloseHandle(XUserHandle user);
SCARLETTX_XGR_API int32_t XUserCompare(XUserHandle user1, XUserHandle user2);
SCARLETTX_XGR_API XGameResult XUserGetId(XUserHandle user, uint64_t* userId);
SCARLETTX_XGR_API XGameResult XUserFindUserById(uint64_t userId, XUserHandle* handle);
SCARLETTX_XGR_API XGameResult XUserGetLocalId(XUserHandle user, XUserLocalId* userLocalId);
SCARLETTX_XGR_API XGameResult XUserFindUserByLocalId(
    XUserLocalId userLocalId,
    XUserHandle* handle);
SCARLETTX_XGR_API XGameResult XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent gamertagComponent,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed);
SCARLETTX_XGR_API XGameResult XUserGetGamerPictureAsync(
    XUserHandle user,
    XUserGamerPictureSize pictureSize,
    XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XUserGetGamerPictureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize);
SCARLETTX_XGR_API XGameResult XUserGetGamerPictureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    size_t* bufferUsed);
SCARLETTX_XGR_API XGameResult XUserGetState(XUserHandle user, XUserState* state);
SCARLETTX_XGR_API XGameResult XUserGetAgeGroup(XUserHandle user, XUserAgeGroup* ageGroup);
SCARLETTX_XGR_API XGameResult XUserGetIsGuest(XUserHandle user, bool* isGuest);
SCARLETTX_XGR_API XGameResult XUserGetMaxUsers(uint32_t* maxUsers);
SCARLETTX_XGR_API XGameResult XUserCheckPrivilege(
    XUserHandle user,
    XUserPrivilegeOptions options,
    XUserPrivilege privilege,
    bool* hasPrivilege,
    XUserPrivilegeDenyReason* reason);
SCARLETTX_XGR_API XGameResult XUserResolvePrivilegeWithUiAsync(
    XUserHandle user,
    XUserPrivilegeOptions options,
    XUserPrivilege privilege,
    XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XUserResolvePrivilegeWithUiResult(XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XUserGetTokenAndSignatureAsync(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions options,
    const char* method,
    const char* url,
    size_t headerCount,
    const XUserGetTokenAndSignatureHttpHeader* headers,
    size_t bodySize,
    const void* bodyBuffer,
    XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XUserGetTokenAndSignatureResultSize(
    XAsyncBlock* async,
    size_t* bufferSize);
SCARLETTX_XGR_API XGameResult XUserGetTokenAndSignatureResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureData** ptrToBuffer,
    size_t* bufferUsed);
SCARLETTX_XGR_API XGameResult XUserGetTokenAndSignatureUtf16Async(
    XUserHandle user,
    XUserGetTokenAndSignatureOptions options,
    const wchar_t* method,
    const wchar_t* url,
    size_t headerCount,
    const XUserGetTokenAndSignatureUtf16HttpHeader* headers,
    size_t bodySize,
    const void* bodyBuffer,
    XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XUserGetTokenAndSignatureUtf16ResultSize(
    XAsyncBlock* async,
    size_t* bufferSize);
SCARLETTX_XGR_API XGameResult XUserGetTokenAndSignatureUtf16Result(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XUserGetTokenAndSignatureUtf16Data** ptrToBuffer,
    size_t* bufferUsed);
SCARLETTX_XGR_API XGameResult XUserResolveIssueWithUiAsync(
    XUserHandle user,
    const char* url,
    XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XUserResolveIssueWithUiResult(XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XUserResolveIssueWithUiUtf16Async(
    XUserHandle user,
    const wchar_t* url,
    XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XUserResolveIssueWithUiUtf16Result(XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XUserRegisterForChangeEvent(
    XTaskQueueHandle queue,
    void* context,
    XUserChangeEventCallback* callback,
    XTaskQueueRegistrationToken* token);
SCARLETTX_XGR_API bool XUserUnregisterForChangeEvent(
    XTaskQueueRegistrationToken token,
    bool wait);
SCARLETTX_XGR_API XGameResult XUserGetSignOutDeferral(
    XUserSignOutDeferralHandle* deferral);
SCARLETTX_XGR_API void XUserCloseSignOutDeferralHandle(
    XUserSignOutDeferralHandle deferral);
// Legacy convenience helper retained for early synthetic tests.
SCARLETTX_XGR_API XGameResult XUserGetDefaultXuid(uint64_t* out_xuid);

// ---------------------------------------------------------------------------
// XGameSave.h
// ---------------------------------------------------------------------------
typedef struct XGameSaveProvider* XGameSaveProviderHandle;
typedef struct XGameSaveContainer* XGameSaveContainerHandle;
typedef struct XGameSaveUpdate* XGameSaveUpdateHandle;

struct XGameSaveContainerInfo {
    const char* name;
    const char* displayName;
    uint32_t blobCount;
    uint64_t totalSize;
    int64_t lastModifiedTime;
    bool needsSync;
};

struct XGameSaveBlobInfo {
    const char* name;
    uint32_t size;
};

struct XGameSaveBlob {
    XGameSaveBlobInfo info;
    uint8_t* data;
};

typedef bool(SCARLETTX_GDK_CALLBACK XGameSaveContainerInfoCallback)(
    const XGameSaveContainerInfo* container,
    void* context);
typedef bool(SCARLETTX_GDK_CALLBACK XGameSaveBlobInfoCallback)(
    const XGameSaveBlobInfo* info,
    void* context);

SCARLETTX_XGR_API XGameResult XGameSaveInitializeProvider(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XGameSaveProviderHandle* provider);
SCARLETTX_XGR_API XGameResult XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider);
SCARLETTX_XGR_API void XGameSaveCloseProvider(XGameSaveProviderHandle provider);
SCARLETTX_XGR_API XGameResult XGameSaveGetRemainingQuota(
    XGameSaveProviderHandle provider,
    int64_t* remainingQuota);
SCARLETTX_XGR_API XGameResult XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* name,
    XGameSaveContainerHandle* containerContext);
SCARLETTX_XGR_API void XGameSaveCloseContainer(XGameSaveContainerHandle context);
SCARLETTX_XGR_API XGameResult XGameSaveDeleteContainer(
    XGameSaveProviderHandle provider,
    const char* name);
SCARLETTX_XGR_API XGameResult XGameSaveEnumerateContainerInfo(
    XGameSaveProviderHandle provider,
    void* context,
    XGameSaveContainerInfoCallback* callback);
SCARLETTX_XGR_API XGameResult XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* displayName,
    XGameSaveUpdateHandle* updateContext);
SCARLETTX_XGR_API void XGameSaveCloseUpdate(XGameSaveUpdateHandle context);
SCARLETTX_XGR_API XGameResult XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t byteCount);
SCARLETTX_XGR_API XGameResult XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName);
SCARLETTX_XGR_API XGameResult XGameSaveSubmitUpdate(
    XGameSaveUpdateHandle updateContext);
SCARLETTX_XGR_API XGameResult XGameSaveSubmitUpdateAsync(
    XGameSaveUpdateHandle updateContext,
    XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XGameSaveSubmitUpdateResult(XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XGameSaveReadBlobData(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t* countOfBlobs,
    size_t blobsSize,
    XGameSaveBlob* blobData);
SCARLETTX_XGR_API XGameResult XGameSaveReadBlobDataAsync(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t countOfBlobs,
    XAsyncBlock* async);
SCARLETTX_XGR_API XGameResult XGameSaveReadBlobDataResult(
    XAsyncBlock* async,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs);
SCARLETTX_XGR_API XGameResult XGameSaveEnumerateBlobInfo(
    XGameSaveContainerHandle container,
    void* context,
    XGameSaveBlobInfoCallback* callback);

// ---------------------------------------------------------------------------
// XMem.h (GDK Memory & Unified Virtual Memory Hooks)
// ---------------------------------------------------------------------------
typedef void*(SCARLETTX_GDK_CALLBACK XMemAllocFunction)(
    size_t dwSize,
    uint64_t dwAttributes);
typedef void(SCARLETTX_GDK_CALLBACK XMemFreeFunction)(
    void* lpAddress,
    uint64_t dwAttributes);

SCARLETTX_XGR_API void* XMemAllocDefault(size_t dwSize, uint64_t dwAttributes);
SCARLETTX_XGR_API void XMemFreeDefault(void* lpAddress, uint64_t dwAttributes);
SCARLETTX_XGR_API void* XMemAlloc(size_t dwSize, uint64_t dwAttributes);
SCARLETTX_XGR_API void XMemFree(void* lpAddress, uint64_t dwAttributes);
SCARLETTX_XGR_API XGameResult XMemSetAllocationHooks(
    XMemAllocFunction* allocFunc,
    XMemFreeFunction* freeFunc);
SCARLETTX_XGR_API XGameResult XMemGetAllocationHooks(
    XMemAllocFunction** allocFunc,
    XMemFreeFunction** freeFunc);
SCARLETTX_XGR_API void* XMemVirtualAlloc(
    void* baseAddress,
    size_t size,
    uint32_t allocationType,
    uint32_t protect);
SCARLETTX_XGR_API bool XMemVirtualFree(
    void* address,
    size_t size,
    uint32_t freeType);

// ---------------------------------------------------------------------------
// XThread.h & XGame.h
// ---------------------------------------------------------------------------
SCARLETTX_XGR_API bool XThreadIsTimeSensitive();
SCARLETTX_XGR_API XGameResult XThreadSetTimeSensitive(bool isTimeSensitive);
SCARLETTX_XGR_API void XThreadAssertNotTimeSensitive();
SCARLETTX_XGR_API XGameResult XGameGetXboxTitleId(uint32_t* titleId);

// ---------------------------------------------------------------------------
// Verified EmbeddedXvd xgameruntime.dll exports (XError + ApiImpl)
// ---------------------------------------------------------------------------
SCARLETTX_XGR_API void XErrorReport(XGameResult hr, const char* msg);
SCARLETTX_XGR_API XGameResult InitializeApiImpl(uint32_t build, uint32_t revision);
SCARLETTX_XGR_API XGameResult InitializeApiImplEx(uint32_t build, uint32_t revision, uint64_t flags);
SCARLETTX_XGR_API void UninitializeApiImpl();
SCARLETTX_XGR_API XGameResult QueryApiImpl(const void* api_id, const void* riid, void** out_impl);

}  // extern "C"
