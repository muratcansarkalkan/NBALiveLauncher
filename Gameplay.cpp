// Gameplay.cpp — NBA Live turnover correction (experimental)
//
// Experimental NBA Live PC turnover-attribution correction.
//
// IMPLEMENTED: NBA Live 06 PC, executable SHA-256 observed during research:
//   4ea37d103ee80cd43d26f5680d6e345dd7be92bfa1d3fa6d31cfa8857161e9dc
//
// NOT YET PORTED: NBA Live 2005, 07, 08. Their hook sites, event layouts,
// dispatcher return addresses and instruction sequences MUST be verified
// independently before adding them to kProfiles below.
//
// Hypothesis (confirmed for the cases tested in x32dbg, not exhaustive):
//   For the 0x19170800 out-of-bounds statistics listener ONLY,
//   when the current event is in state 0x0082 and its +04/+06 words
//   contain opposing sides (0 vs 5), the player turnover increment is
//   spurious because a defender knocked the ball out of bounds while the
//   offense retained possession.
//
// We hook precisely the 7-byte sequence at 004AC384:
//   66 FF 04 57     inc word ptr [edi+edx*2]
//   83 F9 FF        cmp ecx, -1
// and resume at 004AC38B (the untouched 'lea edx,[edi+edx*2]').
// The hook preserves registers, flags, normal gameplay event processing,
// and the original increment for all calls which fail the strict filter.
//
// Integration (existing NBA Live ASI/DLL, Visual Studio Win32/x86):
//   1. Add Gameplay.cpp to your Win32 DLL project (MSVC x86, NOT x64).
//   2. In your normal initialization, BEFORE gameplay and after the EXE
//      is loaded, call: NBALiveTurnoverFix::Install();
//   3. Inspect its bool result. False means no patch was applied.
//
// No DllMain is supplied here: the host plugin controls initialization.
// Install once before active gameplay; hot-patching running game code is
// not thread-safe. Uninstall is intentionally omitted for the same reason.

#if !defined(_MSC_VER) || !defined(_M_IX86)
#error NBALiveTurnoverFix requires MSVC targeting 32-bit x86 (Win32).
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace NBALiveTurnoverFix {

// The hook has an x86-specific instruction replay. Other game editions may
// reuse it ONLY if their instruction bytes match kOverwrittenBytes exactly.
struct GameProfile {
    const char* name;
    DWORD entryPointRva;
    std::uintptr_t imageBase;
    std::uintptr_t hookSite;
    std::uintptr_t resumeAt;
    std::uintptr_t relevantListenerReturn;
    std::uintptr_t commonDispatcherReturn;
    std::uint32_t listenerReturnStackOffset;
    std::uint32_t dispatcherReturnStackOffset;
    std::uintptr_t gameStateGlobal;
    std::uint32_t activeEventPtrOffset;
    std::uint16_t expectedEventState;
    std::uintptr_t diagnosticOriginalSite;
};

// These values belong to the SPECIFIC NBA Live 06 binary analyzed above.
// Never copy them unchanged into a 2005/07/08 profile.
static const GameProfile kProfiles[] = {
    {
        "NBA Live 06 PC",
        0x0000109F,       // PE OptionalHeader.AddressOfEntryPoint
        0x00400000,       // verified original image base
        0x004AC384,       // original turnover/stat counter instruction
        0x004AC38B,       // first instruction after 7 overwritten bytes
        0x0063B7FA,       // return from dispatch [vtable+21Ch]
        0x00480DF6,       // common statistics/event dispatcher return
        0x18,             // offsets relative to ESP at hook entry
        0x20,
        0x00D206AC,       // -> game-state object
        0x00001ABC,       // gameState+1ABC -> CURRENT event object
        0x0082,           // confirmed event state at the writer
        0x0088A961        // MUST be original '6A 01', not test '6A 00'
    }
};

static constexpr BYTE kOriginalDiagnosticBytes[2] = { 0x6A, 0x01 };

static constexpr BYTE kOverwrittenBytes[7] = {
    0x66, 0xFF, 0x04, 0x57, // inc word ptr [edi+edx*2]
    0x83, 0xF9, 0xFF        // cmp ecx,-1
};

// pushfd followed by pushad produces this layout at ESP.
#pragma pack(push, 1)
struct SavedCpuContext {
    DWORD edi;              // +00
    DWORD esi;              // +04
    DWORD ebp;              // +08
    DWORD espAfterPushfd;   // +0C = ESP at start of PUSHAD
    DWORD ebx;              // +10
    DWORD edx;              // +14
    DWORD ecx;              // +18
    DWORD eax;              // +1C
    DWORD flags;            // +20 from PUSHFD
};
#pragma pack(pop)
static_assert(sizeof(SavedCpuContext) == 36, "Unexpected pushad layout");
static_assert(offsetof(SavedCpuContext, espAfterPushfd) == 12, "Unexpected saved ESP offset");

static const GameProfile* g_profile = nullptr;
static bool g_installed = false;

// Referenced by MSVC's inline assembly: keep this as an actual data object.
extern "C" volatile DWORD g_nbaTurnoverResumeAddress = 0;

// Do not inline this into the naked function. Return 1 only on a COMPLETE
// match; all ambiguous events take the original statistics path.
extern "C" __declspec(noinline) int __cdecl
ShouldSuppressTurnoverIncrement(const SavedCpuContext* saved) noexcept {
    const GameProfile* const p = g_profile;
    if (!p || !saved) {
        return 0;
    }

    // The 06 captures show these stack entries at EIP=004AC384:
    //   [original ESP+18h] = 0063B7FA (out-of-bounds listener)
    //   [original ESP+20h] = 00480DF6 (common dispatcher)
    // We do NOT rely on a hardcoded ESP or the address 00CE6EE8.
    __try {
        const std::uintptr_t originalEsp =
            static_cast<std::uintptr_t>(saved->espAfterPushfd) + sizeof(DWORD);

        const DWORD returnFromListener =
            *reinterpret_cast<const volatile DWORD*>(
                originalEsp + p->listenerReturnStackOffset);
        if (returnFromListener != p->relevantListenerReturn) {
            return 0;
        }

        const DWORD returnFromDispatcher =
            *reinterpret_cast<const volatile DWORD*>(
                originalEsp + p->dispatcherReturnStackOffset);
        if (returnFromDispatcher != p->commonDispatcherReturn) {
            return 0;
        }

        // Disassembly at 0088A900 establishes this pointer chain:
        //   mov eax, [00D206AC]
        //   mov eax, [eax+1ABCh]
        // This resolves the active event dynamically.
        const std::uintptr_t gameState =
            *reinterpret_cast<const volatile DWORD*>(p->gameStateGlobal);
        if (gameState < 0x10000) {
            return 0;
        }

        const std::uintptr_t currentEvent =
            *reinterpret_cast<const volatile DWORD*>(gameState + p->activeEventPtrOffset);
        if (currentEvent < 0x10000) {
            return 0;
        }

        const WORD state =
            *reinterpret_cast<const volatile WORD*>(currentEvent + 0x02);
        const WORD side04 =
            *reinterpret_cast<const volatile WORD*>(currentEvent + 0x04);
        const WORD side06 =
            *reinterpret_cast<const volatile WORD*>(currentEvent + 0x06);

        if (state != p->expectedEventState) {
            return 0;
        }

        // Fail closed on unknown encodings. In confirmed captures,
        // side 0 and side 5 are the two team sides.
        const bool valid04 = (side04 == 0 || side04 == 5);
        const bool valid06 = (side06 == 0 || side06 == 5);
        if (!valid04 || !valid06) {
            return 0;
        }

        return side04 != side06 ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Invalid/transient game pointers must never crash the game.
        return 0;
    }
}

// MSVC x86 __declspec(naked): no compiler-generated prologue/epilogue.
// The CPU state is restored BEFORE we replay the original instruction(s).
extern "C" __declspec(naked) void TurnoverIncrementHook() {
    __asm {
        pushfd
        pushad
        mov eax, esp
        push eax
        call ShouldSuppressTurnoverIncrement
        add esp, 4
        test eax, eax
        jnz suppress_increment

        popad
        popfd
        inc word ptr [edi + edx*2]
        cmp ecx, -1
        jmp dword ptr [g_nbaTurnoverResumeAddress]

    suppress_increment:
        popad
        popfd
        // The user verified that skipping THIS increment, with
        // the original 0088A961 = '6A 01', prevents phantom TOV.
        cmp ecx, -1
        jmp dword ptr [g_nbaTurnoverResumeAddress]
    }
}

// The patch is limited to the exact executable/address/instruction layout.
// A different game edition or a modified instruction site is NOT patched.
static const GameProfile* DetectProfile() noexcept {
    BYTE* const base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    if (!base) {
        return nullptr;
    }

    __try {
        const IMAGE_DOS_HEADER* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            dos->e_lfanew > 0x1000) {
            return nullptr;
        }
        const IMAGE_NT_HEADERS32* const nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            return nullptr;
        }

        for (const GameProfile& p : kProfiles) {
            if (reinterpret_cast<std::uintptr_t>(base) != p.imageBase ||
                nt->OptionalHeader.AddressOfEntryPoint != p.entryPointRva) {
                continue;
            }
            if (std::memcmp(reinterpret_cast<const void*>(p.hookSite),
                            kOverwrittenBytes, sizeof(kOverwrittenBytes)) != 0) {
                continue;
            }
            // Refuse to install if the earlier debugger experiment is active.
            if (p.diagnosticOriginalSite &&
                std::memcmp(reinterpret_cast<const void*>(p.diagnosticOriginalSite),
                            kOriginalDiagnosticBytes,
                            sizeof(kOriginalDiagnosticBytes)) != 0) {
                continue;
            }
            return &p;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

// Public integration function. Call this once during the hosting ASI/DLL's
// normal startup path, NOT while gameplay threads are executing these bytes.
bool Install() noexcept {
    if (g_installed) {
        return true;
    }

    const GameProfile* const p = DetectProfile();
    if (!p) {
        return false; // 2005/07/08 and unknown 06 binaries: no changes
    }

    const std::intptr_t relative =
        reinterpret_cast<std::intptr_t>(&TurnoverIncrementHook) -
        (static_cast<std::intptr_t>(p->hookSite) + 5);
    if (relative < std::numeric_limits<std::int32_t>::min() ||
        relative > std::numeric_limits<std::int32_t>::max()) {
        return false; // 32-bit near JMP cannot reach this DLL
    }

    BYTE replacement[sizeof(kOverwrittenBytes)] = {
        0xE9, 0, 0, 0, 0, 0x90, 0x90 // jmp rel32; nop; nop
    };
    const auto displacement = static_cast<std::int32_t>(relative);
    std::memcpy(&replacement[1], &displacement, sizeof(displacement));

    // Ensure the hook's C helper and continuation target are initialized
    // before the game can execute the detoured bytes.
    g_nbaTurnoverResumeAddress = static_cast<DWORD>(p->resumeAt);
    g_profile = p;

    DWORD oldProtection = 0;
    void* const patchAddress = reinterpret_cast<void*>(p->hookSite);
    if (!VirtualProtect(patchAddress, sizeof(replacement),
                        PAGE_EXECUTE_READWRITE, &oldProtection)) {
        g_profile = nullptr;
        return false;
    }

    // Do not patch when another mod has changed the instructions since
    // detection. Keep game state unchanged on this failure.
    if (std::memcmp(patchAddress, kOverwrittenBytes,
                    sizeof(kOverwrittenBytes)) != 0) {
        DWORD ignored = 0;
        VirtualProtect(patchAddress, sizeof(replacement), oldProtection, &ignored);
        g_profile = nullptr;
        return false;
    }

    std::memcpy(patchAddress, replacement, sizeof(replacement));
    FlushInstructionCache(GetCurrentProcess(), patchAddress, sizeof(replacement));

    DWORD ignored = 0;
    VirtualProtect(patchAddress, sizeof(replacement), oldProtection, &ignored);
    g_installed = true;
    return true;
}

bool IsInstalled() noexcept {
    return g_installed;
}

} // namespace NBALiveTurnoverFix
