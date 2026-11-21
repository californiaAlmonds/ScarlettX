#pragma once

#include <cstddef>
#include <cstdint>

#include "scarlettx/gdk/xgameruntime_abi.h"

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_XSAPI)
#    define SCARLETTX_XSAPI_API __declspec(dllexport)
#  else
#    define SCARLETTX_XSAPI_API __declspec(dllimport)
#  endif
#else
#  define SCARLETTX_XSAPI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Standard XSAPI HRESULT error codes (matches public GDK XSAPI-C ABI).
constexpr XGameResult XBL_S_OK = XGR_S_OK;
constexpr XGameResult XBL_E_RUNTIME_ERROR = static_cast<XGameResult>(0x89235100u);
constexpr XGameResult XBL_E_ALREADY_INITIALIZED = static_cast<XGameResult>(0x89235105u);
constexpr XGameResult XBL_E_NOT_INITIALIZED = static_cast<XGameResult>(0x89235106u);

// Standard XSAPI string buffer sizes (matches public GDK XSAPI-C headers).
constexpr size_t XBL_SCID_LENGTH = 38;
constexpr size_t XBL_GAMERTAG_CHAR_SIZE = 16 * 3;
constexpr size_t XBL_MODERN_GAMERTAG_CHAR_SIZE = 12 * 4 + 1;
constexpr size_t XBL_MODERN_GAMERTAG_SUFFIX_CHAR_SIZE = 14 + 1;
constexpr size_t XBL_UNIQUE_MODERN_GAMERTAG_CHAR_SIZE = 16 * 4 + 1;
constexpr size_t XBL_DISPLAY_NAME_CHAR_SIZE = 128;
constexpr size_t XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE = 225;
constexpr size_t XBL_GAMERSCORE_CHAR_SIZE = 16;

// ---------------------------------------------------------------------------
// Lifecycle & Context (xbox_live_global_c.h / xbox_live_context_c.h)
// ---------------------------------------------------------------------------
typedef struct XblContext* XblContextHandle;

struct XblInitArgs {
    XTaskQueueHandle queue;
    const char* scid;
};

SCARLETTX_XSAPI_API XGameResult XblInitialize(const XblInitArgs* args);
SCARLETTX_XSAPI_API XGameResult XblCleanupAsync(XAsyncBlock* async);

SCARLETTX_XSAPI_API XGameResult XblContextCreateHandle(
    XUserHandle user,
    XblContextHandle* context);
SCARLETTX_XSAPI_API XGameResult XblContextDuplicateHandle(
    XblContextHandle handle,
    XblContextHandle* duplicatedHandle);
SCARLETTX_XSAPI_API void XblContextCloseHandle(XblContextHandle handle);
SCARLETTX_XSAPI_API XGameResult XblContextGetUser(
    XblContextHandle context,
    XUserHandle* user);
SCARLETTX_XSAPI_API XGameResult XblContextGetXboxUserId(
    XblContextHandle context,
    uint64_t* xboxUserId);

// ---------------------------------------------------------------------------
// Achievements (achievements_c.h)
// ---------------------------------------------------------------------------
typedef struct XblAchievementsResult* XblAchievementsResultHandle;

enum XblAchievementType : uint32_t {
    XblAchievementType_Unknown = 0,
    XblAchievementType_All = 1,
    XblAchievementType_Persistent = 2,
    XblAchievementType_Challenge = 3,
};

enum XblAchievementOrderBy : uint32_t {
    XblAchievementOrderBy_DefaultOrder = 0,
    XblAchievementOrderBy_TitleId = 1,
    XblAchievementOrderBy_UnlockTime = 2,
};

enum XblAchievementProgressState : uint32_t {
    XblAchievementProgressState_Unknown = 0,
    XblAchievementProgressState_Achieved = 1,
    XblAchievementProgressState_NotStarted = 2,
    XblAchievementProgressState_InProgress = 3,
};

enum XblAchievementParticipationType : uint32_t {
    XblAchievementParticipationType_Unknown = 0,
    XblAchievementParticipationType_Individual = 1,
    XblAchievementParticipationType_Group = 2,
};

struct XblAchievementRequirement {
    const char* id;
    const char* currentProgressValue;
    const char* targetProgressValue;
};

struct XblAchievementProgression {
    XblAchievementRequirement* requirements;
    size_t requirementsCount;
    int64_t timeUnlocked;
};

struct XblAchievement {
    const char* id;
    const char* serviceConfigurationId;
    const char* name;
    uint32_t titleId;
    XblAchievementProgressState progressState;
    XblAchievementProgression progression;
    const char* unlockedDescription;
    const char* lockedDescription;
    XblAchievementType type;
    XblAchievementParticipationType participationType;
    uint32_t percentComplete;
    uint32_t gamerscore;
    bool isSecret;
    bool isRevoked;
};

SCARLETTX_XSAPI_API XGameResult XblAchievementsGetAchievementsForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    uint32_t type,
    bool unlockedOnly,
    uint32_t orderBy,
    uint32_t skipItems,
    uint32_t maxItems,
    XAsyncBlock* async);
SCARLETTX_XSAPI_API XGameResult XblAchievementsGetAchievementsForTitleIdResult(
    XAsyncBlock* async,
    XblAchievementsResultHandle* result);
SCARLETTX_XSAPI_API XGameResult XblAchievementsResultGetAchievements(
    XblAchievementsResultHandle resultHandle,
    const XblAchievement** achievements,
    size_t* achievementsCount);
SCARLETTX_XSAPI_API XGameResult XblAchievementsResultHasNext(
    XblAchievementsResultHandle resultHandle,
    bool* hasNext);
SCARLETTX_XSAPI_API XGameResult XblAchievementsResultDuplicateHandle(
    XblAchievementsResultHandle handle,
    XblAchievementsResultHandle* duplicatedHandle);
SCARLETTX_XSAPI_API void XblAchievementsResultCloseHandle(
    XblAchievementsResultHandle resultHandle);
SCARLETTX_XSAPI_API XGameResult XblAchievementsUpdateAchievementAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async);
SCARLETTX_XSAPI_API XGameResult XblAchievementsUpdateAchievementForTitleIdAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    uint32_t titleId,
    const char* serviceConfigurationId,
    const char* achievementId,
    uint32_t percentComplete,
    XAsyncBlock* async);

// ---------------------------------------------------------------------------
// Presence (presence_c.h)
// ---------------------------------------------------------------------------
typedef struct XblPresenceRecord* XblPresenceRecordHandle;

enum XblPresenceUserState : uint32_t {
    XblPresenceUserState_Unknown = 0,
    XblPresenceUserState_Online = 1,
    XblPresenceUserState_Away = 2,
    XblPresenceUserState_Offline = 3,
};

enum XblPresenceDeviceType : uint32_t {
    XblPresenceDeviceType_Unknown = 0,
    XblPresenceDeviceType_WindowsPhone = 1,
    XblPresenceDeviceType_WindowsPhone7 = 2,
    XblPresenceDeviceType_Web = 3,
    XblPresenceDeviceType_Xbox360 = 4,
    XblPresenceDeviceType_PC = 5,
    XblPresenceDeviceType_Windows8 = 6,
    XblPresenceDeviceType_XboxOne = 7,
    XblPresenceDeviceType_WindowsOneCore = 8,
    XblPresenceDeviceType_WindowsOneCoreMobile = 9,
    XblPresenceDeviceType_iOS = 10,
    XblPresenceDeviceType_Android = 11,
    XblPresenceDeviceType_Nintendo = 12,
    XblPresenceDeviceType_PlayStation = 13,
    XblPresenceDeviceType_Scarlett = 14,
};

struct XblPresenceRichPresenceIds {
    char scid[XBL_SCID_LENGTH];
    const char* presenceId;
    const char** presenceTokenIds;
    size_t presenceTokenIdsCount;
};

struct XblPresenceTitleRecord {
    uint32_t titleId;
    const char* titleName;
    int64_t lastModified;
    bool titleActive;
    const char* richPresenceString;
    uint32_t viewState;
};

struct XblPresenceDeviceRecord {
    XblPresenceDeviceType deviceType;
    const XblPresenceTitleRecord* titleRecords;
    size_t titleRecordsCount;
};

SCARLETTX_XSAPI_API XGameResult XblPresenceSetPresenceAsync(
    XblContextHandle xboxLiveContext,
    bool isUserActiveInTitle,
    const XblPresenceRichPresenceIds* richPresenceIds,
    XAsyncBlock* async);
SCARLETTX_XSAPI_API XGameResult XblPresenceGetPresenceAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xuid,
    XAsyncBlock* async);
SCARLETTX_XSAPI_API XGameResult XblPresenceGetPresenceResult(
    XAsyncBlock* async,
    XblPresenceRecordHandle* presenceRecordHandle);
SCARLETTX_XSAPI_API XGameResult XblPresenceRecordGetXuid(
    XblPresenceRecordHandle handle,
    uint64_t* xuid);
SCARLETTX_XSAPI_API XGameResult XblPresenceRecordGetUserState(
    XblPresenceRecordHandle handle,
    XblPresenceUserState* userState);
SCARLETTX_XSAPI_API XGameResult XblPresenceRecordGetDeviceRecords(
    XblPresenceRecordHandle handle,
    const XblPresenceDeviceRecord** deviceRecords,
    size_t* deviceRecordsCount);
SCARLETTX_XSAPI_API XGameResult XblPresenceRecordDuplicateHandle(
    XblPresenceRecordHandle handle,
    XblPresenceRecordHandle* duplicatedHandle);
SCARLETTX_XSAPI_API void XblPresenceRecordCloseHandle(
    XblPresenceRecordHandle handle);

// ---------------------------------------------------------------------------
// Profile (profile_c.h)
// ---------------------------------------------------------------------------
struct XblUserProfile {
    uint64_t xboxUserId;
    char appDisplayName[XBL_DISPLAY_NAME_CHAR_SIZE];
    char appDisplayPictureResizeUri[XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE];
    char gameDisplayName[XBL_DISPLAY_NAME_CHAR_SIZE];
    char gameDisplayPictureResizeUri[XBL_DISPLAY_PIC_URL_RAW_CHAR_SIZE];
    char gamerscore[XBL_GAMERSCORE_CHAR_SIZE];
    char gamertag[XBL_GAMERTAG_CHAR_SIZE];
    char modernGamertag[XBL_MODERN_GAMERTAG_CHAR_SIZE];
    char modernGamertagSuffix[XBL_MODERN_GAMERTAG_SUFFIX_CHAR_SIZE];
    char uniqueModernGamertag[XBL_UNIQUE_MODERN_GAMERTAG_CHAR_SIZE];
};

SCARLETTX_XSAPI_API XGameResult XblProfileGetUserProfileAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    XAsyncBlock* async);
SCARLETTX_XSAPI_API XGameResult XblProfileGetUserProfileResult(
    XAsyncBlock* async,
    XblUserProfile* profile);

// ---------------------------------------------------------------------------
// User Statistics & Title-Managed Statistics (user_statistics_c.h / title_managed_statistics_c.h)
// ---------------------------------------------------------------------------
struct XblStatistic {
    const char* statisticName;
    const char* statisticType;
    const char* value;
};

struct XblServiceConfigurationStatistic {
    char serviceConfigurationId[XBL_SCID_LENGTH];
    XblStatistic* statistics;
    size_t statisticsCount;
};

struct XblUserStatisticsResult {
    uint64_t xboxUserId;
    XblServiceConfigurationStatistic* serviceConfigStatistics;
    size_t serviceConfigStatisticsCount;
};

enum XblTitleManagedStatType : uint32_t {
    XblTitleManagedStatType_Number = 0,
    XblTitleManagedStatType_String = 1,
};

struct XblTitleManagedStatistic {
    const char* statisticName;
    XblTitleManagedStatType statisticType;
    double numberValue;
    const char* stringValue;
};

SCARLETTX_XSAPI_API XGameResult XblUserStatisticsGetSingleUserStatisticAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const char* serviceConfigurationId,
    const char* statisticName,
    XAsyncBlock* async);
SCARLETTX_XSAPI_API XGameResult XblUserStatisticsGetSingleUserStatisticResultSize(
    XAsyncBlock* async,
    size_t* resultSizeInBytes);
SCARLETTX_XSAPI_API XGameResult XblUserStatisticsGetSingleUserStatisticResult(
    XAsyncBlock* async,
    size_t bufferSize,
    void* buffer,
    XblUserStatisticsResult** ptrToBuffer,
    size_t* bufferUsed);
SCARLETTX_XSAPI_API XGameResult XblTitleManagedStatsWriteAsync(
    XblContextHandle xboxLiveContext,
    uint64_t xboxUserId,
    const XblTitleManagedStatistic* statistics,
    size_t statisticsCount,
    XAsyncBlock* async);

#ifdef __cplusplus
}
#endif
