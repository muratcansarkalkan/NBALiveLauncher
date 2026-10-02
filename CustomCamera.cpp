#include "plugin-std.h"

#include <Windows.h>
#include <cctype>
#include <cstdio>
#include <cstring>

using namespace plugin;

bool ResolveStadiumCameraPath(
    const char* stadiumId,
    char* outPath,
    size_t outPathSize);

void UpdateCustomPathsGameContext(
    const char* stadiumId4,
    void* options);

namespace {

constexpr unsigned int kLive2005EntryPoint = 0x00CD8005;
constexpr unsigned int kLive060708EntryPoint = 0x0040109F;
constexpr unsigned int kLive06AspectRatio = 0x00BD832C;
constexpr unsigned int kLive07AspectRatio = 0x00BBBC3C;
constexpr float kStandardAspectRatio = 1.3333334f;

const char kDefaultCameraScript[] = "merlin\\nbacam.mgd";
char gStadiumId4[5] = {};
char gCustomCameraScript[MAX_PATH] = {};

const unsigned int kLive2005GameCourtSetupCalls[] = {
    0x0060A0A0, 0x0060A0B3, 0x0060A15C, 0x0060A2D1,
    0x0060A379, 0x0060A746, 0x0060ADDE, 0x0060AFE1,
    0x0060B06F, 0x0060B38D, 0x0060C40F, 0x0060D536,
    0x0060D56B, 0x0060E357, 0x0060E39A
};

const unsigned int kLive2005CameraLoadCalls[] = {
    0x00620F1D, 0x00621632, 0x00621912, 0x00621C02,
    0x00622206, 0x00622BA6, 0x006233F2, 0x00623C22
};

const unsigned int kLive06GameCourtSetupCalls[] = {
    0x00617F10, 0x00617F23, 0x00617FCC, 0x00618461,
    0x00618508, 0x00619391, 0x0061941E, 0x0061B146,
    0x0061B17B, 0x0061CD2E, 0x0061D2E2, 0x0061DC80,
    0x0061E059, 0x0061F0BB
};

const unsigned int kLive06CameraLoadCalls[] = {
    0x0063071D, 0x00630E32, 0x00631112, 0x00631438,
    0x00631A48, 0x00632418, 0x00632C38, 0x006333F2
};

// NBA Live 07 stores the stadium abbreviation through a small GameCourt
// setter instead of passing it through the NBA Live 05/06 Setup signature.
const unsigned int kLive07GameCourtNameCalls[] = {
    0x0064EDE7, 0x006504D2, 0x00650B42, 0x006515A2,
    0x00655CF1, 0x006567E1, 0x00656D83, 0x00657053
};

const unsigned int kLive07CameraLoadCalls[] = {
    0x0066721D, 0x00667932, 0x00667C12, 0x00667F28,
    0x00668548, 0x00668F18, 0x00669738, 0x00669F02
};

enum StadiumCaptureKind {
    CAPTURE_GAMECOURT_SETUP_0506,
    CAPTURE_GAMECOURT_NAME_07
};

struct CustomCameraBuild {
    const char* name;
    StadiumCaptureKind stadiumCaptureKind;
    unsigned int gameCourtSetup;
    unsigned int cameraManagerLoadScripts;
    unsigned int nbaCameraScriptSlot;
    const unsigned int* gameCourtSetupCalls;
    size_t gameCourtSetupCallCount;
    const unsigned int* cameraLoadCalls;
    size_t cameraLoadCallCount;
};

const CustomCameraBuild kLive2005Build = {
    "NBA Live 2005",
    CAPTURE_GAMECOURT_SETUP_0506,
    0x00608A00,
    0x006251A0,
    0x00B640CC,
    kLive2005GameCourtSetupCalls,
    sizeof(kLive2005GameCourtSetupCalls) /
        sizeof(kLive2005GameCourtSetupCalls[0]),
    kLive2005CameraLoadCalls,
    sizeof(kLive2005CameraLoadCalls) / sizeof(kLive2005CameraLoadCalls[0])
};

const CustomCameraBuild kLive06Build = {
    "NBA Live 06",
    CAPTURE_GAMECOURT_SETUP_0506,
    0x00616D60,
    0x00634980,
    0x00BCA4E4,
    kLive06GameCourtSetupCalls,
    sizeof(kLive06GameCourtSetupCalls) /
        sizeof(kLive06GameCourtSetupCalls[0]),
    kLive06CameraLoadCalls,
    sizeof(kLive06CameraLoadCalls) / sizeof(kLive06CameraLoadCalls[0])
};

const CustomCameraBuild kLive07Build = {
    "NBA Live 07",
    CAPTURE_GAMECOURT_NAME_07,
    0x0067B710,
    0x0066B4C0,
    0x00BEC128,
    kLive07GameCourtNameCalls,
    sizeof(kLive07GameCourtNameCalls) /
        sizeof(kLive07GameCourtNameCalls[0]),
    kLive07CameraLoadCalls,
    sizeof(kLive07CameraLoadCalls) /
        sizeof(kLive07CameraLoadCalls[0])
};

const CustomCameraBuild* gBuild = nullptr;

const CustomCameraBuild* DetectBuild() {
    const unsigned int entryPoint = FM::GetEntryPoint();

    if (entryPoint == kLive2005EntryPoint)
        return &kLive2005Build;

    if (entryPoint == kLive060708EntryPoint &&
        patch::GetFloat(kLive06AspectRatio) == kStandardAspectRatio) {
        return &kLive06Build;
    }

    if (entryPoint == kLive060708EntryPoint &&
        patch::GetFloat(kLive07AspectRatio) == kStandardAspectRatio) {
        return &kLive07Build;
    }

    return nullptr;
}

bool IsValidStadiumId4(const char* value) {
    if (!value)
        return false;

    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == '\0' || !std::isalnum(c))
            return false;
    }

    return value[4] == '\0';
}

void CacheStadiumId4(const char* value) {
    if (!IsValidStadiumId4(value)) {
        gStadiumId4[0] = '\0';
        return;
    }

    for (int i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        gStadiumId4[i] = static_cast<char>(std::tolower(c));
    }
    gStadiumId4[4] = '\0';
}

bool FileExistsBesideExecutable(const char* relativePath) {
    char absolutePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, absolutePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;

    char* separator = std::strrchr(absolutePath, '\\');
    if (!separator)
        return false;

    separator[1] = '\0';
    const size_t used = std::strlen(absolutePath);
    const size_t available = MAX_PATH - used;
    if (std::strlen(relativePath) + 1 > available)
        return false;

    std::strcat(absolutePath, relativePath);
    const DWORD attributes = GetFileAttributesA(absolutePath);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void SelectCameraScript() {
    const char* selectedPath = kDefaultCameraScript;

    if (gStadiumId4[0] != '\0') {
        const bool hasConfiguredCamera =
            ResolveStadiumCameraPath(
                gStadiumId4,
                gCustomCameraScript,
                sizeof(gCustomCameraScript));

        if (!hasConfiguredCamera) {
            sprintf_s(
                gCustomCameraScript,
                sizeof(gCustomCameraScript),
                "assets\\camera\\%s.mgd",
                gStadiumId4);
            gCustomCameraScript[sizeof(gCustomCameraScript) - 1] = '\0';
        }

        if (FileExistsBesideExecutable(gCustomCameraScript))
            selectedPath = gCustomCameraScript;
    }

    patch::SetPointer(gBuild->nbaCameraScriptSlot,
                      const_cast<char*>(selectedPath));
}

// Shared NBA Live 2005/06 signature:
// GameCourt::Setup(const char*, const char*, CameraGlobal*, OptionsSettings*,
//                  bool, bool, const char*)
void __fastcall GameCourtSetupHook(
    void* gameCourt,
    void*,
    const char* ubiPath,
    const char* stadiumId4,
    void* camera,
    void* options,
    bool arg5,
    bool arg6,
    const char* arg7) {
    CacheStadiumId4(stadiumId4);
    UpdateCustomPathsGameContext(stadiumId4, options);

    CallMethodDynGlobal(
        gBuild->gameCourtSetup,
        gameCourt,
        ubiPath,
        stadiumId4,
        camera,
        options,
        arg5,
        arg6,
        arg7);
}

// NBA Live 07: GameCourt::SetStadiumName(const char* stadiumId4).
// The original copies the zero-terminated ID into GameCourt + 0x3BC.
void __fastcall GameCourtSetStadiumNameHook07(
    void* gameCourt,
    void*,
    const char* stadiumId4) {
    CacheStadiumId4(stadiumId4);
    UpdateCustomPathsGameContext(stadiumId4, nullptr);
    CallMethodDynGlobal(
        gBuild->gameCourtSetup,
        gameCourt,
        stadiumId4);
}

// CamManager::LoadCameraScripts(int mode)
void __fastcall CameraLoadHook(void* cameraManager, void*, int mode) {
    SelectCameraScript();
    CallMethodDynGlobal(
        gBuild->cameraManagerLoadScripts, cameraManager, mode);
}

bool ValidateCallSites(
    const unsigned int* sites,
    size_t count,
    unsigned int expectedTarget) {
    for (size_t i = 0; i < count; ++i) {
        const unsigned int callAddress = sites[i];
        if (patch::GetUChar(callAddress) != 0xE8)
            return false;

        const int displacement = patch::GetInt(callAddress + 1);
        const unsigned int target = callAddress + 5 + displacement;
        if (target != expectedTarget)
            return false;
    }
    return true;
}

void RedirectCalls(const unsigned int* sites, size_t count, void* hook) {
    for (size_t i = 0; i < count; ++i)
        patch::RedirectCall(sites[i], hook);
}

} // namespace

void InitializeCustomCamera() {
    gBuild = DetectBuild();
    if (!gBuild)
        return;

    // Refuse the whole patch if this is not the exact executable revision.
    // This prevents a partial installation when an address belongs to a
    // different build.
    if (!ValidateCallSites(
            gBuild->gameCourtSetupCalls,
            gBuild->gameCourtSetupCallCount,
            gBuild->gameCourtSetup) ||
        !ValidateCallSites(
            gBuild->cameraLoadCalls,
            gBuild->cameraLoadCallCount,
            gBuild->cameraManagerLoadScripts)) {
        OutputDebugStringA(
            "[CustomCamera] Call-site validation failed.\n");
        gBuild = nullptr;
        return;
    }

    gStadiumId4[0] = '\0';
    patch::SetPointer(gBuild->nbaCameraScriptSlot,
                      const_cast<char*>(kDefaultCameraScript));
    void* stadiumCaptureHook =
        gBuild->stadiumCaptureKind == CAPTURE_GAMECOURT_NAME_07
            ? reinterpret_cast<void*>(&GameCourtSetStadiumNameHook07)
            : reinterpret_cast<void*>(&GameCourtSetupHook);

    RedirectCalls(
                  gBuild->gameCourtSetupCalls,
                  gBuild->gameCourtSetupCallCount,
                  stadiumCaptureHook);
    RedirectCalls(
                  gBuild->cameraLoadCalls,
                  gBuild->cameraLoadCallCount,
                  reinterpret_cast<void*>(&CameraLoadHook));

    char message[96] = {};
    sprintf_s(
        message,
        sizeof(message),
        "[CustomCamera] Initialized for %s.\n",
        gBuild->name);
    OutputDebugStringA(message);
}
