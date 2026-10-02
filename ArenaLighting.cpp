#include "plugin-std.h"

#include <Windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace plugin;

namespace {

constexpr unsigned int kLive2005EntryPoint = 0x00CD8005;

// TextureStadium's NBA Live 2005 material-colour submission:
//   push ecx                         ; packed material ARGB
//   push 3Ch                         ; D3DRS_TEXTUREFACTOR
//   push esi                         ; IDirect3DDevice9*
//   call dword ptr [edi+0E4h]        ; SetRenderState
//
// This was the stable, visibly effective path from the original scale test.
// It is local to TextureStadium and does not alter the court, players, menus,
// or the global D3DRS_AMBIENT state.
constexpr unsigned int kTextureFactorSubmission = 0x007AFCCB;
constexpr size_t kTextureFactorSubmissionSize = 10;

float gBrightness = 1.0f;
float gContrast = 1.0f;
float gSaturation = 1.0f;
float gGamma = 1.0f;
bool gStadiumLightingEnabled = false;
bool gDebugLoggingEnabled = false;
bool gInitialized = false;
volatile LONG gFirstTextureFactorLogged = 0;

bool BuildGamePath(char* outPath, size_t outSize, const char* relativePath) {
    if (!outPath || outSize == 0 || !relativePath)
        return false;

    char executablePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(
        nullptr,
        executablePath,
        static_cast<DWORD>(sizeof(executablePath)));

    if (length == 0 || length >= sizeof(executablePath))
        return false;

    char* separator = std::strrchr(executablePath, '\\');
    if (!separator)
        return false;

    separator[1] = '\0';

    const int written = std::snprintf(
        outPath,
        outSize,
        "%s%s",
        executablePath,
        relativePath);

    if (written < 0 || static_cast<size_t>(written) >= outSize) {
        outPath[0] = '\0';
        return false;
    }

    return true;
}

void Log(const char* format, ...) {
    if (!gDebugLoggingEnabled)
        return;

    char logPath[MAX_PATH] = {};
    if (!BuildGamePath(logPath, sizeof(logPath), "logs\\ArenaLighting.log"))
        return;

    FILE* file = nullptr;
    if (fopen_s(&file, logPath, "a") != 0 || !file)
        return;

    SYSTEMTIME time = {};
    GetLocalTime(&time);

    std::fprintf(
        file,
        "[%02u:%02u:%02u.%03u] ",
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds);

    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);

    std::fputc('\n', file);
    std::fclose(file);
}

float ClampChannel(float channel) {
    if (channel < 0.0f)
        return 0.0f;
    if (channel > 1.0f)
        return 1.0f;
    return channel;
}

unsigned int PackChannel(float channel) {
    return static_cast<unsigned int>(
        ClampChannel(channel) * 255.0f + 0.5f);
}

std::uint32_t __cdecl AdjustPackedTextureFactor(std::uint32_t original) {
    if (!gStadiumLightingEnabled)
        return original;

    const unsigned int alpha = original & 0xFF000000u;
    float red = static_cast<float>((original >> 16) & 0xFFu) / 255.0f;
    float green = static_cast<float>((original >> 8) & 0xFFu) / 255.0f;
    float blue = static_cast<float>(original & 0xFFu) / 255.0f;

    // Rec. 709 luminance keeps neutral material colours neutral while
    // saturation changes only their distance from gray.
    const float luminance =
        red * 0.2126f + green * 0.7152f + blue * 0.0722f;
    red = luminance + (red - luminance) * gSaturation;
    green = luminance + (green - luminance) * gSaturation;
    blue = luminance + (blue - luminance) * gSaturation;

    // Midpoint contrast, multiplicative brightness, then nonlinear midtone
    // shaping. Gamma > 1.0 darkens midtones; gamma < 1.0 lifts them.
    red = ((red - 0.5f) * gContrast + 0.5f) * gBrightness;
    green = ((green - 0.5f) * gContrast + 0.5f) * gBrightness;
    blue = ((blue - 0.5f) * gContrast + 0.5f) * gBrightness;

    red = std::pow(ClampChannel(red), gGamma);
    green = std::pow(ClampChannel(green), gGamma);
    blue = std::pow(ClampChannel(blue), gGamma);

    const unsigned int packedRed = PackChannel(red);
    const unsigned int packedGreen = PackChannel(green);
    const unsigned int packedBlue = PackChannel(blue);

    const std::uint32_t adjusted =
        alpha | (packedRed << 16) | (packedGreen << 8) | packedBlue;

    if (InterlockedCompareExchange(&gFirstTextureFactorLogged, 1, 0) == 0) {
        Log(
            "First TextureStadium D3DRS_TEXTUREFACTOR: original=0x%08X adjusted=0x%08X brightness=%.4f contrast=%.4f saturation=%.4f gamma=%.4f",
            original,
            adjusted,
            gBrightness,
            gContrast,
            gSaturation,
            gGamma);
    }

    return adjusted;
}

__declspec(naked) void TextureFactorSubmissionHook() {
    __asm {
        // ECX contains the packed material colour produced by TextureStadium.
        push ecx
        call AdjustPackedTextureFactor
        add esp, 4

        // Reproduce the original IDirect3DDevice9::SetRenderState call with
        // the adjusted packed colour in EAX.
        push eax
        push 3Ch
        push esi
        call dword ptr [edi+0E4h]
        ret
    }
}

bool ValidateTextureFactorSubmission() {
    const unsigned char expected[kTextureFactorSubmissionSize] = {
        0x51,
        0x6A, 0x3C,
        0x56,
        0xFF, 0x97, 0xE4, 0x00, 0x00, 0x00
    };

    for (size_t i = 0; i < sizeof(expected); ++i) {
        if (patch::GetUChar(kTextureFactorSubmission + i) != expected[i])
            return false;
    }

    return true;
}

bool InstallRelativeCall(
    unsigned int address,
    size_t patchSize,
    const void* target) {
    if (patchSize < 5)
        return false;

    unsigned char replacement[16] = {};
    if (patchSize > sizeof(replacement))
        return false;

    replacement[0] = 0xE8;
    for (size_t i = 5; i < patchSize; ++i)
        replacement[i] = 0x90;

    const std::intptr_t displacement =
        reinterpret_cast<std::intptr_t>(target) -
        static_cast<std::intptr_t>(address + 5);
    const std::int32_t relative = static_cast<std::int32_t>(displacement);
    std::memcpy(replacement + 1, &relative, sizeof(relative));

    DWORD oldProtection = 0;
    void* destination = reinterpret_cast<void*>(address);
    if (!VirtualProtect(
            destination,
            patchSize,
            PAGE_EXECUTE_READWRITE,
            &oldProtection)) {
        return false;
    }

    std::memcpy(destination, replacement, patchSize);
    FlushInstructionCache(GetCurrentProcess(), destination, patchSize);

    DWORD ignored = 0;
    VirtualProtect(destination, patchSize, oldProtection, &ignored);
    return true;
}

} // namespace

void SetArenaLightingStadiumLighting(
    const char* stadiumId,
    bool enabled,
    float brightness,
    float contrast,
    float saturation,
    float gamma) {
    if (brightness < 0.0f || brightness > 2.0f)
        brightness = 1.0f;
    if (contrast < 0.0f || contrast > 2.0f)
        contrast = 1.0f;
    if (saturation < 0.0f || saturation > 2.0f)
        saturation = 1.0f;
    if (gamma < 0.1f || gamma > 3.0f)
        gamma = 1.0f;

    gStadiumLightingEnabled = enabled;
    gBrightness = brightness;
    gContrast = contrast;
    gSaturation = saturation;
    gGamma = gamma;
    InterlockedExchange(&gFirstTextureFactorLogged, 0);

    Log(
        "Selected stadium '%s': MATERIAL_COLOUR_LIGHTING=%s BRIGHTNESS=%.4f CONTRAST=%.4f SATURATION=%.4f GAMMA=%.4f",
        stadiumId && stadiumId[0] ? stadiumId : "<unknown>",
        gStadiumLightingEnabled ? "enabled" : "disabled",
        gBrightness,
        gContrast,
        gSaturation,
        gGamma);
}

void InitializeArenaLighting() {
    if (gInitialized || FM::GetEntryPoint() != kLive2005EntryPoint)
        return;

    char iniPath[MAX_PATH] = {};
    if (!BuildGamePath(iniPath, sizeof(iniPath), "main.ini"))
        return;

    gDebugLoggingEnabled =
        GetPrivateProfileIntA("DEBUG", "CONSOLE", 0, iniPath) != 0;

    if (gDebugLoggingEnabled) {
        char logsPath[MAX_PATH] = {};
        if (BuildGamePath(logsPath, sizeof(logsPath), "logs"))
            CreateDirectoryA(logsPath, nullptr);
    }

    if (!ValidateTextureFactorSubmission()) {
        OutputDebugStringA(
            "[ArenaLighting] NBA Live 2005 TextureStadium validation failed; no patch installed.\n");
        Log("TextureStadium texture-factor validation failed; no patch installed.");
        return;
    }

    if (!InstallRelativeCall(
            kTextureFactorSubmission,
            kTextureFactorSubmissionSize,
            &TextureFactorSubmissionHook)) {
        OutputDebugStringA(
            "[ArenaLighting] NBA Live 2005 TextureStadium hook installation failed.\n");
        Log("TextureStadium texture-factor hook installation failed.");
        return;
    }

    gInitialized = true;

    Log(
        "Initialized NBA Live 2005 TextureStadium material-colour brightness/contrast: factor=0x%08X",
        kTextureFactorSubmission);

    OutputDebugStringA(
        "[ArenaLighting] NBA Live 2005 TextureStadium material-colour adjustment installed.\n");
}
