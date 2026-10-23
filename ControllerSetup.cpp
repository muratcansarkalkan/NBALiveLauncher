#include "plugin-std.h"
#include "Games.h"
#include "ControllerSetup.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <cstdint>
#include <climits>
#include <cstring>
#include <ShlObj.h>
#pragma comment(lib, "Shell32.lib")

using namespace plugin;

namespace {
    LiveControllerSetup::Settings g_settings;
    constexpr const char* kIniFile = ".\\main.ini";
    struct ControllerGameApi {
        const char* name = nullptr;
        const char* documentsFolder = nullptr;

        uintptr_t nativeSetSide = 0;
        uintptr_t nativeLoadConfig = 0;
        uintptr_t nativeSelectConfig = 0;
        uintptr_t nativeManagerAccessor = 0;

        uintptr_t nativeCommandSingleton = 0;
        uintptr_t nativeCommandSubmit = 0;
        uintptr_t nativeCommandDrain = 0;
        uintptr_t bballStringConstructor = 0;
        uintptr_t bballStringDestructor = 0;

        uintptr_t inputConfigManagerGlobal = 0;
        uintptr_t deviceManagerGlobal = 0;

        uintptr_t nativeConfigCall = 0;
        uintptr_t frontEndInitCall = 0;
        uintptr_t frontEndInit = 0;
        uintptr_t gameModeGlobal = 0;

        // NBA Live 2005's native SelectConfig path executes the selected
        // profile itself through sub_61ADC0.  NBA Live 06 keeps the existing
        // proven manual-exec + selection-sync sequence.
        bool selectConfigExecutesProfile = false;
        bool watchFrontEndReady = false;
        uintptr_t reserved07[3] = {};
        uintptr_t nativeGetPlayerLock = 0;
        uintptr_t nativeSetPlayerLock = 0;
    };

    ControllerGameApi g_api{};

    bool ResolveControllerGameApi() {
        const uintptr_t entryPoint = FM::GetEntryPoint();

        if (entryPoint == 0xCD8005) {
            g_api = {
                "NBA Live 2005",
                "NBA LIVE 2005",
                0x0059C6C0, // SetControllerSide
                0x00619970, // config/profile loader
                0x0061C200, // native SelectConfig(deviceIndex,catalogueId)
                0x0061C490, // catalogue manager accessor

                0, 0, 0, 0, 0, // not needed: native SelectConfig executes profile

                0x00C4E8BC, // input-config manager global
                0x00C4E8C8, // device manager global

                0x00619CEB, // enumeration loader callsite
                0x005D0AE8, // FE init callsite
                0x005D0840, // FE init target
                0x00C49D34, // FEUberGameMode singleton

                true,
                false,
                { 0, 0, 0 }
            };
            g_api.nativeGetPlayerLock = 0x004C8940;
            g_api.nativeSetPlayerLock = 0x004C89F0;
            return true;
        }

        if (entryPoint == 0x40109F &&
            patch::GetFloat(0xBD832C) == 1.3333334f) {
            g_api = {
                "NBA Live 06",
                "NBA LIVE 06",
                0x005B7C40,
                0x00628C20,
                0x0062B930,
                0x0062BBC0,

                0x00475790,
                0x00475010,
                0x00475550,
                0x004152B0,
                0x00415430,

                0x00CB75B4,
                0x00CB75C0,

                0x00628FBB,
                0x005EB305,
                0x005DDF70,
                0x00CB2A2C,

                false,
                false,
                { 0, 0, 0 }
            };
            g_api.nativeGetPlayerLock = 0x004BF0E0;
            g_api.nativeSetPlayerLock = 0x004BF190;
            return true;
        }

        if (entryPoint == 0x40109F) {
            const unsigned char* selector =
                reinterpret_cast<const unsigned char*>(0x00662380);

            if (selector[0] == 0x56 &&
                selector[1] == 0x8B &&
                selector[2] == 0xF1) {
                g_api = {
                    "NBA Live 07",
                    "NBA LIVE 07",
                    0x005945F0,
                    0x0065FC90,
                    0x00662380,
                    0x00662610,

                    0, 0, 0, 0, 0,

                    0x00CEF09C,
                    0x00CEF0A8,

                    0x0066000B,
                    0,
                    0,
                    0x00CEB6F4,

                    true,
                    true,
                    { 0, 0, 0 }
                };
                g_api.nativeGetPlayerLock = 0x004EE7E0;
                g_api.nativeSetPlayerLock = 0x004EE890;
                return true;
            }
        }

        if (entryPoint == 0x40109F) {
            const unsigned char* selector08 =
                reinterpret_cast<const unsigned char*>(0x00684D80);

            // NBA Live 08 native ControllerConfig::SetControllerConfig routes
            // through 00684D80, whose prologue is push esi / mov esi,ecx.
            if (selector08[0] == 0x56 &&
                selector08[1] == 0x8B &&
                selector08[2] == 0xF1) {
                g_api = {
                    "NBA Live 08",
                    "NBA LIVE 08",

                    0x005B51E0, // native SetControllerSide(slot, side)
                    0x00681250, // native config/profile loader
                    0x00684D80, // SelectConfig(deviceIndex, catalogueId)
                    0x00685000, // catalogue manager accessor

                    0, 0, 0, 0, 0, // SelectConfig executes the selected profile

                    0x00DAAF54, // input-config manager global
                    0x00DAAF60, // device manager global

                    0x0068162B, // native enumeration loader callsite
                    0,
                    0,
                    0x00DA7214, // FEUberGameMode singleton

                    true,       // native SelectConfig executes profile
                    true,       // wait for FEUberGameMode + 0x58 readiness
                    { 0, 0, 0 }
                };
                g_api.nativeGetPlayerLock = 0x004F2D70;
                g_api.nativeSetPlayerLock = 0x004F2E20;
                return true;
            }
        }

        g_api = {};
        return false;
    }

    using SelectConfigFn = void (__thiscall*)(void*, int /*deviceIndex*/, int /*catalogueId*/);
    using ConfigManagerAccessorFn = void* (__cdecl*)();
    bool g_syncNativeSelection = true;
    using LoadConfigFn = unsigned char (__thiscall *)(void*, unsigned int, const char*);
    constexpr unsigned kKeyboardNativeSlot = 7;
    constexpr unsigned kKeyboardEntryKey = 1;
    using SetSideFn = void (__stdcall *)(unsigned int, int);
    // Both native player-lock APIs accept *user slots* 0..7. They internally
    // map joystick slots 0..6 to native input indexes +3, and keyboard 7 to 1.
    // This is the same user-slot domain as SetControllerSide, *not* the
    // separate 0..24 configuration-loader index or the profile catalogue ID.
    void Log(const char* message);
    using GetPlayerLockFn = int (__stdcall *)(unsigned int);
    using SetPlayerLockFn = void (__stdcall *)(unsigned int, int);
    int ReadPlayerLockSetting(const char* section) {
        const int mode = GetPrivateProfileIntA(section, "PLAYER_LOCK", -1, kIniFile);
        if (mode >= -1 && mode <= 4) return mode;
        char msg[180] = {};
        sprintf_s(msg, sizeof(msg),
            "[ControllerSetup] %s PLAYER_LOCK=%d invalid; use -1 or 0..4.\n",
            section, mode);
        Log(msg);
        return -1;
    }

    void ApplyPlayerLock(const char* label, unsigned int userSlot, int mode, int side) {
        if (mode < 0) return; // -1 means leave EA selection unchanged.
        if (side != 0 && side != 1) {
            char msg[180] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] %s player lock skipped: controller not assigned HOME/AWAY.\n", label);
            Log(msg);
            return;
        }
        if (userSlot > 7 || !g_api.nativeGetPlayerLock || !g_api.nativeSetPlayerLock)
            return;
        auto getter = reinterpret_cast<GetPlayerLockFn>(g_api.nativeGetPlayerLock);
        auto setter = reinterpret_cast<SetPlayerLockFn>(g_api.nativeSetPlayerLock);
        const int before = getter(userSlot);
        if (before != mode) setter(userSlot, mode);
        const int after = getter(userSlot);
        char msg[256] = {};
        sprintf_s(msg, sizeof(msg),
            "[ControllerSetup] %s PLAYER_LOCK userSlot=%u side=%d requested=%d before=%d after=%d %s.\n",
            label, userSlot, side, mode, before, after,
            after == mode ? "APPLIED" : "NOT_CONFIRMED");
        Log(msg);
    }
    int g_keyboardSide = -1;
    int g_keyboardPlayerLock = -1;
    std::string g_keyboardConfig;
    int g_keyboardConfigDeviceIndex = -1;

    struct JoystickOverride {
        bool enabled = false;
        int nativeSlot = -1;
        int side = -1;
        int playerLock = -1;
        std::string config;
        int configDeviceIndex = -1;
    };

    JoystickOverride g_joysticks[4];
    bool g_nativeConfigSeen[25] = {};
    bool g_nativeConfigLoadSucceeded[25] = {};
    using FrontEndInitFn = void (__thiscall *)(void*);
    bool g_deferredSelectionSyncDone = false;

    void Log(const char* message);
    std::string GetString(const char* section, const char* key, const char* fallback);
    std::string Lower(std::string);
    bool ValidConfigName(const std::string&, LiveControllerSetup::DeviceKind);
    bool FileExists(const std::string&);
    std::string ResolveConfigDirectory();

    // Native BBallString stores a pointer to its reference-counted character data.
    // The constructor/destructor must be used; a raw char* is not a BBallString.
    struct NativeBBallString { const char* characters = nullptr; };
    using NativeStringConstructorFn = void* (__thiscall*)(void*, const char*);
    using NativeStringDestructorFn = void (__thiscall*)(void*);
    using NativeCommandSingletonFn = void* (__cdecl*)();
    using NativeCommandSubmitFn = unsigned char (__thiscall*)(void*, const NativeBBallString*);
    using NativeCommandDrainFn = void (__thiscall*)(void*);

    bool ExecuteSelectedProfile(const char* label, int configIndex,
                                const std::string& filename) {
        if (g_api.selectConfigExecutesProfile) {
            Log("[ControllerSetup] Manual exec skipped: this game applies the profile through native SelectConfig.\n");
            return false;
        }
        if (configIndex < 0 || configIndex >= 25 || filename.empty())
            return false;
        if (!g_nativeConfigLoadSucceeded[configIndex]) {
            char msg[256] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] %s: exec skipped: native config index %d was not loaded successfully.\n",
                label, configIndex);
            Log(msg);
            return false;
        }
        const std::string path = ResolveConfigDirectory() + filename;
        if (!FileExists(path)) {
            std::string msg = "[ControllerSetup] " + std::string(label) +
                ": exec skipped: file missing: " + path + "\n";
            Log(msg.c_str());
            return false;
        }
        // ValidConfigName rejects quotes and separators in the filename;
        // reject quotes in the resolved directory too, because exec is a command.
        if (path.find('"') != std::string::npos) {
            Log("[ControllerSetup] exec skipped: quote in configuration directory.\n");
            return false;
        }
        const std::string commandText = "exec \"" + path + "\"";
        NativeBBallString command;
        reinterpret_cast<NativeStringConstructorFn>(g_api.bballStringConstructor)(
            &command, commandText.c_str());
        bool success = false;
        void* processor = reinterpret_cast<NativeCommandSingletonFn>(
            g_api.nativeCommandSingleton)();
        if (processor) {
            const unsigned char accepted =
                reinterpret_cast<NativeCommandSubmitFn>(g_api.nativeCommandSubmit)(
                    processor, &command);
            // The frontend performs a drain even after the submit call.
            processor = reinterpret_cast<NativeCommandSingletonFn>(
                g_api.nativeCommandSingleton)();
            if (processor)
                reinterpret_cast<NativeCommandDrainFn>(g_api.nativeCommandDrain)(processor);
            char msg[1000] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] %s: native command accepted=%u: %s\n",
                label, static_cast<unsigned>(accepted), commandText.c_str());
            Log(msg);
            success = accepted != 0;
        } else {
            Log("[ControllerSetup] exec skipped: command processor unavailable.\n");
        }
        reinterpret_cast<NativeStringDestructorFn>(g_api.bballStringDestructor)(&command);
        return success;
    }

    // Native FE records: manager+0x10 is a linked list of {id, BBallString,
    // ..., next at +0x28}.  The string at node+4 is a BBallString object,
    // NOT the filename contained in the input loader's separate record.
    // Resolve the requested name against this catalogue before selecting.
    // Only select an existing ID; never create or alter the game's config files.
    bool ReadableRange(const void* address, size_t length) {
        if (!address || !length) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(address, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const DWORD protection = mbi.Protect & 0xFF;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
            return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(address);
        const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return p <= end && length <= end - p;
    }

    bool ReadNativeName(const unsigned char* node, std::string& result) {
        if (!ReadableRange(node, 0x2C)) return false;
        const char* characters = *reinterpret_cast<const char* const*>(node + 4);
        if (!ReadableRange(characters, 1)) return false;
        // Native strings contain a length header immediately before char data.
        // Read chars conservatively one at a time, bounded by max filename size.
        result.clear();
        for (size_t i = 0; i <= 100; ++i) {
            if (!ReadableRange(characters + i, 1)) return false;
            const char c = characters[i];
            if (!c) return !result.empty();
            if (static_cast<unsigned char>(c) < 0x20 ||
                static_cast<unsigned char>(c) > 0x7E) return false;
            result.push_back(c);
        }
        return false;
    }

    bool SynchronizeVerifiedNativeSelection(const char* label, int configIndex,
                                             const std::string& filename) {
        if (!g_syncNativeSelection) return false;
        if (configIndex < 0 || configIndex >= 25 || filename.empty()) return false;
        auto* manager = reinterpret_cast<ConfigManagerAccessorFn>(g_api.nativeManagerAccessor)();
        if (!ReadableRange(manager, 0x14)) {
            Log("[ControllerSetup] Native selection sync skipped: manager unavailable.\n");
            return false;
        }
        auto* node = *reinterpret_cast<unsigned char**>(
            reinterpret_cast<unsigned char*>(manager) + 0x10);
        int selectedId = -1;
        for (int i = 0; node && i < 128; ++i) {
            if (!ReadableRange(node, 0x2C)) {
                Log("[ControllerSetup] Native catalogue traversal stopped: unreadable node.\n");
                break;
            }

            // Catalogue node layout shared by the verified 05/06/07/08
            // native selectors:
            //   +0x00 = catalogue/profile ID
            //   +0x04 = BBallString profile name
            //   +0x28 = next node
            const int id = *reinterpret_cast<const int*>(node);

            std::string name;
            if (ReadNativeName(node, name)) {
                char msg[288] = {};
                sprintf_s(msg, sizeof(msg),
                    "[ControllerSetup] Native catalogue entry id=%d name=%s\n",
                    id, name.c_str());
                Log(msg);

                if (_stricmp(name.c_str(), filename.c_str()) == 0) {
                    selectedId = id;
                    break;
                }
            }

            auto* next = *reinterpret_cast<unsigned char**>(node + 0x28);
            if (next == node)
                break;
            node = next;
        }

        if (selectedId < 0) {
            char msg[256] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] %s: FE selection sync skipped: profile '%s' was not found in the native catalogue.\n",
                label, filename.c_str());
            Log(msg);
            return false;
        }

        char msg[256] = {};
        sprintf_s(msg, sizeof(msg),
            "[ControllerSetup] %s: synchronizing native selection: deviceIndex=%d catalogueId=%d (%s).\n",
            label, configIndex, selectedId, filename.c_str());
        Log(msg);

        // ABI proven from 0x62B930:
        //   this = controller catalogue manager
        //   arg0 = native device/config index
        //   arg4 = catalogue/profile ID
        //
        // The older experimental build had these two arguments reversed.
        reinterpret_cast<SelectConfigFn>(g_api.nativeSelectConfig)(
            manager,
            configIndex,
            selectedId);

        Log("[ControllerSetup] Native FE/gameplay profile selection synchronized.\n");
        return true;
    }

    // This index is from the native input-config manager (0..24), not
    // SetControllerSide's user slot (0..7). Never infer one from the other.
    void ApplyNativeConfig(const char* label, int configIndex,
                           const std::string& filename) {
        if (filename.empty() || configIndex < 0)
            return;
        if (configIndex > 24) {
            Log("[ControllerSetup] Config skipped: CONFIG_DEVICE_INDEX must be 0..24.\n");
            return;
        }
        const std::string path = ResolveConfigDirectory() + filename;
        if (!FileExists(path)) {
            char msg[768] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] %s config missing: %s\n", label, path.c_str());
            Log(msg);
            return;
        }
        auto* configMgr = *reinterpret_cast<void* const*>(g_api.inputConfigManagerGlobal);
        auto* deviceMgr = *reinterpret_cast<void* const*>(g_api.deviceManagerGlobal);
        if (!configMgr || !deviceMgr) {
            Log("[ControllerSetup] Config skipped: native input managers not initialized.\n");
            return;
        }
        // The native loader prepends "configs/" to filename and uses the
        // game's virtual file system. Directory selection here is ONLY a
        // preflight check, not an override of the game's VFS resolution.
        char msg[768] = {};
        sprintf_s(msg, sizeof(msg),
            "[ControllerSetup] %s: loading %s for config device index %d (manager=%p).\n",
            label, filename.c_str(), configIndex, configMgr);
        Log(msg);
        const auto result = reinterpret_cast<LoadConfigFn>(g_api.nativeLoadConfig)(
            configMgr, static_cast<unsigned int>(configIndex), filename.c_str());
        sprintf_s(msg, sizeof(msg),
            "[ControllerSetup] %s: native config loader returned %u.\n",
            label, static_cast<unsigned>(result));
        Log(msg);
        g_nativeConfigLoadSucceeded[configIndex] = result != 0;
    }

    // The original device enumeration issues a thiscall to the game-specific
    // native config loader at the game-specific enumeration callsite. __fastcall wrapper receives ECX manager and unused EDX,
    // with the two stack arguments still cleaned by RET 8.
    unsigned char __fastcall NativeConfigSelectionHook(
        void* manager, void*, unsigned int deviceIndex, const char* nativeFilename) {
        const char* filename = nativeFilename;
        const char* source = "native";
        if (g_settings.enabled && deviceIndex < 25) {
            g_nativeConfigSeen[deviceIndex] = true;
            // No dependence on DirectInput product name: match the actual
            // configuration-device index captured in the debugger.
            if (g_keyboardConfigDeviceIndex == static_cast<int>(deviceIndex) &&
                !g_keyboardConfig.empty() &&
                FileExists(ResolveConfigDirectory() + g_keyboardConfig)) {
                filename = g_keyboardConfig.c_str();
                source = "KEYBOARD";
            }
            for (int i = 0; i < 4; ++i) {
                const auto& pad = g_joysticks[i];
                if (!pad.enabled || pad.configDeviceIndex != static_cast<int>(deviceIndex) ||
                    pad.config.empty())
                    continue;
                if (!FileExists(ResolveConfigDirectory() + pad.config)) {
                    char missing[768] = {};
                    sprintf_s(missing, sizeof(missing),
                        "[ControllerSetup] JOYSTICK_%d override missing: %s\n",
                        i + 1, pad.config.c_str());
                    Log(missing);
                    continue;
                }
                filename = pad.config.c_str();
                source = "JOYSTICK";
                break;
            }
        }
        char msg[768] = {};
        sprintf_s(msg, sizeof(msg),
            "[ControllerSetup] Native config selection: index=%u source=%s original=%s selected=%s.\n",
            deviceIndex, source, nativeFilename ? nativeFilename : "(null)",
            filename ? filename : "(null)");
        Log(msg);
        auto* effectiveManager = manager;
        const auto result = reinterpret_cast<LoadConfigFn>(g_api.nativeLoadConfig)(
            effectiveManager, deviceIndex, filename);
        sprintf_s(msg, sizeof(msg),
            "[ControllerSetup] Native config selection: index=%u loader returned %u.\n",
            deviceIndex, static_cast<unsigned>(result));
        Log(msg);
        if (deviceIndex < 25)
            g_nativeConfigLoadSucceeded[deviceIndex] = result != 0;
        return result;
    }

    bool InstallNativeConfigSelectionHook() {
        auto* instruction = reinterpret_cast<unsigned char*>(g_api.nativeConfigCall);
        if (instruction[0] != 0xE8) {
            Log("[ControllerSetup] Config hook aborted: native config callsite is not CALL rel32.\n");
            return false;
        }
        int32_t oldRel = 0;
        memcpy(&oldRel, instruction + 1, sizeof(oldRel));
        const uintptr_t target = g_api.nativeConfigCall + 5 + static_cast<intptr_t>(oldRel);
        if (target != g_api.nativeLoadConfig) {
            char msg[192] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] Config hook aborted: unexpected call target %p.\n",
                reinterpret_cast<void*>(target));
            Log(msg);
            return false;
        }
        const intptr_t offset = reinterpret_cast<intptr_t>(&NativeConfigSelectionHook) -
                                static_cast<intptr_t>(g_api.nativeConfigCall + 5);
        if (offset < INT32_MIN || offset > INT32_MAX) {
            Log("[ControllerSetup] Config hook aborted: hook outside rel32 range.\n");
            return false;
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(instruction, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            Log("[ControllerSetup] Config hook aborted: VirtualProtect failed.\n");
            return false;
        }
        const int32_t newRel = static_cast<int32_t>(offset);
        memcpy(instruction + 1, &newRel, sizeof(newRel));
        FlushInstructionCache(GetCurrentProcess(), instruction, 5);
        DWORD ignored = 0;
        VirtualProtect(instruction, 5, oldProtect, &ignored);
        {
            char msg[192] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] Installed native config selection hook for %s at %p.\n",
                g_api.name ? g_api.name : "unknown game",
                reinterpret_cast<void*>(g_api.nativeConfigCall));
            Log(msg);
        }
        return true;
    }

    void SynchronizeConfiguredSelectionsOnce() {
        if (g_deferredSelectionSyncDone || !g_syncNativeSelection)
            return;

        g_deferredSelectionSyncDone = true;

        if (g_keyboardConfigDeviceIndex >= 0 &&
            !g_keyboardConfig.empty()) {
            SynchronizeVerifiedNativeSelection(
                "KEYBOARD",
                g_keyboardConfigDeviceIndex,
                g_keyboardConfig);
        }

        for (int i = 0; i < 4; ++i) {
            const auto& pad = g_joysticks[i];
            if (!pad.enabled ||
                pad.configDeviceIndex < 0 ||
                pad.config.empty())
                continue;

            char label[32] = {};
            sprintf_s(label, sizeof(label), "JOYSTICK_%d", i + 1);

            SynchronizeVerifiedNativeSelection(
                label,
                pad.configDeviceIndex,
                pad.config);
        }
    }

    HANDLE g_frontEndWatcher = nullptr;
    void* g_lastAppliedGameMode = nullptr;

    bool IsReadableAddress(const void* ptr, size_t size) {
        if (!ptr || size == 0)
            return false;

        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        const DWORD protect = mbi.Protect & 0xFF;
        if (protect == PAGE_NOACCESS || protect == PAGE_GUARD)
            return false;

        const auto start =
            reinterpret_cast<uintptr_t>(ptr);
        const auto end =
            start + size;
        const auto regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) +
            mbi.RegionSize;

        return end >= start && end <= regionEnd;
    }

    void ApplyConfiguredControllerState(
        unsigned char* gameMode,
        void* sideData)
    {
        if (!g_settings.enabled || !gameMode || !sideData)
            return;

        char msg[256] = {};
        sprintf_s(
            msg, sizeof(msg),
            "[ControllerSetup] %s FE ready: mode=%p sideData=%p.\n",
            g_api.name ? g_api.name : "Unknown game",
            static_cast<void*>(gameMode), sideData);
        Log(msg);

        SynchronizeConfiguredSelectionsOnce();

        if (g_keyboardSide >= 0) {
            sprintf_s(
                msg, sizeof(msg),
                "[ControllerSetup] %s: assigning keyboard slot %u to side %d.\n",
                g_api.name ? g_api.name : "Unknown game",
                kKeyboardNativeSlot, g_keyboardSide);
            Log(msg);

            reinterpret_cast<SetSideFn>(g_api.nativeSetSide)(
                kKeyboardNativeSlot, g_keyboardSide);
            ApplyPlayerLock("KEYBOARD", kKeyboardNativeSlot, g_keyboardPlayerLock, g_keyboardSide);

            Log("[ControllerSetup] Keyboard side assignment returned.\n");
        }

        bool assigned[8] = {};
        assigned[kKeyboardNativeSlot] = true;

        for (int i = 0; i < 4; ++i) {
            const auto& pad = g_joysticks[i];

            if (!pad.enabled ||
                pad.side < 0 ||
                pad.nativeSlot < 0)
                continue;

            if (pad.nativeSlot >= 7 ||
                assigned[pad.nativeSlot]) {
                sprintf_s(
                    msg, sizeof(msg),
                    "[ControllerSetup] JOYSTICK_%d skipped: invalid/duplicate native slot %d.\n",
                    i + 1, pad.nativeSlot);
                Log(msg);
                continue;
            }

            assigned[pad.nativeSlot] = true;

            sprintf_s(
                msg, sizeof(msg),
                "[ControllerSetup] JOYSTICK_%d: assigning verified native slot %d to side %d.\n",
                i + 1, pad.nativeSlot, pad.side);
            Log(msg);

            reinterpret_cast<SetSideFn>(g_api.nativeSetSide)(
                static_cast<unsigned int>(pad.nativeSlot),
                pad.side);
            char joystickLabel[32] = {};
            sprintf_s(joystickLabel, sizeof(joystickLabel), "JOYSTICK_%d", i + 1);
            ApplyPlayerLock(joystickLabel, static_cast<unsigned int>(pad.nativeSlot), pad.playerLock, pad.side);

            sprintf_s(
                msg, sizeof(msg),
                "[ControllerSetup] JOYSTICK_%d native side assignment returned.\n",
                i + 1);
            Log(msg);
        }
    }

    DWORD WINAPI FrontEndReadyWatcher(LPVOID) {
        // Live 07/08 use the same robust startup strategy: watch the proven
        // FEUberGameMode singleton rather than guessing a particular FE callsite.
        //
        // Apply once per FEUberGameMode instance when both the singleton and
        // its +0x58 controller-side data are readable. If the frontend creates
        // a new FEUberGameMode later, apply once to that new instance too.
        for (;;) {
            if (!g_settings.enabled)
                break;

            auto** singleton =
                reinterpret_cast<unsigned char**>(g_api.gameModeGlobal);

            if (IsReadableAddress(singleton, sizeof(*singleton))) {
                auto* gameMode = *singleton;

                if (gameMode &&
                    gameMode != g_lastAppliedGameMode &&
                    IsReadableAddress(gameMode + 0x58, sizeof(void*))) {

                    auto* sideData =
                        *reinterpret_cast<void**>(gameMode + 0x58);

                    if (sideData &&
                        IsReadableAddress(sideData, 16)) {
                        // Give the native FE a short window to finish its own
                        // initial slot setup before imposing main.ini.
                        Sleep(100);

                        // Revalidate after the delay.
                        if (*singleton == gameMode &&
                            IsReadableAddress(gameMode + 0x58, sizeof(void*)) &&
                            *reinterpret_cast<void**>(gameMode + 0x58) == sideData) {

                            g_lastAppliedGameMode = gameMode;
                            ApplyConfiguredControllerState(
                                gameMode, sideData);
                        }
                    }
                }
            }

            Sleep(50);
        }

        return 0;
    }

    bool InstallFrontEndWatcher() {
        if (!g_api.watchFrontEndReady)
            return false;

        if (g_frontEndWatcher)
            return true;

        g_frontEndWatcher = CreateThread(
            nullptr,
            0,
            FrontEndReadyWatcher,
            nullptr,
            0,
            nullptr);

        if (!g_frontEndWatcher) {
            Log("[ControllerSetup] FE readiness watcher failed to start.\n");
            return false;
        }

        char msg[160] = {};
        sprintf_s(
            msg, sizeof(msg),
            "[ControllerSetup] Started %s FE readiness watcher.\n",
            g_api.name ? g_api.name : "unknown-game");
        Log(msg);
        return true;
    }

    // Called through the original game-specific FE initializer callsite.
    // __fastcall preserves the incoming ECX as the first argument.
    void __fastcall FrontEndInitHook(void* self, void*) {
        reinterpret_cast<FrontEndInitFn>(g_api.frontEndInit)(self);

        if (!g_settings.enabled)
            return;
        bool anyAssignment = g_keyboardSide >= 0 ||
            (!g_keyboardConfig.empty() && g_keyboardConfigDeviceIndex >= 0);
        for (const auto& pad : g_joysticks)
            anyAssignment |= pad.enabled && ((pad.nativeSlot >= 0 && pad.side >= 0) ||
                (pad.configDeviceIndex >= 0 && !pad.config.empty()));
        if (!anyAssignment)
            return;

        // FEUberGameMode is created during the original initializer.
        auto* gameMode = *reinterpret_cast<unsigned char* const*>(g_api.gameModeGlobal);
        if (!gameMode) {
            Log("[ControllerSetup] FE initialization completed, but singleton is null; skipping.\n");
            return;
        }
        auto* sideData = *reinterpret_cast<void* const*>(gameMode + 0x58);
        if (!sideData) {
            Log("[ControllerSetup] FE initialization completed, but singleton+0x58 is null; skipping.\n");
            return;
        }
        // Apply config files before side assignment. These are separate native APIs.
        if (g_keyboardConfigDeviceIndex >= 0 && g_keyboardConfigDeviceIndex < 25 &&
            !g_nativeConfigSeen[g_keyboardConfigDeviceIndex])
            ApplyNativeConfig("KEYBOARD", g_keyboardConfigDeviceIndex, g_keyboardConfig);
        for (int i = 0; i < 4; ++i) {
            const auto& pad = g_joysticks[i];
            if (!pad.enabled || pad.configDeviceIndex < 0 || pad.config.empty())
                continue;
            char name[32] = {};
            sprintf_s(name, "JOYSTICK_%d", i + 1);
            if (pad.configDeviceIndex >= 0 && pad.configDeviceIndex < 25 &&
                !g_nativeConfigSeen[pad.configDeviceIndex])
                ApplyNativeConfig(name, pad.configDeviceIndex, pad.config);
        }
        // Loading the profile record does not apply its live bindings. The
        // frontend separately issues: exec "<full path>"; submit; drain.
        // Apply each configured profile once, after input-manager setup.
        bool executed[25] = {};
        if (g_keyboardConfigDeviceIndex >= 0 && g_keyboardConfigDeviceIndex < 25 &&
            !g_keyboardConfig.empty()) {
            executed[g_keyboardConfigDeviceIndex] = true;
            if (g_api.selectConfigExecutesProfile) {
                SynchronizeVerifiedNativeSelection(
                    "KEYBOARD", g_keyboardConfigDeviceIndex, g_keyboardConfig);
            } else {
                const bool applied = ExecuteSelectedProfile(
                    "KEYBOARD", g_keyboardConfigDeviceIndex, g_keyboardConfig);
                if (applied)
                    SynchronizeVerifiedNativeSelection(
                        "KEYBOARD", g_keyboardConfigDeviceIndex, g_keyboardConfig);
            }
        }
        for (int i = 0; i < 4; ++i) {
            const auto& pad = g_joysticks[i];
            if (!pad.enabled || pad.config.empty() || pad.configDeviceIndex < 0 ||
                pad.configDeviceIndex >= 25)
                continue;
            if (executed[pad.configDeviceIndex]) {
                Log("[ControllerSetup] Duplicate CONFIG_DEVICE_INDEX: later profile skipped.\n");
                continue;
            }
            executed[pad.configDeviceIndex] = true;
            char label[32] = {};
            sprintf_s(label, "JOYSTICK_%d", i + 1);
            if (g_api.selectConfigExecutesProfile) {
                SynchronizeVerifiedNativeSelection(
                    label, pad.configDeviceIndex, pad.config);
            } else {
                const bool applied = ExecuteSelectedProfile(
                    label, pad.configDeviceIndex, pad.config);
                if (applied)
                    SynchronizeVerifiedNativeSelection(
                        label, pad.configDeviceIndex, pad.config);
            }
        }
        char msg[256] = {};
        if (g_keyboardSide >= 0) {
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] FE ready: mode=%p sideData=%p; assigning keyboard slot %u to side %d.\n",
                static_cast<void*>(gameMode), sideData, kKeyboardNativeSlot, g_keyboardSide);
            Log(msg);
            reinterpret_cast<SetSideFn>(g_api.nativeSetSide)(kKeyboardNativeSlot, g_keyboardSide);
            ApplyPlayerLock("KEYBOARD", kKeyboardNativeSlot, g_keyboardPlayerLock, g_keyboardSide);
            Log("[ControllerSetup] Native side assignment returned.\n");
        } else {
            Log("[ControllerSetup] No keyboard assignment requested.\n");
        }

        // The user-slot mapping for joysticks is *not* the same as the
        // DirectInput enumeration index. Only explicitly verified slots apply.
        bool assigned[8] = {};
        assigned[kKeyboardNativeSlot] = true;
        for (int i = 0; i < 4; ++i) {
            const auto& pad = g_joysticks[i];
            if (!pad.enabled || pad.side < 0 || pad.nativeSlot < 0)
                continue;
            if (pad.nativeSlot >= 7 || assigned[pad.nativeSlot]) {
                sprintf_s(msg, sizeof(msg),
                    "[ControllerSetup] JOYSTICK_%d skipped: invalid/duplicate native slot %d.\n",
                    i + 1, pad.nativeSlot);
                Log(msg);
                continue;
            }
            assigned[pad.nativeSlot] = true;
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] JOYSTICK_%d: assigning verified native slot %d to side %d.\n",
                i + 1, pad.nativeSlot, pad.side);
            Log(msg);
            reinterpret_cast<SetSideFn>(g_api.nativeSetSide)(
                static_cast<unsigned>(pad.nativeSlot), pad.side);
            char joystickLabel[32] = {};
            sprintf_s(joystickLabel, sizeof(joystickLabel), "JOYSTICK_%d", i + 1);
            ApplyPlayerLock(joystickLabel, static_cast<unsigned int>(pad.nativeSlot), pad.playerLock, pad.side);
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] JOYSTICK_%d native side assignment returned.\n", i + 1);
            Log(msg);
        }
    }

    bool InstallNativeCallHook() {
        if (g_api.watchFrontEndReady)
            return InstallFrontEndWatcher();

        auto* instruction = reinterpret_cast<unsigned char*>(g_api.frontEndInitCall);
        if (instruction[0] != 0xE8) {
            Log("[ControllerSetup] Hook aborted: FE init callsite is not CALL rel32.\n");
            return false;
        }
        int32_t originalRel = 0;
        memcpy(&originalRel, instruction + 1, sizeof(originalRel));
        const uintptr_t target = g_api.frontEndInitCall + 5 + static_cast<intptr_t>(originalRel);
        if (target != g_api.frontEndInit) {
            char msg[192] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] Hook aborted: unexpected existing call target %p.\n",
                reinterpret_cast<void*>(target));
            Log(msg);
            return false;
        }
        const intptr_t relative = reinterpret_cast<intptr_t>(&FrontEndInitHook) -
                                  static_cast<intptr_t>(g_api.frontEndInitCall + 5);
        if (relative < INT32_MIN || relative > INT32_MAX) {
            Log("[ControllerSetup] Hook aborted: replacement address out of rel32 range.\n");
            return false;
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(instruction, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            Log("[ControllerSetup] Hook aborted: VirtualProtect failed.\n");
            return false;
        }
        const int32_t newRel = static_cast<int32_t>(relative);
        memcpy(instruction + 1, &newRel, sizeof(newRel));
        FlushInstructionCache(GetCurrentProcess(), instruction, 5);
        DWORD ignored = 0;
        VirtualProtect(instruction, 5, oldProtect, &ignored);
        {
            char msg[192] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] Installed native FE init call-site hook for %s at %p.\n",
                g_api.name ? g_api.name : "unknown game",
                reinterpret_cast<void*>(g_api.frontEndInitCall));
            Log(msg);
        }
        return true;
    }

    void Log(const char* message) {
        OutputDebugStringA(message);
        HANDLE file = CreateFileA(".\\ControllerSetup.log", FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, message, static_cast<DWORD>(strlen(message)), &written, nullptr);
            CloseHandle(file);
        }
    }

    std::string GetString(const char* section, const char* key, const char* fallback = "") {
        char data[512] = {};
        GetPrivateProfileStringA(section, key, fallback, data,
                                 static_cast<DWORD>(sizeof(data)), kIniFile);
        return data;
    }

    std::string Lower(std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    }

    // Input is a filename, not an arbitrary disk path. Prevent traversal and
    // keep loading delegated to the original game's configs/ directory.
    bool ValidConfigName(const std::string& name, LiveControllerSetup::DeviceKind kind) {
        if (name.empty() || name.size() > 100 || name == "." || name == "..")
            return false;
        for (unsigned char ch : name) {
            if (!std::isalnum(ch) && ch != '_' && ch != '-' && ch != '.')
                return false;
        }
        const std::string low = Lower(name);
        if (kind == LiveControllerSetup::DeviceKind::Keyboard)
            return low.size() >= 4 && low.substr(low.size() - 4) == ".kfg";
        if (kind == LiveControllerSetup::DeviceKind::Gamepad)
            return low.size() >= 4 && low.substr(low.size() - 4) == ".jfg";
        return low.size() >= 4 && (low.substr(low.size() - 4) == ".kfg" ||
                                    low.substr(low.size() - 4) == ".jfg");
    }

    // Documents is the user-config source when enabled. Otherwise use the
    // installation's interface/configs folder. This only RESOLVES filenames;
    // it does not redirect the native game's config loader yet.
    std::string ResolveConfigDirectory() {
        const bool useDocuments = GetPrivateProfileIntA(
            "CONTROLLERS", "USE_DOCUMENTS_CONFIGS", 1, kIniFile) != 0;
        if (useDocuments) {
            char documents[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL,
                    nullptr, SHGFP_TYPE_CURRENT, documents))) {
                std::string path = documents;
                path += "\\";
                path += g_api.documentsFolder ? g_api.documentsFolder : "NBA LIVE 06";
                path += "\\configs\\";
                return path;
            }
            Log("[ControllerSetup] Cannot find Documents directory.\n");
            return {};
        }
        return ".\\interface\\configs\\";
    }

    bool FileExists(const std::string& path) {
        const DWORD attr = GetFileAttributesA(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES &&
               (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    void ParseSlot(int i, LiveControllerSetup::Slot& slot) {
        using namespace LiveControllerSetup;
        char section[32] = {};
        sprintf_s(section, sizeof(section), "CONTROLLER_%d", i + 1);
        slot.enabled = GetPrivateProfileIntA(section, "ENABLED", 0, kIniFile) != 0;
        if (!slot.enabled)
            return;

        const auto device = Lower(GetString(section, "DEVICE", "AUTO"));
        if (device == "keyboard") slot.device = DeviceKind::Keyboard;
        else if (device == "gamepad") slot.device = DeviceKind::Gamepad;
        else if (device == "auto") slot.device = DeviceKind::Auto;
        else {
            Log("[ControllerSetup] Invalid DEVICE; using AUTO.\n");
            slot.device = DeviceKind::Auto;
        }

        const auto side = Lower(GetString(section, "SIDE", "UNCHANGED"));
        if (side == "home") slot.side = Side::Home;
        else if (side == "away") slot.side = Side::Away;
        else if (side == "neutral") slot.side = Side::Neutral;
        else if (side == "unchanged") slot.side = Side::Unchanged;
        else {
            Log("[ControllerSetup] Invalid SIDE; using UNCHANGED.\n");
            slot.side = Side::Unchanged;
        }

        slot.playerLock = ReadPlayerLockSetting(section);
        slot.gamepadIndex = GetPrivateProfileIntA(section, "GAMEPAD_INDEX", -1, kIniFile);
        if (slot.gamepadIndex < -1 || slot.gamepadIndex > 3) {
            Log("[ControllerSetup] Invalid GAMEPAD_INDEX; using -1.\n");
            slot.gamepadIndex = -1;
        }
        slot.configFilename = GetString(section, "CONFIG", "");
        if (!slot.configFilename.empty() && !ValidConfigName(slot.configFilename, slot.device)) {
            Log("[ControllerSetup] Invalid CONFIG filename/extension; ignoring.\n");
            slot.configFilename.clear();
        }

        if (!slot.configFilename.empty()) {
            const std::string path = ResolveConfigDirectory() + slot.configFilename;
            if (!FileExists(path)) {
                char missing[700] = {};
                sprintf_s(missing, sizeof(missing),
                    "[ControllerSetup] Config not found (slot %d): %s\n",
                    i + 1, path.c_str());
                Log(missing);
            } else {
                char found[700] = {};
                sprintf_s(found, sizeof(found),
                    "[ControllerSetup] Config exists (slot %d): %s\n",
                    i + 1, path.c_str());
                Log(found);
            }
        }

        char msg[256] = {};
        sprintf_s(msg, sizeof(msg),
            "[ControllerSetup] Slot %d parsed (device=%d side=%d pad=%d config=%s).\n",
            i + 1, static_cast<int>(slot.device), static_cast<int>(slot.side),
            slot.gamepadIndex, slot.configFilename.c_str());
        Log(msg);
    }
}

namespace LiveControllerSetup {
    const Settings& GetSettings() { return g_settings; }
}

void InitializeControllerSetup() {
    Log("[ControllerSetup] InitializeControllerSetup entered.\n");
    g_settings = {};
    g_keyboardSide = -1;
    g_keyboardPlayerLock = -1;
    g_keyboardConfig.clear();
    g_keyboardConfigDeviceIndex = -1;
    for (auto& pad : g_joysticks)
        pad = {};
    memset(g_nativeConfigSeen, 0, sizeof(g_nativeConfigSeen));
    memset(g_nativeConfigLoadSucceeded, 0, sizeof(g_nativeConfigLoadSucceeded));
    g_deferredSelectionSyncDone = false;
    if (!ResolveControllerGameApi()) {
        Log("[ControllerSetup] Unsupported executable/version; initialization skipped.\n");
        return;
    }
    {
        char msg[160] = {};
        sprintf_s(msg, sizeof(msg),
            "[ControllerSetup] Runtime profile: %s.\n",
            g_api.name ? g_api.name : "unknown");
        Log(msg);
    }

    g_settings.enabled = GetPrivateProfileIntA("CONTROLLERS", "ENABLED", 0, kIniFile) != 0;

    // End-user policy: when controller overrides are enabled, keep the native
    // FE/gameplay catalogue selection synchronized automatically. 07 proved
    // that loading a different .jfg alone does not change the displayed/native
    // selected profile name.
    g_syncNativeSelection = g_settings.enabled;

    if (!g_settings.enabled) {
        Log("[ControllerSetup] Disabled or main.ini not found (ENABLED != 1).\n");
        return;
    }
    Log("[ControllerSetup] main.ini enabled.\n");

    {
        std::string dir = ResolveConfigDirectory();
        std::string logline = "[ControllerSetup] Config directory: " + dir + "\n";
        Log(logline.c_str());
    }
    for (int i = 0; i < 8; ++i)
        ParseSlot(i, g_settings.slots[i]);

    // [KEYBOARD] is the preferred spelling; legacy CONTROLLER_8 is fallback.
    const bool keyboardEnabled = GetPrivateProfileIntA(
        "KEYBOARD", "ENABLED", 0, kIniFile) != 0;
    if (keyboardEnabled) {
        g_keyboardConfig = GetString("KEYBOARD", "CONFIG", "");
        if (!g_keyboardConfig.empty() &&
            !ValidConfigName(g_keyboardConfig, LiveControllerSetup::DeviceKind::Keyboard)) {
            Log("[ControllerSetup] KEYBOARD invalid .kfg name; ignored.\n");
            g_keyboardConfig.clear();
        }
        const std::string keyboardIndex = GetString("KEYBOARD", "CONFIG_DEVICE_INDEX", "");
        g_keyboardConfigDeviceIndex = (keyboardIndex.empty() || Lower(keyboardIndex) == "auto")
            ? 1 : GetPrivateProfileIntA("KEYBOARD", "CONFIG_DEVICE_INDEX", -1, kIniFile);
        if (g_keyboardConfigDeviceIndex != 1 && !g_keyboardConfig.empty()) {
            Log("[ControllerSetup] KEYBOARD: invalid native loader index (expected 1); config override disabled.\n");
            g_keyboardConfigDeviceIndex = -1;
        }
        g_keyboardPlayerLock = ReadPlayerLockSetting("KEYBOARD");
        g_settings.slots[7].playerLock = g_keyboardPlayerLock;
        const auto side = Lower(GetString("KEYBOARD", "SIDE", "UNCHANGED"));
        if (side == "home") g_keyboardSide = 0;
        else if (side == "away") g_keyboardSide = 1;
        else if (side == "neutral") g_keyboardSide = 2;
    } else if (g_settings.slots[7].enabled &&
               g_settings.slots[7].device == LiveControllerSetup::DeviceKind::Keyboard) {
        g_keyboardPlayerLock = g_settings.slots[7].playerLock;
        switch (g_settings.slots[7].side) {
        case LiveControllerSetup::Side::Home: g_keyboardSide = 0; break;
        case LiveControllerSetup::Side::Away: g_keyboardSide = 1; break;
        case LiveControllerSetup::Side::Neutral: g_keyboardSide = 2; break;
        default: break;
        }
    }
    {
        char msg[128] = {};
        sprintf_s(msg, sizeof(msg), "[ControllerSetup] Resolved keyboard side argument: %d.\n", g_keyboardSide);
        Log(msg);
    }
    // Native user-slot indexes are separate from DirectInput pad indexes.
    // Therefore NATIVE_SLOT is mandatory for this first joystick version.
    bool haveAssignments = g_keyboardSide >= 0;
    bool used[8] = {};
    used[7] = true; // Never permit joystick overwriting the keyboard record.
    for (int i = 0; i < 4; ++i) {
        char section[32] = {};
        sprintf_s(section, sizeof(section), "JOYSTICK_%d", i + 1);
        auto& pad = g_joysticks[i];
        pad.enabled = GetPrivateProfileIntA(section, "ENABLED", 0, kIniFile) != 0;
        if (!pad.enabled)
            continue;
        pad.nativeSlot = GetPrivateProfileIntA(section, "NATIVE_SLOT", -1, kIniFile);
        pad.playerLock = ReadPlayerLockSetting(section);
        if (pad.nativeSlot >= 0 && pad.nativeSlot < 7)
            g_settings.slots[pad.nativeSlot].playerLock = pad.playerLock;
        pad.config = GetString(section, "CONFIG", "");
        if (!pad.config.empty() &&
            !ValidConfigName(pad.config, LiveControllerSetup::DeviceKind::Gamepad)) {
            Log("[ControllerSetup] Invalid joystick .jfg filename; ignored.\n");
            pad.config.clear();
        }
        // The loader target and the catalogue profile ID are different domains.
        // For FE joystick slots 0..3, the original game's verified mapping is
        // config loader index = native side slot + 3. Do not extrapolate for 4..6.
        const int expectedLoaderIndex =
            (pad.nativeSlot >= 0 && pad.nativeSlot <= 3) ? pad.nativeSlot + 3 : -1;
        const std::string loaderKey = GetString(section, "CONFIG_DEVICE_INDEX", "");
        const std::string typoKey = GetString(section, "CONFIG_DEVICEINDEX", "");
        if (!typoKey.empty()) {
            char msg[240] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] %s: CONFIG_DEVICEINDEX is not valid; use CONFIG_DEVICE_INDEX or omit it for AUTO.\n",
                section);
            Log(msg);
        }
        if (loaderKey.empty()) {
            pad.configDeviceIndex = expectedLoaderIndex;
            if (expectedLoaderIndex >= 0) {
                char msg[240] = {};
                sprintf_s(msg, sizeof(msg),
                    "[ControllerSetup] %s: automatic loader index %d derived from verified native slot %d.\n",
                    section, expectedLoaderIndex, pad.nativeSlot);
                Log(msg);
            }
        } else if (Lower(loaderKey) == "auto") {
            pad.configDeviceIndex = expectedLoaderIndex;
        } else {
            pad.configDeviceIndex = GetPrivateProfileIntA(
                section, "CONFIG_DEVICE_INDEX", -1, kIniFile);
            // Prevent confusing a profile catalogue ID (e.g. Wingman=7)
            // with the physical controller's loader index (joystick 0=3).
            if (expectedLoaderIndex >= 0 && pad.configDeviceIndex != expectedLoaderIndex) {
                char msg[280] = {};
                sprintf_s(msg, sizeof(msg),
                    "[ControllerSetup] %s: ERROR CONFIG_DEVICE_INDEX=%d conflicts with verified native slot %d (loader index %d). Config override disabled.\n",
                    section, pad.configDeviceIndex, pad.nativeSlot, expectedLoaderIndex);
                Log(msg);
                pad.configDeviceIndex = -1;
            }
        }
        if (pad.configDeviceIndex > 24) {
            Log("[ControllerSetup] Invalid native loader index (>24); config override disabled.\n");
            pad.configDeviceIndex = -1;
        }
        const auto side = Lower(GetString(section, "SIDE", "UNCHANGED"));
        if (side == "home") pad.side = 0;
        else if (side == "away") pad.side = 1;
        else if (side == "neutral") pad.side = 2;
        else pad.side = -1;
        if (pad.nativeSlot < 0 || pad.nativeSlot >= 7 || used[pad.nativeSlot]) {
            // A missing native *side* slot does not prevent configuration loading.
            if (pad.configDeviceIndex >= 0 && !pad.config.empty()) {
                char msg[180] = {};
                sprintf_s(msg, sizeof(msg),
                    "[ControllerSetup] %s side skipped: NATIVE_SLOT=%d; config still eligible.\n",
                    section, pad.nativeSlot);
                Log(msg);
                haveAssignments = true;
                continue;
            }
            char msg[180] = {};
            sprintf_s(msg, sizeof(msg),
                "[ControllerSetup] %s skipped: NATIVE_SLOT=%d (must be unique 0..6; slot 7 is keyboard).\n",
                section, pad.nativeSlot);
            Log(msg);
            pad.enabled = false;
            continue;
        }
        used[pad.nativeSlot] = true;
        if (pad.configDeviceIndex >= 0 && !pad.config.empty())
            haveAssignments = true;
        if (pad.side < 0) {
            char msg[120] = {};
            sprintf_s(msg, sizeof(msg), "[ControllerSetup] %s ignored: SIDE=UNCHANGED/invalid.\n", section);
            Log(msg);
            continue;
        }
        char msg[180] = {};
        sprintf_s(msg, sizeof(msg),
            "[ControllerSetup] %s enabled: native slot=%d side=%d.\n",
            section, pad.nativeSlot, pad.side);
        Log(msg);
        haveAssignments = true;
    }
    if (!g_keyboardConfig.empty() && g_keyboardConfigDeviceIndex >= 0)
        haveAssignments = true;
    if (!haveAssignments) {
        Log("[ControllerSetup] No valid controller overrides.\n");
        return;
    }
    InstallNativeConfigSelectionHook();
    InstallNativeCallHook();
}
