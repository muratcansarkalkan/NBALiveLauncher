// NBA Live 06: native frontend input enablement and optional APT-boundary diagnostics.
// Works on the supported 1.0 NOCD build only. All changes are in process memory.
// No MinHook dependency; no invented screen-specific button behavior.
// Configurable gate parity, native FE producer trace, input-step correlation.
// No direct native side setter calls or unverified APT VM dispatch hooks.
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <array>
#include <algorithm>
#include "plugin-std.h"
#include "Games.h"

namespace DeviceNavigation {
namespace {
struct PatchSite { uintptr_t address; unsigned char original[6]; size_t length; };
constexpr PatchSite kInputGuards[] = {
    {0x00533807, {0x0F,0x85,0x90,0x04,0x00,0x00}, 6},
    {0x00533BF0, {0x0F,0x85,0xA7,0x00,0x00,0x00}, 6}
};
constexpr uintptr_t kBindingAddress = 0x00B9BE20;
constexpr char kOriginalBindings[] =
    "bind pov0_e FE_RIGHT\n"
    "bind pov0_w FE_LEFT\n"
    "bind pov0_s FE_DOWN\n"
    "bind pov0_n FE_UP\n\n"
    "bind +axis0 FE_LA_RIGHT\n"
    "bind -axis0 FE_LA_LEFT\n"
    "bind +axis1 FE_LA_DOWN\n"
    "bind -axis1 FE_LA_UP\n\n"
    "bind button0 SELECT\n"
    "bind button1 PREVSCREEN\n"
    "bind button2 SWITCHLAYER\n"
    "bind button3 MENU\n"
    "bind button4 HELP\n"
    "bind button5 SPECIAL2\n"
    "bind button6 PREVGROUP\n"
    "bind button7 NEXTGROUP\n"
    "bind button8 NEXTSCREEN\n"
    "bind button9 SPECIAL1\n\n";
constexpr char kMappedBindings[] =
    "bind pov0_e FE_RIGHT\n"
    "bind pov0_w FE_LEFT\n"
    "bind pov0_s FE_DOWN\n"
    "bind pov0_n FE_UP\n\n"
    "bind +axis0 FE_LA_RIGHT\n"
    "bind -axis0 FE_LA_LEFT\n"
    "bind +axis1 FE_LA_DOWN\n"
    "bind -axis1 FE_LA_UP\n\n"
    "bind button0 HELP\n"
    "bind button1 SELECT\n"
    "bind button2 PREVSCREEN\n"
    "bind button3 MENU\n"
    "bind button4 PREVGROUP\n"
    "bind button5 NEXTGROUP\n"
    "bind button6 SWITCHLAYER\n"
    "bind button7 SWITCHLAYER\n"
    "bind button8 MENU\n"
    "bind button9 NEXTSCREEN\n\n";
constexpr size_t kBindingLength = sizeof(kOriginalBindings) - 1;
static_assert(sizeof(kMappedBindings) <= sizeof(kOriginalBindings), "Binding table replacement too long");

constexpr uintptr_t kAptCallsites[] = {0x005989D1, 0x00598A08};
constexpr uintptr_t kAptUpdate = 0x00582CD0;
// Both instructions are CALL rel32 to 0x00582CD0.
constexpr unsigned char kAptOriginal[2][5] = {
    {0xE8,0xFA,0xA2,0xFE,0xFF},
    {0xE8,0xC3,0xA2,0xFE,0xFF}
};

bool gEnabled = false;
bool gGuardsChanged = false;
bool gTraceGateState = false;
int gLastObservedGate = -1;
std::string gPatchedBindings;

bool gBindingsChanged = false;
bool gTraceInstalled = false;
bool gTraceEnabled = false;
bool gSideProbeInstalled = false;
bool gSideProbeEnabled = false;
bool gDirectionalTraceEnabled = false;
bool gProducerTraceEnabled = false;
bool gAptStepTraceEnabled = false;
uint32_t gAptStepSequence = 0;
uint32_t gProducedEvents = 0;
uint32_t gSideSetCalls = 0;
uint32_t gStepSideAtEntry = 0;
uint32_t gStepActionCount = 0;
uint32_t gStepState = 0;
uint32_t gStepRelevantEvents = 0;
uint32_t gStepTraceLines = 0;
uint32_t gDirectionalSequence = 0;
uint32_t gDirectionalEvents = 0;
uint32_t gSideGetCalls[2] = {};
uint32_t gLastFingerprint = 0;
uint32_t gLastCount = 0;

std::string ConfigurationPath();
void Log(const char* line) noexcept {
    OutputDebugStringA(line);
    std::string path = ConfigurationPath();
    const size_t separator = path.find_last_of("\\/");
    path.resize(separator == std::string::npos ? 0 : separator + 1);
    path += "DeviceNavigation.log";
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "ab") == 0 && file) {
        std::fputs(line, file);
        std::fclose(file);
    }
}

bool WritableCopy(uintptr_t address, const void* bytes, size_t size) noexcept {
    DWORD old = 0;
    void* at = reinterpret_cast<void*>(address);
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(at, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    DWORD ignored = 0;
    return VirtualProtect(at, size, old, &ignored) != 0;
}

bool Readable(const void* p, size_t bytes) noexcept {
    if (!p || !bytes) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(p, &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return begin <= end && bytes <= end - begin;
}

bool SupportedBuild() noexcept {
    if (FM::GetEntryPoint() != 0x40109F ||
        plugin::patch::GetFloat(0xBD832C) != 1.3333334f ||
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x00400000) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(0x00400000);
    if (!Readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(0x00400000 + dos->e_lfanew);
    return Readable(nt, sizeof(*nt)) && nt->Signature == IMAGE_NT_SIGNATURE &&
        nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386;
}

std::string ConfigurationPath() {
    char name[MAX_PATH] = {};
    DWORD length = GetModuleFileNameA(nullptr, name, MAX_PATH);
    if (!length || length >= MAX_PATH) return "DeviceNavigation.ini";
    std::string path(name, length);
    size_t end = path.find_last_of("\\/");
    path.resize(end == std::string::npos ? 0 : end + 1);
    return path + "DeviceNavigation.ini";
}

bool ConfigFlag(const std::string& file, const char* name, int defaultValue) {
    return GetPrivateProfileIntA("DEVICE_NAVIGATION", name, defaultValue, file.c_str()) != 0;
}

// Keys in [FE_BINDINGS] correspond to the unmodified PC frontend binding table.
// Overrides are constrained to existing action tokens; the native engine retains
// control of interpretation, controller IDs and screen-specific dispatch.
struct BindingKey { const char* nativeKey; const char* iniKey; };
constexpr BindingKey kBindingKeys[] = {
    {"pov0_e", "POV_E"}, {"pov0_w", "POV_W"},
    {"pov0_s", "POV_S"}, {"pov0_n", "POV_N"},
    {"+axis0", "AXIS0_POS"}, {"-axis0", "AXIS0_NEG"},
    {"+axis1", "AXIS1_POS"}, {"-axis1", "AXIS1_NEG"},
    {"button0", "BUTTON0"}, {"button1", "BUTTON1"},
    {"button2", "BUTTON2"}, {"button3", "BUTTON3"},
    {"button4", "BUTTON4"}, {"button5", "BUTTON5"},
    {"button6", "BUTTON6"}, {"button7", "BUTTON7"},
    {"button8", "BUTTON8"}, {"button9", "BUTTON9"}
};

// These tokens already occur in the original or tested remapped bindings.
// Adding new action names requires additional native confirmation.
bool AllowedAction(const std::string& s) noexcept {
    constexpr const char* kAllowed[] = {
        "FE_RIGHT", "FE_LEFT", "FE_DOWN", "FE_UP",
        "FE_LA_RIGHT", "FE_LA_LEFT", "FE_LA_DOWN", "FE_LA_UP",
        "SELECT", "PREVSCREEN", "SWITCHLAYER", "MENU", "HELP",
        "SPECIAL2", "PREVGROUP", "NEXTGROUP", "NEXTSCREEN", "SPECIAL1"
    };
    for (const char* candidate : kAllowed) if (s == candidate) return true;
    return false;
}

std::string ReadIniString(const std::string& ini, const char* section,
                          const char* key, const char* fallback = "") {
    char result[96] = {};
    GetPrivateProfileStringA(section, key, fallback, result,
                             static_cast<DWORD>(sizeof(result)), ini.c_str());
    return result;
}

// Build an exact replacement for the game's fixed-size embedded binding string.
// Fails closed if a custom mapping is invalid or doesn't fit the original slot.
bool ApplyBindingConfiguration(const std::string& ini, bool standardRemap) {
    if (std::memcmp(reinterpret_cast<const void*>(kBindingAddress),
                    kOriginalBindings, kBindingLength) != 0) {
        Log("[DeviceNavigation] FE binding source bytes differ; skipped.\n");
        return false;
    }
    const char* base = standardRemap ? kMappedBindings : kOriginalBindings;
    std::string text(base);
    bool customized = false;

    for (const auto& binding : kBindingKeys) {
        const std::string custom = ReadIniString(ini, "FE_BINDINGS", binding.iniKey);
        if (custom.empty()) continue;
        if (!AllowedAction(custom)) {
            char message[180] = {};
            std::snprintf(message, sizeof(message),
                "[DeviceNavigation] Unsupported FE_BINDINGS %s=%s; all remapping skipped.\n",
                binding.iniKey, custom.c_str());
            Log(message);
            return false;
        }
        const std::string needle = std::string("bind ") + binding.nativeKey + " ";
        size_t start = text.find(needle);
        if (start == std::string::npos ||
            (start > 0 && text[start - 1] != '\n')) {
            Log("[DeviceNavigation] Unexpected FE bindings text layout; skipped.\n");
            return false;
        }
        start += needle.length();
        const size_t end = text.find('\n', start);
        if (end == std::string::npos) return false;
        text.replace(start, end - start, custom);
        customized = true;
    }

    if (!customized && !standardRemap) {
        Log("[DeviceNavigation] Native FE bindings preserved.\n");
        return true;
    }
    if (text.size() > kBindingLength) {
        char line[180] = {};
        std::snprintf(line, sizeof(line),
            "[DeviceNavigation] FE binding configuration too long (%u>%u); skipped.\n",
            static_cast<unsigned>(text.size()), static_cast<unsigned>(kBindingLength));
        Log(line);
        return false;
    }
    text.resize(kBindingLength, ' ');
    if (!WritableCopy(kBindingAddress, text.data(), text.size())) return false;
    gPatchedBindings = text;
    gBindingsChanged = true;
    Log(customized
        ? "[DeviceNavigation] FE bindings loaded from [FE_BINDINGS] and installed.\n"
        : "[DeviceNavigation] Standard FE bindings remap installed.\n");
    return true;
}

void RestoreBindings() {
    if (!gBindingsChanged || gPatchedBindings.size() != kBindingLength) return;
    if (std::memcmp(reinterpret_cast<const void*>(kBindingAddress),
                    gPatchedBindings.data(), kBindingLength) == 0 &&
        WritableCopy(kBindingAddress, kOriginalBindings, kBindingLength)) {
        gBindingsChanged = false;
        gPatchedBindings.clear();
    }
}

// Optional read-only native callback probe for Play Now and Practice.
// Replaces only the PUSH-immediate registration argument in process memory,
// before FETranslation registers its callbacks. Does NOT replace side logic.
// Requires initialization before registration functions 0x5D4E90/0x5D5300.
struct SideCallbackPatch {
    uintptr_t pushAddress;
    uintptr_t originalCallback;
    void* replacement;
};

using SideNativeCallback = void (__cdecl*)(void*, void*);
using ControllerManagerAccessor = void* (__cdecl*)();
using ControllerLookup = void* (__thiscall*)(void*, int);

void DumpSideSnapshot(const char* mode, const char* phase) noexcept {
    if (!gSideProbeEnabled) return;
    auto managerFn = reinterpret_cast<ControllerManagerAccessor>(0x004A6130);
    auto lookupFn = reinterpret_cast<ControllerLookup>(0x00486250);
    void* manager = managerFn();
    if (!Readable(manager, 0x174)) return;
    char line[512] = {};
    int pos = std::snprintf(line, sizeof(line),
        "[DeviceNavigation] %s %s native slots:", mode, phase);
    for (int id = 0; id < 8 && pos > 0 && pos < static_cast<int>(sizeof(line)) - 45; ++id) {
        const int slot = id == 7 ? 1 : id + 3;
        const auto* obj = static_cast<const unsigned char*>(lookupFn(manager, slot));
        if (obj && Readable(obj, 0x20)) {
            const int association = *reinterpret_cast<const int*>(obj + 0x0C);
            const int state = *reinterpret_cast<const int*>(obj + 0x1C);
            pos += std::snprintf(line + pos, sizeof(line) - pos,
                " id%d/s%d=(%d,%d)", id, slot, association, state);
        } else {
            pos += std::snprintf(line + pos, sizeof(line) - pos,
                " id%d/s%d=missing", id, slot);
        }
    }
    std::snprintf(line + (pos < 0 ? 0 : (pos < static_cast<int>(sizeof(line)) ? pos : sizeof(line)-1)),
        pos < 0 ? sizeof(line) : (pos < static_cast<int>(sizeof(line)) ? sizeof(line)-pos : 1), "\n");
    Log(line);
}

// FETranslation argument array: the first and second script parameters are
// indices 0 and 1. This is a passive read; extractor methods mutate cursors.
bool ReadSideArguments(void* translator, int* controllerId, int* side) noexcept {
    if (!Readable(translator, 0x18)) return false;
    const uintptr_t ctx = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(translator) + 0x14);
    if (!Readable(reinterpret_cast<const void*>(ctx), 0x1B4)) return false;
    const int count = *reinterpret_cast<const int*>(ctx + 0x1AC);
    const uintptr_t array = *reinterpret_cast<const uint32_t*>(ctx + 0x1A4);
    if (count < 2 || count > 64 || !Readable(reinterpret_cast<const void*>(array), 0x330))
        return false;
    *controllerId = *reinterpret_cast<const int*>(array + 4);
    *side = *reinterpret_cast<const int*>(array + 0x198 + 4);
    return true;
}

void __cdecl PlayNowSetSideProbe(void* translator, void* result) {
    int id = -999, side = -999;
    const bool haveArgs = ReadSideArguments(translator, &id, &side);
    char line[320] = {};
    std::snprintf(line, sizeof(line),
        "[DeviceNavigation] SIDE_SET begin fe_seq=%u apt_step=%u id=%d requested=%d args=%s\n",
        gDirectionalSequence, gAptStepSequence, id, side, haveArgs ? "read" : "unavailable");
    Log(line);
    ++gSideSetCalls;
    DumpSideSnapshot("PlayNow", "before");
    reinterpret_cast<SideNativeCallback>(0x005BE9B0)(translator, result);
    DumpSideSnapshot("PlayNow", "after");
    std::snprintf(line, sizeof(line), "[DeviceNavigation] SIDE_SET end seq=%u\n", gDirectionalSequence);
    Log(line);
}
void __cdecl PracticeSetSideProbe(void* translator, void* result) {
    int id = -999, side = -999;
    const bool haveArgs = ReadSideArguments(translator, &id, &side);
    char line[200] = {};
    std::snprintf(line, sizeof(line),
        "[DeviceNavigation] PRACTICE_SIDE_SET apt_step=%u id=%d requested=%d args=%s\n",
        gAptStepSequence, id, side, haveArgs ? "read" : "unavailable");
    Log(line);
    ++gSideSetCalls;
    DumpSideSnapshot("Practice", "before");
    reinterpret_cast<SideNativeCallback>(0x005BE9E0)(translator, result);
    DumpSideSnapshot("Practice", "after");
}
void __cdecl PlayNowGetSideProbe(void* translator, void* result) {
    reinterpret_cast<SideNativeCallback>(0x005CAF60)(translator, result);
    if (++gSideGetCalls[0] <= 8 || gSideGetCalls[0] % 120 == 0) {
        Log("[DeviceNavigation] PlayNow GetControllerSide CALLED\n");
        DumpSideSnapshot("PlayNow", "get");
    }
}
void __cdecl PracticeGetSideProbe(void* translator, void* result) {
    reinterpret_cast<SideNativeCallback>(0x005CAFE0)(translator, result);
    if (++gSideGetCalls[1] <= 8 || gSideGetCalls[1] % 120 == 0) {
        Log("[DeviceNavigation] Practice GetControllerSide CALLED\n");
        DumpSideSnapshot("Practice", "get");
    }
}

const SideCallbackPatch kSideCallbacks[] = {
    {0x005D50ED, 0x005BE9B0, reinterpret_cast<void*>(&PlayNowSetSideProbe)},
    {0x005D5113, 0x005CAF60, reinterpret_cast<void*>(&PlayNowGetSideProbe)},
    {0x005D53B9, 0x005BE9E0, reinterpret_cast<void*>(&PracticeSetSideProbe)},
    {0x005D53DF, 0x005CAFE0, reinterpret_cast<void*>(&PracticeGetSideProbe)},
};

bool PatchCallback(const SideCallbackPatch& site, uintptr_t target) noexcept {
    if (*reinterpret_cast<const unsigned char*>(site.pushAddress) != 0x68) return false;
    const uintptr_t immediateAddress = site.pushAddress + 1;
    uint32_t current = 0;
    std::memcpy(&current, reinterpret_cast<const void*>(immediateAddress), 4);
    if (current != static_cast<uint32_t>(site.originalCallback) &&
        current != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(site.replacement))) return false;
    uint32_t value = static_cast<uint32_t>(target);
    return WritableCopy(immediateAddress, &value, 4);
}

bool InstallSideProbe() noexcept {
    if (gSideProbeInstalled) return true;
    // Validate all instruction encodings/expected callbacks before modifying any.
    for (const auto& site : kSideCallbacks) {
        if (!Readable(reinterpret_cast<void*>(site.pushAddress), 5) ||
            *reinterpret_cast<const unsigned char*>(site.pushAddress) != 0x68) return false;
        uint32_t current = 0;
        std::memcpy(&current, reinterpret_cast<const void*>(site.pushAddress + 1), 4);
        if (current != site.originalCallback) return false;
    }
    size_t changed = 0;
    for (const auto& site : kSideCallbacks) {
        if (!PatchCallback(site, reinterpret_cast<uintptr_t>(site.replacement))) {
            for (size_t i = 0; i < changed; ++i)
                PatchCallback(kSideCallbacks[i], kSideCallbacks[i].originalCallback);
            return false;
        }
        ++changed;
    }
    gSideProbeInstalled = true;
    gSideProbeEnabled = true;
    Log("[DeviceNavigation] PlayNow/Practice callback probe installed; requires pre-registration initialization.\n");
    return true;
}

void RemoveSideProbe() noexcept {
    gSideProbeEnabled = false;
    if (!gSideProbeInstalled) return;
    bool allRestored = true;
    for (const auto& site : kSideCallbacks) {
        if (!PatchCallback(site, site.originalCallback)) allRestored = false;
    }
    if (allRestored) gSideProbeInstalled = false;
    // Note: FETranslation may retain pointers to this module after registration.
    // Do not unload the launcher module while the game remains running.
}

// Native FE action producer sites, inspected in original x86 executable.
// All are E8 rel32 calls from FEInputInterface::Update to the native
// __thiscall AppendAction at 0x00525690 (which returns with retn 1Ch).
// The probe only logs the by-value 28-byte record and tail-jumps to the original.
constexpr uintptr_t kProducerTarget = 0x00525690;
constexpr uintptr_t kProducerSites[] = {
    0x0053362F, 0x0053364B, 0x005336A6, 0x005336FA,
    0x0053374E, 0x005337A2, 0x005337F6, 0x0053387E
};
struct NativeProducerPatch {
    uintptr_t site;
    unsigned char original[5];
};
NativeProducerPatch gProducerPatches[sizeof(kProducerSites) / sizeof(kProducerSites[0])] = {};
bool gProducerInstalled = false;

bool RelativeCall(uintptr_t source, uintptr_t target, unsigned char out[5]) noexcept {
    const intptr_t delta = static_cast<intptr_t>(target) - static_cast<intptr_t>(source + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    out[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(delta);
    std::memcpy(out + 1, &rel, sizeof(rel));
    return true;
}

void __cdecl TraceProducedNative(uintptr_t returnAddress, const uint32_t* words, void* nativeQueue) noexcept {
    if (!gProducerTraceEnabled || gProducedEvents >= 384 || !Readable(words, 28)) return;
    if (words[0] != 1) return;  // Exclude continuous mouse-motion records.
    ++gProducedEvents;
    char line[300] = {};
    std::snprintf(line, sizeof(line),
        "[DeviceNavigation] FE_PRODUCE n=%u caller=%08X queue=%p "
        "raw=%08X/%08X/%08X/%08X/%08X/%08X/%08X apt_step=%u\n",
        gProducedEvents, static_cast<unsigned>(returnAddress - 5), nativeQueue,
        words[0], words[1], words[2], words[3], words[4], words[5], words[6],
        gAptStepSequence);
    Log(line);
}

// The bridge sees [esp]=return address, [esp+4..+31]=7 action DWORDs;
// ECX=FEInputInterface queue object. Preserve all registers/flags and let
// the original __thiscall (retn 1Ch) consume the original stack arguments.
__declspec(naked) void NativeProducerBridge() {
    __asm {
        pushfd
        pushad
        mov eax, [esp+24]    // original ECX from pushad frame
        mov edx, [esp+36]    // callsite return address
        lea ebx, [esp+40]    // by-value action record
        push eax
        push ebx
        push edx
        call TraceProducedNative
        add esp, 12
        popad
        popfd
        push 00525690h
        ret
    }
}

bool InstallProducerTrace() noexcept {
    if (gProducerInstalled) return true;
    unsigned char bridge[5] = {};
    for (size_t i = 0; i < sizeof(kProducerSites)/sizeof(kProducerSites[0]); ++i) {
        unsigned char expected[5] = {};
        if (!RelativeCall(kProducerSites[i], kProducerTarget, expected) ||
            !RelativeCall(kProducerSites[i], reinterpret_cast<uintptr_t>(&NativeProducerBridge), bridge) ||
            !Readable(reinterpret_cast<const void*>(kProducerSites[i]), 5) ||
            std::memcmp(reinterpret_cast<const void*>(kProducerSites[i]), expected, 5) != 0) {
            Log("[DeviceNavigation] FE producer hook skipped: original callsite mismatch.\n");
            return false;
        }
        gProducerPatches[i].site = kProducerSites[i];
        std::memcpy(gProducerPatches[i].original, expected, 5);
    }
    size_t done = 0;
    for (; done < sizeof(kProducerSites)/sizeof(kProducerSites[0]); ++done) {
        if (!RelativeCall(kProducerSites[done], reinterpret_cast<uintptr_t>(&NativeProducerBridge), bridge) ||
            !WritableCopy(kProducerSites[done], bridge, 5)) break;
    }
    if (done < sizeof(kProducerSites)/sizeof(kProducerSites[0])) {
        for (size_t i = 0; i < done; ++i)
            WritableCopy(gProducerPatches[i].site, gProducerPatches[i].original, 5);
        Log("[DeviceNavigation] FE producer hook installation failed; rolled back.\n");
        return false;
    }
    gProducerInstalled = true;
    gProducerTraceEnabled = true;
    Log("[DeviceNavigation] FE producer hook installed: 8 native append callsites (logging only).\n");
    return true;
}

void RemoveProducerTrace() noexcept {
    gProducerTraceEnabled = false;
    if (!gProducerInstalled) return;
    bool restored = true;
    for (const auto& patch : gProducerPatches) {
        unsigned char expected[5] = {};
        if (!RelativeCall(patch.site, reinterpret_cast<uintptr_t>(&NativeProducerBridge), expected) ||
            std::memcmp(reinterpret_cast<const void*>(patch.site), expected, 5) != 0 ||
            !WritableCopy(patch.site, patch.original, 5)) restored = false;
    }
    if (restored) gProducerInstalled = false;
    // Caller MUST ensure no game thread executes these bytes during removal.
}

// The frontend FESystem object is in ESI at both observed APT-update callsites.
// Read FEInputInterface's native 28-byte queue *before* APT consumes its events.
void __cdecl CaptureAfterAptUpdate(void* feSystem) noexcept {
    if (!gAptStepTraceEnabled || !Readable(feSystem, 0x44)) return;
    const uintptr_t feInput = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(feSystem) + 0x40);
    if (!Readable(reinterpret_cast<const void*>(feInput), 0x4C)) return;
    const uint32_t afterCount = *reinterpret_cast<const uint32_t*>(feInput + 0x48);
    const uint32_t sideCalls = gSideSetCalls - gStepSideAtEntry;
    if ((gStepRelevantEvents || sideCalls) && gStepTraceLines++ < 384) {
        char line[280] = {};
        std::snprintf(line, sizeof(line),
            "[DeviceNavigation] APT_STEP_DONE step=%u fe_state=%u input_before=%u "
            "input_after=%u category1_before=%u side_calls_during=%u\n",
            gAptStepSequence, gStepState, gStepActionCount,
            afterCount, gStepRelevantEvents, sideCalls);
        Log(line);
    }
}

void __cdecl CaptureActionQueue(void* feSystem) noexcept {
    if (!gTraceEnabled || !Readable(feSystem, 0x44)) return;
    const uintptr_t feInput = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(feSystem) + 0x40);
    if (!Readable(reinterpret_cast<void*>(feInput), 0x4C)) return;
    const uintptr_t records = *reinterpret_cast<const uint32_t*>(feInput + 0x40);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(feInput + 0x48);
    const uint32_t state = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(feSystem) + 0x0C);
    if (gAptStepTraceEnabled) {
        ++gAptStepSequence;
        gStepState = state;
        gStepActionCount = count;
        gStepSideAtEntry = gSideSetCalls;
        gStepRelevantEvents = 0;
        if (count <= 32 && (!count || Readable(reinterpret_cast<const void*>(records), count*28u))) {
            const uint32_t* rec = reinterpret_cast<const uint32_t*>(records);
            for (uint32_t i = 0; i < count; ++i)
                if (rec[7u*i] == 1) ++gStepRelevantEvents;
        }
    }
    // Sample the native gate before APT consumption, even when the queue is empty.
    if (gTraceGateState) {
        const int gate = *reinterpret_cast<const volatile uint8_t*>(0x00C4B57C);
        if (gate != gLastObservedGate) {
            char message[160] = {};
            std::snprintf(message, sizeof(message),
                "[DeviceNavigation] NATIVE_GATE state=%u disabled=%d count=%u\n", state, gate, count);
            Log(message);
            gLastObservedGate = gate;
        }
    }
    if (!count || count > 32 || !Readable(reinterpret_cast<void*>(records), count * 28u)) return;
    const auto* words = reinterpret_cast<const uint32_t*>(records);

    if (gDirectionalTraceEnabled) {
        // 28-byte FEInputInterface records: word0=category, word1=action,
        // word3=source/context code (its exact meaning is not yet verified).
        // Include all category-1 records, including the 0x0A path gated by
        // 0x00533807, not just previously observed source/context 3.
        // Exclude continuous cursor-coordinate category 2 records.
        if (state != 1 && state != 4) return;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t* item = words + 7u * i;
            if (item[0] != 1) continue;
            if (gDirectionalEvents >= 512) return;
            ++gDirectionalEvents;
            ++gDirectionalSequence;
            char line[320] = {};
            std::snprintf(line, sizeof(line),
                "[DeviceNavigation] FE_ACTION seq=%u state=%u code=0x%02X context=%u word4=0x%08X extra=%08X/%08X/%08X count=%u\n",
                gDirectionalSequence, state, item[1], item[3], item[4], item[2], item[5], item[6], count);
            Log(line);
        }
        return;
    }

    // Original unrestricted APT queue trace, retained for compatibility.
    uint32_t fingerprint = 2166136261u;
    for (uint32_t i = 0; i < count * 7; ++i)
        fingerprint = (fingerprint ^ words[i]) * 16777619u;
    if (count == gLastCount && fingerprint == gLastFingerprint) return;
    gLastCount = count;
    gLastFingerprint = fingerprint;
    char line[384] = {};
    std::snprintf(line, sizeof(line), "[DeviceNavigation] FESystem state=%u, action count=%u\n", state, count);
    Log(line);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t* item = words + 7u * i;
        std::snprintf(line, sizeof(line),
            "[DeviceNavigation] action %u: %08X %08X %08X %08X %08X %08X %08X\n",
            i, item[0], item[1], item[2], item[3], item[4], item[5], item[6]);
        Log(line);
    }
}

// Both callsites push three DWORD arguments (arg0=frame, arg1=1, arg2=1).
// We preserve the original ECX/stack, invoke the native __thiscall (retn 0Ch)
// and log a passive post-update checkpoint. No synthetic input is issued.
__declspec(naked) void AptUpdateTraceBridge() {
    __asm {
        pushfd
        pushad
        push esi
        call CaptureActionQueue
        add esp, 4
        popad
        popfd
        push dword ptr [esp+12]   // copy arg2
        push dword ptr [esp+12]   // copy arg1
        push dword ptr [esp+12]   // copy arg0
        mov eax, 00582CD0h
        call eax
        pushfd
        pushad
        push esi
        call CaptureAfterAptUpdate
        add esp, 4
        popad
        popfd
        ret 0Ch                  // consume caller's original arguments
    }
}

bool InstallAptTrace() noexcept {
    if (gTraceInstalled) return true;
    for (int i = 0; i < 2; ++i)
        if (std::memcmp(reinterpret_cast<void*>(kAptCallsites[i]), kAptOriginal[i], 5) != 0) return false;
    const uintptr_t bridge = reinterpret_cast<uintptr_t>(&AptUpdateTraceBridge);
    unsigned char patch[2][5] = {};
    for (int i = 0; i < 2; ++i) {
        intptr_t delta = static_cast<intptr_t>(bridge) - static_cast<intptr_t>(kAptCallsites[i] + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) return false;
        patch[i][0] = 0xE8;
        const int32_t displacement = static_cast<int32_t>(delta);
        std::memcpy(patch[i] + 1, &displacement, sizeof(displacement));
    }
    if (!WritableCopy(kAptCallsites[0], patch[0], 5)) return false;
    if (!WritableCopy(kAptCallsites[1], patch[1], 5)) {
        WritableCopy(kAptCallsites[0], kAptOriginal[0], 5);
        return false;
    }
    gTraceInstalled = true;
    gTraceEnabled = true;
    Log("[DeviceNavigation] APT boundary trace installed for two FE states.\n");
    return true;
}

void RemoveAptTrace() noexcept {
    gTraceEnabled = false;
    if (!gTraceInstalled) return;
    // Don't overwrite code that has been modified by another component.
    const uintptr_t bridge = reinterpret_cast<uintptr_t>(&AptUpdateTraceBridge);
    bool restored = true;
    for (int i = 0; i < 2; ++i) {
        unsigned char expected[5] = {0xE8};
        const int32_t displacement = static_cast<int32_t>(bridge - (kAptCallsites[i] + 5));
        std::memcpy(expected + 1, &displacement, 4);
        if (std::memcmp(reinterpret_cast<const void*>(kAptCallsites[i]), expected, 5) != 0 ||
            !WritableCopy(kAptCallsites[i], kAptOriginal[i], 5)) restored = false;
    }
    if (restored) gTraceInstalled = false;
}
} // namespace

bool Install() noexcept {
    if (gEnabled) return true;
    if (!SupportedBuild()) { Log("[DeviceNavigation] Unsupported build; skipped.\n"); return false; }
    const std::string ini = ConfigurationPath();
    Log("[DeviceNavigation] Initializing; configuration read beside game executable.\n");
    if (!ConfigFlag(ini, "ENABLE", 1)) { Log("[DeviceNavigation] Disabled by INI.\n"); return false; }

    // Native=original JNE instructions, Bypass=historical two NOP patches.
    // Both modes use the exact same source build for direct A/B comparisons.
    const std::string mode = ReadIniString(ini, "DEVICE_NAVIGATION", "GATE_MODE", "BYPASS");
    const bool bypass = mode == "BYPASS";
    if (!bypass && mode != "NATIVE") {
        Log("[DeviceNavigation] Unknown GATE_MODE; use NATIVE or BYPASS.\n");
        return false;
    }
    for (const auto& guard : kInputGuards)
        if (std::memcmp(reinterpret_cast<const void*>(guard.address), guard.original, guard.length) != 0) {
            Log("[DeviceNavigation] Unexpected input-guard bytes; skipped.\n");
            return false;
        }
    if (bypass) {
        const unsigned char nops[6] = {0x90,0x90,0x90,0x90,0x90,0x90};
        if (!WritableCopy(kInputGuards[0].address, nops, 6)) return false;
        if (!WritableCopy(kInputGuards[1].address, nops, 6)) {
            WritableCopy(kInputGuards[0].address, kInputGuards[0].original, 6);
            return false;
        }
        gGuardsChanged = true;
        Log("[DeviceNavigation] GATE_MODE=BYPASS; both native frontend input checks NOPed.\n");
    } else {
        Log("[DeviceNavigation] GATE_MODE=NATIVE; both frontend checks preserved.\n");
    }
    gEnabled = true;

    // Overrides are optional; no method is called on the native side setter.
    ApplyBindingConfiguration(ini, ConfigFlag(ini, "REMAP_BUTTONS", 0));

    if (ConfigFlag(ini, "TRACE_CONTROLLER_SIDE", 0)) {
        if (!InstallSideProbe()) Log("[DeviceNavigation] Side probe skipped: callback registration bytes differ.\n");
    }
    gDirectionalTraceEnabled = ConfigFlag(ini, "TRACE_DIRECTIONAL", 0);
    gAptStepTraceEnabled = ConfigFlag(ini, "TRACE_APT_STEPS", 0);
    if (ConfigFlag(ini, "TRACE_FE_PRODUCER", 0))
        if (!InstallProducerTrace()) Log("[DeviceNavigation] FE producer hook not active.\n");
    gTraceGateState = ConfigFlag(ini, "TRACE_GATE", 0);
    if (gDirectionalTraceEnabled)
        Log("[DeviceNavigation] Directional probe: category-1 records in FE states 1 and 4, all contexts.\n");
    if (gTraceGateState)
        Log("[DeviceNavigation] Native gate state sampling enabled at APT boundary.\n");
    if (gAptStepTraceEnabled)
        Log("[DeviceNavigation] APT step correlation enabled; does NOT assert script handler received action.\n");
    if (gDirectionalTraceEnabled || gAptStepTraceEnabled || gTraceGateState || ConfigFlag(ini, "TRACE_ACTIONS", 0)) {
        if (!InstallAptTrace()) Log("[DeviceNavigation] APT trace skipped: callsite mismatch.\n");
    } else {
        Log("[DeviceNavigation] Action trace disabled by INI.\n");
    }
    return true;
}

void Shutdown() noexcept {
    RemoveProducerTrace();
    RemoveSideProbe();
    RemoveAptTrace();
    gDirectionalTraceEnabled = false;
    gAptStepTraceEnabled = false;
    gTraceGateState = false;
    RestoreBindings();
    if (!gEnabled) return;
    if (gGuardsChanged) {
        const unsigned char nops[6] = {0x90,0x90,0x90,0x90,0x90,0x90};
        bool ok = true;
        for (const auto& guard : kInputGuards)
            if (std::memcmp(reinterpret_cast<const void*>(guard.address), nops, 6) != 0 ||
                !WritableCopy(guard.address, guard.original, guard.length)) ok = false;
        if (!ok) Log("[DeviceNavigation] Could not restore all guard bytes.\n");
        else gGuardsChanged = false;
    }
    gEnabled = false;
}
} // namespace DeviceNavigation

void InitializeDeviceNavigation() { DeviceNavigation::Install(); }
