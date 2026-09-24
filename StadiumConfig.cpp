#include "plugin-std.h"
#include "Games.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdarg>

using namespace plugin;

namespace
{
    bool gDebugLoggingEnabled = false;

    void InitializeDebugLogging()
    {
        char exePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (!length || length >= MAX_PATH)
            return;

        char* slash = std::strrchr(exePath, '\\');
        if (!slash)
            return;

        *(slash + 1) = '\0';

        char iniPath[MAX_PATH] = {};
        std::snprintf(iniPath, sizeof(iniPath), "%smain.ini", exePath);

        gDebugLoggingEnabled =
            GetPrivateProfileIntA("DEBUG", "CONSOLE", 0, iniPath) != 0;

        if (!gDebugLoggingEnabled)
            return;

        char logsPath[MAX_PATH] = {};
        std::snprintf(logsPath, sizeof(logsPath), "%slogs", exePath);
        CreateDirectoryA(logsPath, nullptr);
    }

    bool BuildDebugLogPath(
        char* outPath,
        size_t outSize,
        const char* fileName)
    {
        if (!outPath || !outSize || !fileName || !gDebugLoggingEnabled)
        {
            if (outPath && outSize)
                outPath[0] = '\0';
            return false;
        }

        char exePath[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (!length || length >= MAX_PATH)
        {
            outPath[0] = '\0';
            return false;
        }

        char* slash = std::strrchr(exePath, '\\');
        if (!slash)
        {
            outPath[0] = '\0';
            return false;
        }

        *(slash + 1) = '\0';

        const int written =
            std::snprintf(outPath, outSize, "%slogs\\%s", exePath, fileName);

        if (written < 0 || static_cast<size_t>(written) >= outSize)
        {
            outPath[0] = '\0';
            return false;
        }

        return true;
    }

    // ============================================================
    // Shared stadium JSON
    //
    // assets/stadia/<stadium-abbr>.json
    //
    // {
    //   "dorna": {
    //     "ad_count": 12,
    //     "period": 8.6,
    //     "transition_period": 0.6
    //   }
    // }
    //
    // Missing file / section / key => EA defaults.
    // ============================================================

    constexpr float kDefaultPeriodSeconds = 8.6f;
    constexpr float kDefaultTransitionSeconds = 0.6f;
    constexpr int   kDefaultAdCount = 4;

    enum GameId
    {
        GAME_NONE = 0,
        GAME_2005 = 2005,
        GAME_2006 = 2006,
        GAME_2007 = 2007,
        GAME_2008 = 2008
    };

    struct GameDornaInfo
    {
        int game = GAME_NONE;

        uintptr_t updateFunction = 0;
        size_t updateScanSize = 0;

        uintptr_t logicalAdCountPatch = 0;

        uintptr_t periodTicksAddress = 0;
        uintptr_t transitionTicksAddress = 0;

        bool usesFloatTiming = false;
        float originalPeriodSeconds = kDefaultPeriodSeconds;
        float originalTransitionSeconds = kDefaultTransitionSeconds;
    };

    GameDornaInfo gGame{};

    // Private runtime values read by patched GameStadium::Update.
    float gDornaScrollStep = 0.25f;
    int   gLegacyPeriodTicks = 0;
    int   gLegacyTransitionTicks = 0;

    float gDornaPeriodSecondsRuntime = kDefaultPeriodSeconds;
    float gDornaTransitionSecondsRuntime = kDefaultTransitionSeconds;

    // EA baseline values captured before stadium overrides.
    int gOriginalPeriodTicks = 0;
    int gOriginalTransitionTicks = 0;

    // Current stadium runtime state.
    std::string gCurrentStadiumId;
    int   gCurrentAdCount = kDefaultAdCount;
    float gCurrentPeriodSeconds = kDefaultPeriodSeconds;
    float gCurrentTransitionSeconds = kDefaultTransitionSeconds;

    struct StadiumDornaConfig
    {
        bool hasAdCount = false;
        int adCount = kDefaultAdCount;

        bool hasPeriod = false;
        float period = kDefaultPeriodSeconds;

        bool hasTransitionPeriod = false;
        float transitionPeriod = kDefaultTransitionSeconds;
    };

    // ------------------------------------------------------------
    // Logger
    // ------------------------------------------------------------

    std::string GetGameDirectory()
    {
        char path[MAX_PATH]{};

        DWORD len = GetModuleFileNameA(
            nullptr,
            path,
            static_cast<DWORD>(sizeof(path))
        );

        if (len == 0 || len >= sizeof(path))
            return ".";

        std::string result(path, len);
        const size_t slash = result.find_last_of("\\/");

        if (slash == std::string::npos)
            return ".";

        return result.substr(0, slash);
    }

    void Log(const char* format, ...)
    {
        if (!gDebugLoggingEnabled)
            return;

        char message[1024]{};

        va_list args;
        va_start(args, format);

        vsnprintf_s(
            message,
            sizeof(message),
            _TRUNCATE,
            format,
            args
        );

        va_end(args);

        char logPath[MAX_PATH] = {};
        if (!BuildDebugLogPath(
                logPath,
                sizeof(logPath),
                "StadiumConfig.log"))
        {
            return;
        }

        FILE* file = nullptr;

        if (fopen_s(&file, logPath, "a") == 0 && file)
        {
            SYSTEMTIME st{};
            GetLocalTime(&st);

            std::fprintf(
                file,
                "[%02u:%02u:%02u.%03u] %s\n",
                st.wHour,
                st.wMinute,
                st.wSecond,
                st.wMilliseconds,
                message
            );

            std::fclose(file);
        }
    }

    // ------------------------------------------------------------
    // Memory helpers
    // ------------------------------------------------------------

    template <typename T>
    bool WriteValue(uintptr_t address, const T& value)
    {
        DWORD oldProtect = 0;

        if (!VirtualProtect(
                reinterpret_cast<void*>(address),
                sizeof(T),
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        std::memcpy(
            reinterpret_cast<void*>(address),
            &value,
            sizeof(T)
        );

        DWORD ignored = 0;

        VirtualProtect(
            reinterpret_cast<void*>(address),
            sizeof(T),
            oldProtect,
            &ignored
        );

        FlushInstructionCache(
            GetCurrentProcess(),
            reinterpret_cast<void*>(address),
            sizeof(T)
        );

        return true;
    }

    bool PatchBytes(
        uintptr_t address,
        const uint8_t* bytes,
        size_t size)
    {
        DWORD oldProtect = 0;

        if (!VirtualProtect(
                reinterpret_cast<void*>(address),
                size,
                PAGE_EXECUTE_READWRITE,
                &oldProtect))
        {
            return false;
        }

        std::memcpy(
            reinterpret_cast<void*>(address),
            bytes,
            size
        );

        DWORD ignored = 0;

        VirtualProtect(
            reinterpret_cast<void*>(address),
            size,
            oldProtect,
            &ignored
        );

        FlushInstructionCache(
            GetCurrentProcess(),
            reinterpret_cast<void*>(address),
            size
        );

        return true;
    }

    bool IsReadableAddress(const void* address)
    {
        MEMORY_BASIC_INFORMATION mbi{};

        if (!VirtualQuery(address, &mbi, sizeof(mbi)))
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        if (mbi.Protect & PAGE_GUARD)
            return false;

        return (mbi.Protect & 0xFF) != PAGE_NOACCESS;
    }

    bool FloatMatches(
        uintptr_t address,
        float expected,
        float epsilon = 0.0001f)
    {
        if (!IsReadableAddress(
                reinterpret_cast<const void*>(address)))
        {
            return false;
        }

        float value = 0.0f;

        std::memcpy(
            &value,
            reinterpret_cast<const void*>(address),
            sizeof(value)
        );

        return std::fabs(value - expected) <= epsilon;
    }

    bool ReplaceAbsoluteOperand(
        uintptr_t operandAddress,
        const void* replacement)
    {
        const uint32_t replacementAddress =
            static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(replacement)
            );

        return WriteValue<uint32_t>(
            operandAddress,
            replacementAddress
        );
    }

    int RedirectFloatReferences(
        uintptr_t functionAddress,
        size_t scanSize,
        float originalValue,
        float* replacementValue)
    {
        auto* code =
            reinterpret_cast<uint8_t*>(functionAddress);

        int patched = 0;

        for (size_t i = 0; i + 6 <= scanSize; ++i)
        {
            const bool isFmulAbsolute =
                code[i] == 0xD8 &&
                code[i + 1] == 0x0D;

            const bool isFldAbsolute =
                code[i] == 0xD9 &&
                code[i + 1] == 0x05;

            if (!isFmulAbsolute && !isFldAbsolute)
                continue;

            uint32_t referencedAddress = 0;

            std::memcpy(
                &referencedAddress,
                code + i + 2,
                sizeof(referencedAddress)
            );

            if (!FloatMatches(
                    referencedAddress,
                    originalValue))
            {
                continue;
            }

            if (ReplaceAbsoluteOperand(
                    functionAddress + i + 2,
                    replacementValue))
            {
                ++patched;
                i += 5;
            }
        }

        return patched;
    }

    int RedirectAbsoluteAddressReferences(
        uintptr_t functionAddress,
        size_t scanSize,
        uintptr_t originalAddress,
        const void* replacementAddress)
    {
        auto* code =
            reinterpret_cast<uint8_t*>(functionAddress);

        const uint32_t original =
            static_cast<uint32_t>(originalAddress);

        const uint32_t replacement =
            static_cast<uint32_t>(
                reinterpret_cast<uintptr_t>(replacementAddress)
            );

        int patched = 0;

        for (size_t i = 0;
             i + sizeof(uint32_t) <= scanSize;
             ++i)
        {
            uint32_t candidate = 0;

            std::memcpy(
                &candidate,
                code + i,
                sizeof(candidate)
            );

            if (candidate != original)
                continue;

            if (WriteValue<uint32_t>(
                    functionAddress + i,
                    replacement))
            {
                ++patched;
                i += sizeof(uint32_t) - 1;
            }
        }

        return patched;
    }

    uintptr_t ReadRelativeCallTarget(
        uintptr_t callAddress)
    {
        if (*reinterpret_cast<const uint8_t*>(
                callAddress) != 0xE8)
        {
            return 0;
        }

        int32_t relative = 0;

        std::memcpy(
            &relative,
            reinterpret_cast<const void*>(
                callAddress + 1),
            sizeof(relative)
        );

        return callAddress + 5 + relative;
    }

    // ------------------------------------------------------------
    // JSON/path helpers
    // ------------------------------------------------------------

    std::string NormalizeStadiumId(
        const char* stadiumId)
    {
        if (!stadiumId)
            return {};

        std::string id(stadiumId);

        while (!id.empty() &&
               std::isspace(
                   static_cast<unsigned char>(
                       id.back())))
        {
            id.pop_back();
        }

        size_t first = 0;

        while (first < id.size() &&
               std::isspace(
                   static_cast<unsigned char>(
                       id[first])))
        {
            ++first;
        }

        if (first)
            id.erase(0, first);

        std::transform(
            id.begin(),
            id.end(),
            id.begin(),
            [](unsigned char c)
            {
                return static_cast<char>(
                    std::tolower(c));
            }
        );

        id.erase(
            std::remove_if(
                id.begin(),
                id.end(),
                [](char c)
                {
                    const unsigned char uc =
                        static_cast<unsigned char>(c);

                    return !(
                        std::isalnum(uc) ||
                        c == '_' ||
                        c == '-'
                    );
                }
            ),
            id.end()
        );

        return id;
    }

    std::string BuildStadiumJsonPath(
        const std::string& stadiumId)
    {
        return
            GetGameDirectory() +
            "\\assets\\stadia\\" +
            stadiumId +
            ".json";
    }

    bool FindObjectBody(
        const std::string& json,
        const char* key,
        std::string& outBody)
    {
        const std::string quotedKey =
            std::string("\"") + key + "\"";

        const size_t keyPos =
            json.find(quotedKey);

        if (keyPos == std::string::npos)
            return false;

        const size_t colonPos =
            json.find(
                ':',
                keyPos + quotedKey.size()
            );

        if (colonPos == std::string::npos)
            return false;

        const size_t openPos =
            json.find(
                '{',
                colonPos + 1
            );

        if (openPos == std::string::npos)
            return false;

        int depth = 0;

        for (size_t i = openPos;
             i < json.size();
             ++i)
        {
            if (json[i] == '{')
            {
                ++depth;
            }
            else if (json[i] == '}')
            {
                --depth;

                if (depth == 0)
                {
                    outBody =
                        json.substr(
                            openPos + 1,
                            i - openPos - 1
                        );

                    return true;
                }
            }
        }

        return false;
    }

    bool FindNumber(
        const std::string& objectBody,
        const char* key,
        double& outValue)
    {
        const std::string quotedKey =
            std::string("\"") + key + "\"";

        const size_t keyPos =
            objectBody.find(quotedKey);

        if (keyPos == std::string::npos)
            return false;

        const size_t colonPos =
            objectBody.find(
                ':',
                keyPos + quotedKey.size()
            );

        if (colonPos == std::string::npos)
            return false;

        const char* start =
            objectBody.c_str() +
            colonPos +
            1;

        while (*start &&
               std::isspace(
                   static_cast<unsigned char>(
                       *start)))
        {
            ++start;
        }

        char* end = nullptr;

        const double value =
            std::strtod(start, &end);

        if (end == start ||
            !std::isfinite(value))
        {
            return false;
        }

        outValue = value;
        return true;
    }

    bool LoadStadiumDornaConfig(
        const std::string& stadiumId,
        StadiumDornaConfig& outConfig)
    {
        outConfig = {};

        const std::string path =
            BuildStadiumJsonPath(stadiumId);

        Log(
            "Looking for stadium config: %s",
            path.c_str()
        );

        std::ifstream file(
            path,
            std::ios::in |
            std::ios::binary
        );

        if (!file)
        {
            Log(
                "No stadium config found for '%s'. Using EA defaults.",
                stadiumId.c_str()
            );

            return false;
        }

        Log(
            "Found stadium config for '%s'.",
            stadiumId.c_str()
        );

        std::ostringstream stream;
        stream << file.rdbuf();

        std::string dornaBody;

        if (!FindObjectBody(
                stream.str(),
                "dorna",
                dornaBody))
        {
            Log(
                "Config '%s' has no dorna object. Using EA defaults.",
                stadiumId.c_str()
            );

            return true;
        }

        double value = 0.0;

        if (FindNumber(
                dornaBody,
                "ad_count",
                value))
        {
            const int count =
                static_cast<int>(
                    std::lround(value)
                );

            if (count >= 1 &&
                count <= 127)
            {
                outConfig.hasAdCount = true;
                outConfig.adCount = count;

                Log(
                    "  ad_count = %d",
                    count
                );
            }
        }

        if (FindNumber(
                dornaBody,
                "period",
                value) &&
            value > 0.0)
        {
            outConfig.hasPeriod = true;
            outConfig.period =
                static_cast<float>(value);

            Log(
                "  period = %.4f",
                outConfig.period
            );
        }

        if (FindNumber(
                dornaBody,
                "transition_period",
                value) &&
            value >= 0.0)
        {
            outConfig.hasTransitionPeriod = true;
            outConfig.transitionPeriod =
                static_cast<float>(value);

            Log(
                "  transition_period = %.4f",
                outConfig.transitionPeriod
            );
        }

        return true;
    }

    // ------------------------------------------------------------
    // Shared Dorna runtime
    // ------------------------------------------------------------

    void CaptureOriginalTiming()
    {
        if (gGame.usesFloatTiming)
        {
            Log(
                "Original NBA Live %d timing: period=%.4fs transition=%.4fs",
                gGame.game,
                gGame.originalPeriodSeconds,
                gGame.originalTransitionSeconds
            );
            return;
        }

        if (gOriginalPeriodTicks <= 0)
        {
            gOriginalPeriodTicks =
                *reinterpret_cast<const int*>(
                    gGame.periodTicksAddress
                );
        }

        if (gOriginalTransitionTicks <= 0)
        {
            gOriginalTransitionTicks =
                *reinterpret_cast<const int*>(
                    gGame.transitionTicksAddress
                );
        }

        if (gOriginalPeriodTicks <= 0 ||
            gOriginalPeriodTicks > 100000)
        {
            gOriginalPeriodTicks = 516;
        }

        if (gOriginalTransitionTicks <= 0 ||
            gOriginalTransitionTicks >
                gOriginalPeriodTicks)
        {
            gOriginalTransitionTicks = 37;
        }

        Log(
            "Original NBA Live %d timing: periodTicks=%d transitionTicks=%d",
            gGame.game,
            gOriginalPeriodTicks,
            gOriginalTransitionTicks
        );
    }

    bool ApplyLogicalAdCount(
        int adCount)
    {
        if (adCount < 1 ||
            adCount > 127)
        {
            return false;
        }

        uint8_t patchBytes[7] = {
            0x90, 0x90, 0x90, 0x90,
            0x90, 0x90, 0x90
        };

        patchBytes[0] = 0x6B;

        if (gGame.game == GAME_2007 ||
            gGame.game == GAME_2008)
        {
            // 07/08:
            //   lea ecx, ds:0[esi*4]
            // =>
            //   imul ecx, esi, AD_COUNT
            patchBytes[1] = 0xCE;
        }
        else
        {
            // 05/06:
            //   lea esi, ds:0[ecx*4]
            // =>
            //   imul esi, ecx, AD_COUNT
            patchBytes[1] = 0xF1;
        }

        patchBytes[2] =
            static_cast<uint8_t>(adCount);

        return PatchBytes(
            gGame.logicalAdCountPatch,
            patchBytes,
            sizeof(patchBytes)
        );
    }

    void ApplyCurrentDornaTiming()
    {
        CaptureOriginalTiming();

        gDornaScrollStep =
            1.0f /
            static_cast<float>(
                gCurrentAdCount
            );

        ApplyLogicalAdCount(
            gCurrentAdCount
        );

        if (gGame.usesFloatTiming)
        {
            gDornaPeriodSecondsRuntime =
                gCurrentPeriodSeconds;

            gDornaTransitionSecondsRuntime =
                gCurrentTransitionSeconds;

            Log(
                "Applied Dorna runtime: game=%d stadium='%s' adCount=%d step=%.8f period=%.4fs transition=%.4fs",
                gGame.game,
                gCurrentStadiumId.c_str(),
                gCurrentAdCount,
                gDornaScrollStep,
                gDornaPeriodSecondsRuntime,
                gDornaTransitionSecondsRuntime
            );

            return;
        }

        gLegacyPeriodTicks =
            static_cast<int>(
                std::lround(
                    static_cast<double>(
                        gOriginalPeriodTicks
                    ) *
                    (
                        static_cast<double>(
                            gCurrentPeriodSeconds
                        ) /
                        static_cast<double>(
                            kDefaultPeriodSeconds
                        )
                    )
                )
            );

        if (gLegacyPeriodTicks < 1)
            gLegacyPeriodTicks = 1;

        if (std::fabs(
                gCurrentTransitionSeconds -
                kDefaultTransitionSeconds) <
            0.0001f)
        {
            gLegacyTransitionTicks =
                gOriginalTransitionTicks;
        }
        else if (
            gCurrentTransitionSeconds <= 0.0f)
        {
            gLegacyTransitionTicks = 0;
        }
        else
        {
            const double originalTransitionSeconds =
                static_cast<double>(
                    gOriginalTransitionTicks
                ) *
                (
                    static_cast<double>(
                        kDefaultPeriodSeconds
                    ) /
                    static_cast<double>(
                        gOriginalPeriodTicks
                    )
                );

            gLegacyTransitionTicks =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(
                            gOriginalTransitionTicks
                        ) *
                        (
                            static_cast<double>(
                                gCurrentTransitionSeconds
                            ) /
                            originalTransitionSeconds
                        )
                    )
                );
        }

        if (gLegacyTransitionTicks < 0)
            gLegacyTransitionTicks = 0;

        if (gLegacyTransitionTicks >
            gLegacyPeriodTicks)
        {
            gLegacyTransitionTicks =
                gLegacyPeriodTicks;
        }

        Log(
            "Applied Dorna runtime: game=%d stadium='%s' adCount=%d step=%.8f period=%.4fs (%d ticks) transition=%.4fs (%d ticks)",
            gGame.game,
            gCurrentStadiumId.c_str(),
            gCurrentAdCount,
            gDornaScrollStep,
            gCurrentPeriodSeconds,
            gLegacyPeriodTicks,
            gCurrentTransitionSeconds,
            gLegacyTransitionTicks
        );
    }

    void RestoreDefaultDornaConfig()
    {
        gCurrentAdCount =
            kDefaultAdCount;

        gCurrentPeriodSeconds =
            kDefaultPeriodSeconds;

        gCurrentTransitionSeconds =
            kDefaultTransitionSeconds;

        ApplyCurrentDornaTiming();
    }

    void LoadAndApplyStadiumConfig(
        const char* stadiumShortName)
    {
        gCurrentStadiumId =
            NormalizeStadiumId(
                stadiumShortName
            );

        // Always reset first so a missing JSON cannot inherit
        // settings from the previous arena.
        RestoreDefaultDornaConfig();

        if (gCurrentStadiumId.empty())
            return;

        StadiumDornaConfig config{};

        if (!LoadStadiumDornaConfig(
                gCurrentStadiumId,
                config))
        {
            return;
        }

        if (config.hasAdCount)
        {
            gCurrentAdCount =
                config.adCount;
        }

        if (config.hasPeriod)
        {
            gCurrentPeriodSeconds =
                config.period;
        }

        if (config.hasTransitionPeriod)
        {
            gCurrentTransitionSeconds =
                config.transitionPeriod;
        }

        if (gCurrentTransitionSeconds >
            gCurrentPeriodSeconds)
        {
            gCurrentTransitionSeconds =
                gCurrentPeriodSeconds;
        }

        ApplyCurrentDornaTiming();
    }

    void InstallSharedUpdatePatches()
    {
        CaptureOriginalTiming();

        const int scrollRefs =
            RedirectFloatReferences(
                gGame.updateFunction,
                gGame.updateScanSize,
                0.25f,
                &gDornaScrollStep
            );

        int periodRefs = 0;
        int transitionRefs = 0;

        if (gGame.usesFloatTiming)
        {
            periodRefs =
                RedirectFloatReferences(
                    gGame.updateFunction,
                    gGame.updateScanSize,
                    gGame.originalPeriodSeconds,
                    &gDornaPeriodSecondsRuntime
                );

            transitionRefs =
                RedirectFloatReferences(
                    gGame.updateFunction,
                    gGame.updateScanSize,
                    gGame.originalTransitionSeconds,
                    &gDornaTransitionSecondsRuntime
                );
        }
        else
        {
            periodRefs =
                RedirectAbsoluteAddressReferences(
                    gGame.updateFunction,
                    gGame.updateScanSize,
                    gGame.periodTicksAddress,
                    &gLegacyPeriodTicks
                );

            transitionRefs =
                RedirectAbsoluteAddressReferences(
                    gGame.updateFunction,
                    gGame.updateScanSize,
                    gGame.transitionTicksAddress,
                    &gLegacyTransitionTicks
                );
        }

        Log(
            "Update redirects installed: game=%d scrollRefs=%d periodRefs=%d transitionRefs=%d",
            gGame.game,
            scrollRefs,
            periodRefs,
            transitionRefs
        );

        RestoreDefaultDornaConfig();
    }

    // ============================================================
    // NBA Live 05
    // ============================================================

    constexpr uintptr_t kSetupDornaBoardsCallSite05 =
        0x005FD785;

    using SetupDornaBoards05_t =
        void (__thiscall*)(
            void* stadium,
            const char* strUBIPath,
            const char* stadiumShortName,
            void* options
        );

    SetupDornaBoards05_t
        gOriginalSetupDornaBoards05 = nullptr;

    void __fastcall SetupDornaBoards05_Hook(
        void* stadium,
        void*,
        const char* strUBIPath,
        const char* stadiumShortName,
        void* options)
    {
        Log(
            "NBA Live 05 GameStadium::Setup detected stadium: arg='%s' this+0x58='%s'",
            stadiumShortName
                ? stadiumShortName
                : "<null>",
            stadium
                ? reinterpret_cast<const char*>(
                    reinterpret_cast<uintptr_t>(
                        stadium) + 0x58)
                : "<null>"
        );

        LoadAndApplyStadiumConfig(
            stadiumShortName
        );

        gOriginalSetupDornaBoards05(
            stadium,
            strUBIPath,
            stadiumShortName,
            options
        );
    }

    void InstallStadiumConfig05()
    {
        gGame = {
            GAME_2005,
            0x005FD7B0, // GameStadium::Update
            0x180,
            0x005FD7EF, // lea esi,[ecx*4]
            0x00C4AAA8, // period ticks
            0x00C4A998, // transition ticks
            false,
            kDefaultPeriodSeconds,
            kDefaultTransitionSeconds
        };

        gOriginalPeriodTicks = 0;
        gOriginalTransitionTicks = 0;

        Log(
            "Initializing NBA Live 05 StadiumConfig."
        );

        InstallSharedUpdatePatches();

        const uintptr_t target =
            ReadRelativeCallTarget(
                kSetupDornaBoardsCallSite05
            );

        if (!target)
        {
            Log(
                "ERROR: Could not resolve NBA Live 05 SetupDornaBoards call target at 0x%08X.",
                static_cast<unsigned int>(
                    kSetupDornaBoardsCallSite05)
            );

            return;
        }

        gOriginalSetupDornaBoards05 =
            reinterpret_cast<
                SetupDornaBoards05_t>(
                    target);

        patch::RedirectCall(
            kSetupDornaBoardsCallSite05,
            SetupDornaBoards05_Hook
        );

        Log(
            "NBA Live 05 SetupDornaBoards hook installed: callSite=0x%08X target=0x%08X",
            static_cast<unsigned int>(
                kSetupDornaBoardsCallSite05),
            static_cast<unsigned int>(
                target)
        );
    }

    // ============================================================
    // NBA Live 06
    // ============================================================

    constexpr uintptr_t
        kLoadArenaLightingSettings06 =
            0x005FCEB0;

    constexpr uintptr_t
        kSetupLightingCallSite06 =
            0x0060AF0F;

    using LoadArenaLightingSettings06_t =
        void (__thiscall*)(
            void* stadium,
            const char* stadiumShortName
        );

    LoadArenaLightingSettings06_t
        gOriginalLoadArenaLightingSettings06 =
            reinterpret_cast<
                LoadArenaLightingSettings06_t>(
                    kLoadArenaLightingSettings06
                );

    void __fastcall
    LoadArenaLightingSettings06_Hook(
        void* stadium,
        void*,
        const char* stadiumShortName)
    {
        Log(
            "NBA Live 06 GameStadium::Setup detected stadium: arg='%s' this+0x58='%s'",
            stadiumShortName
                ? stadiumShortName
                : "<null>",
            stadium
                ? reinterpret_cast<const char*>(
                    reinterpret_cast<uintptr_t>(
                        stadium) + 0x58)
                : "<null>"
        );

        gOriginalLoadArenaLightingSettings06(
            stadium,
            stadiumShortName
        );

        LoadAndApplyStadiumConfig(
            stadiumShortName
        );
    }

    void InstallStadiumConfig06()
    {
        gGame = {
            GAME_2006,
            0x0060C3A0, // GameStadium::Update
            0x180,
            0x0060C3DF, // lea esi,[ecx*4]
            0x00CB37D4, // period ticks
            0x00CB3A68, // transition ticks
            false,
            kDefaultPeriodSeconds,
            kDefaultTransitionSeconds
        };

        gOriginalPeriodTicks = 0;
        gOriginalTransitionTicks = 0;

        Log(
            "Initializing NBA Live 06 StadiumConfig."
        );

        InstallSharedUpdatePatches();

        patch::RedirectCall(
            kSetupLightingCallSite06,
            LoadArenaLightingSettings06_Hook
        );

        Log(
            "NBA Live 06 Setup hook installed at 0x%08X.",
            static_cast<unsigned int>(
                kSetupLightingCallSite06)
        );
    }
    // ============================================================
    // NBA Live 07
    // ============================================================

    constexpr uintptr_t kSetupDornaCallSite07 =
        0x0069466C;

    constexpr uintptr_t kSetupDornaFunction07 =
        0x00686E10;

    using SetupDorna07_t =
        int (__thiscall*)(
            void* dornaObject,
            const char* stadiumTeamAbbr,
            void* gameContext
        );

    SetupDorna07_t gOriginalSetupDorna07 =
        reinterpret_cast<SetupDorna07_t>(
            kSetupDornaFunction07
        );

    int __fastcall SetupDorna07_Hook(
        void* dornaObject,
        void*,
        const char* stadiumTeamAbbr,
        void* gameContext)
    {
        Log(
            "NBA Live 07 Dorna setup detected stadium: arg='%s'",
            stadiumTeamAbbr
                ? stadiumTeamAbbr
                : "<null>"
        );

        LoadAndApplyStadiumConfig(
            stadiumTeamAbbr
        );

        return gOriginalSetupDorna07(
            dornaObject,
            stadiumTeamAbbr,
            gameContext
        );
    }

    void InstallStadiumConfig07()
    {
        gGame = {
            GAME_2007,
            0x00686A90, // GameStadium::UpdateAdBoardScroll
            0x220,
            0x00686B0A, // lea ecx,[esi*4]
            0,
            0,
            true,
            8.6000004f,
            0.60000002f
        };

        gOriginalPeriodTicks = 0;
        gOriginalTransitionTicks = 0;

        gDornaPeriodSecondsRuntime =
            gGame.originalPeriodSeconds;

        gDornaTransitionSecondsRuntime =
            gGame.originalTransitionSeconds;

        Log(
            "Initializing NBA Live 07 StadiumConfig."
        );

        InstallSharedUpdatePatches();

        patch::RedirectCall(
            kSetupDornaCallSite07,
            SetupDorna07_Hook
        );

        Log(
            "NBA Live 07 Dorna setup hook installed at 0x%08X -> 0x%08X.",
            static_cast<unsigned int>(
                kSetupDornaCallSite07),
            static_cast<unsigned int>(
                kSetupDornaFunction07)
        );
    }

    // ============================================================
    // NBA Live 08
    // ============================================================

    constexpr uintptr_t kSetupDornaCallSite08 =
        0x006BB607;

    constexpr uintptr_t kSetupDornaFunction08 =
        0x006AAF50;

    using SetupDorna08_t =
        int (__thiscall*)(
            void* dornaObject,
            const char* stadiumTeamAbbr,
            void* gameContext
        );

    SetupDorna08_t gOriginalSetupDorna08 =
        reinterpret_cast<SetupDorna08_t>(
            kSetupDornaFunction08
        );

    int __fastcall SetupDorna08_Hook(
        void* dornaObject,
        void*,
        const char* stadiumTeamAbbr,
        void* gameContext)
    {
        Log(
            "NBA Live 08 Dorna setup detected stadium: arg='%s'",
            stadiumTeamAbbr
                ? stadiumTeamAbbr
                : "<null>"
        );

        LoadAndApplyStadiumConfig(
            stadiumTeamAbbr
        );

        return gOriginalSetupDorna08(
            dornaObject,
            stadiumTeamAbbr,
            gameContext
        );
    }

    void InstallStadiumConfig08()
    {
        gGame = {
            GAME_2008,
            0x006AAB20, // GameStadium::UpdateAdBoardScroll
            0x240,
            0x006AABCD, // lea ecx,[esi*4]
            0,
            0,
            true,
            8.6000004f,
            0.60000002f
        };

        gOriginalPeriodTicks = 0;
        gOriginalTransitionTicks = 0;

        gDornaPeriodSecondsRuntime =
            gGame.originalPeriodSeconds;

        gDornaTransitionSecondsRuntime =
            gGame.originalTransitionSeconds;

        Log(
            "Initializing NBA Live 08 StadiumConfig."
        );

        InstallSharedUpdatePatches();

        patch::RedirectCall(
            kSetupDornaCallSite08,
            SetupDorna08_Hook
        );

        Log(
            "NBA Live 08 Dorna setup hook installed at 0x%08X -> 0x%08X.",
            static_cast<unsigned int>(
                kSetupDornaCallSite08),
            static_cast<unsigned int>(
                kSetupDornaFunction08)
        );
    }

}


// ============================================================
// Public entry point
// ============================================================

void InitializeStadiumDornaConfig()
{
    InitializeDebugLogging();

    switch (FM::GetEntryPoint())
    {
    case 0xCD8005:
        // NBA Live 2005
        InstallStadiumConfig05();
        break;

    case 0x40109F:
        if (patch::GetFloat(
                0xBD832C) == 1.3333334f)
        {
            // NBA Live 06
            InstallStadiumConfig06();
        }
        else if (patch::GetFloat(
                     0xBBBC3C) == 1.3333334f)
        {
            // NBA Live 07
            InstallStadiumConfig07();
        }
        else if (patch::GetFloat(
                     0xC3DF84) == 1.3333334f)
        {
            // NBA Live 08
            InstallStadiumConfig08();
        }
        break;

    default:
        break;
    }
}
