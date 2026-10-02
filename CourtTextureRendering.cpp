#include "plugin-std.h"

#include <Windows.h>
#include <d3d9.h>

#include <cerrno>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace plugin;

namespace {

    // -----------------------------------------------------------------------------
    // NBA Live 06 court texture research patch
    // -----------------------------------------------------------------------------
    //
    // Part 1: gNBACourt_RMRuntime court-specific float state.
    //
    // Verified against the NBA Live 06 1.0 NOCD executable supplied 2026-10-02:
    //
    //   009F72CE  mov  ecx,[edi]
    //   009F72D0  push 0C0400000h       ; -3.0f
    //   009F72D5  push 11h              ; NBACourt internal float-state index
    //   009F72D7  lea  eax,[ebp-4]
    //   009F72DA  push eax
    //   009F72DB  call 006FA717         ; internal float-state setter
    //
    // The first experiments prove that changing this value materially changes
    // court texture mip/LOD appearance.  Only the immediate float is patched.
    //
    // Part 2: D3D9 texture filtering and transparency-AA experiments.
    //
    // NBA Live 06 normally restores sampler states such as:
    //     MINFILTER     = LINEAR
    //     MAGFILTER     = LINEAR
    //     MIPFILTER     = LINEAR
    //     MAXANISOTROPY = 1
    //
    // MSAA cannot smooth detail inside a textured polygon.  This module can force
    // anisotropic minification and linear mip filtering as an EXPERIMENT for
    // high-resolution courts.  At this stage the D3D filtering override is global
    // to the game's texture samplers; the NBACourt LOD float remains court-only.
    // If anisotropy materially improves the court, we can isolate the filter
    // override to the NBACourt draw scope in the next pass.
    // -----------------------------------------------------------------------------

    constexpr uintptr_t kLive06MarkerAddress = 0x00BD832C;
    constexpr float     kLive06MarkerValue = 1.3333334f;

    constexpr uintptr_t kCourtLodPushAddress = 0x009F72D0;
    constexpr uintptr_t kCourtLodValueAddress = kCourtLodPushAddress + 1;
    constexpr float     kNativeCourtLodBias = -3.0f;

    // Verified NBACourt alpha-state writes inside gNBACourt_RMRuntime.
    //
    //   009F70D2  push 5      ; slot 04 = D3DRS_ALPHAFUNC = D3DCMP_GREATER
    //   009F70D4  push 04h
    //
    //   009F70EB  push 254    ; slot 05 = D3DRS_ALPHAREF
    //   009F70F0  push 05h
    //
    //   009F7105  push 1      ; slot 06 = D3DRS_ALPHATESTENABLE
    //   009F7107  push 06h
    //
    //   009F711A  push 1      ; slot 07 = D3DRS_ALPHABLENDENABLE
    //   009F711C  push 07h
    //
    // For the first controlled experiment we leave ALPHAFUNC=GREATER and
    // ALPHABLENDENABLE=1 native.  Only ALPHAREF and ALPHATESTENABLE are exposed.
    constexpr uintptr_t kCourtAlphaRefPushAddress = 0x009F70EB;
    constexpr uintptr_t kCourtAlphaRefValueAddress = kCourtAlphaRefPushAddress + 1;
    constexpr uintptr_t kCourtAlphaTestPushAddress = 0x009F7105;
    constexpr uintptr_t kCourtAlphaBlendPushAddress = 0x009F711A;

    constexpr unsigned int kNativeCourtAlphaRef = 254;
    constexpr unsigned char kNativeCourtAlphaTest = 1;
    constexpr unsigned char kNativeCourtAlphaBlend = 1;

    const unsigned char kCourtAlphaRefSignature[] = {
        0x68, 0xFE, 0x00, 0x00, 0x00, // push 254
        0x6A, 0x05,                   // push slot 05
        0x8D, 0x55, 0xFC,             // lea edx,[ebp-4]
        0x52                          // push edx
    };
    const unsigned char kCourtAlphaTestSignature[] = {
        0x6A, 0x01,                   // push 1
        0x6A, 0x06,                   // push slot 06
        0x8D, 0x45, 0xFC,             // lea eax,[ebp-4]
        0x50                          // push eax
    };
    const unsigned char kCourtAlphaBlendSignature[] = {
        0x6A, 0x01,                   // push 1
        0x6A, 0x07,                   // push slot 07
        0x8D, 0x55, 0xFC,             // lea edx,[ebp-4]
        0x52                          // push edx
    };

    const unsigned char kCourtLodSignature[] = {
        0x68, 0x00, 0x00, 0x40, 0xC0, // push -3.0f
        0x6A, 0x11,                   // push 11h
        0x8D, 0x45, 0xFC,             // lea eax,[ebp-4]
        0x50,                         // push eax
        0xE8, 0x37, 0x34, 0xD0, 0xFF // call 006FA717
    };

    int  gRequestedAnisotropy = 0;
    int  gEffectiveAnisotropy = 0;
    bool gForceTrilinear = true;
    bool gFilteringEnabled = false;

    // Experimental D3D9 transparency antialiasing.
    // 0 = off
    // 1 = alpha-to-coverage (NVIDIA ATOC or ATI/AMD A2M1)
    // 2 = NVIDIA transparency supersampling (SSAA)
    int  gTransparencyAAMode = 0;
    bool gTransparencyAAEnabled = false;
    bool gMsaaActive = false;
    UINT gActiveAdapter = D3DADAPTER_DEFAULT;
    D3DDEVTYPE gActiveDeviceType = D3DDEVTYPE_HAL;

    enum class TransparencyAABackend {
        None,
        NvidiaATOC,
        AmdA2M1,
        NvidiaSSAA
    };

    TransparencyAABackend gTransparencyAABackend =
        TransparencyAABackend::None;

    constexpr DWORD FourCC(char a, char b, char c, char d)
    {
        return
            static_cast<DWORD>(static_cast<unsigned char>(a)) |
            (static_cast<DWORD>(static_cast<unsigned char>(b)) << 8) |
            (static_cast<DWORD>(static_cast<unsigned char>(c)) << 16) |
            (static_cast<DWORD>(static_cast<unsigned char>(d)) << 24);
    }

    constexpr D3DFORMAT kFormatATOC =
        static_cast<D3DFORMAT>(FourCC('A', 'T', 'O', 'C'));
    constexpr D3DFORMAT kFormatSSAA =
        static_cast<D3DFORMAT>(FourCC('S', 'S', 'A', 'A'));
    constexpr DWORD kFormatA2M1 = FourCC('A', '2', 'M', '1');
    constexpr DWORD kFormatA2M0 = FourCC('A', '2', 'M', '0');

    using Direct3DCreate9Fn = IDirect3D9 * (WINAPI*)(UINT);
    using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
        D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    using ResetFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    using SetSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
    using CreateStateBlockFn = HRESULT(STDMETHODCALLTYPE*)(
        IDirect3DDevice9*, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**);
    using StateBlockApplyFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DStateBlock9*);

    Direct3DCreate9Fn   gOriginalDirect3DCreate9 = nullptr;
    CreateDeviceFn      gOriginalCreateDevice = nullptr;
    ResetFn             gOriginalReset = nullptr;
    SetSamplerStateFn   gOriginalSetSamplerState = nullptr;
    CreateStateBlockFn  gOriginalCreateStateBlock = nullptr;
    StateBlockApplyFn   gOriginalStateBlockApply = nullptr;

    void Log(const char* format, ...)
    {
        char body[512] = {};
        va_list args;
        va_start(args, format);
        vsnprintf_s(body, sizeof(body), _TRUNCATE, format, args);
        va_end(args);

        char line[576] = {};
        sprintf_s(line, sizeof(line), "[CourtTextureRendering] %s\n", body);
        OutputDebugStringA(line);
    }

    bool IsLive06Build()
    {
        if (FM::GetEntryPoint() != 0x0040109F)
            return false;

        return std::fabs(patch::GetFloat(kLive06MarkerAddress) -
            kLive06MarkerValue) < 0.00001f;
    }

    bool ValidateCourtLodPatchSite()
    {
        return std::memcmp(
            reinterpret_cast<const void*>(kCourtLodPushAddress),
            kCourtLodSignature,
            sizeof(kCourtLodSignature)) == 0;
    }

    bool ReadIniFloat(const char* key, float& value)
    {
        char text[64] = {};
        GetPrivateProfileStringA(
            "GRAPHICS", key, "", text,
            static_cast<DWORD>(sizeof(text)), ".\\main.ini");

        if (!text[0])
            return false;

        errno = 0;
        char* end = nullptr;
        const float parsed = std::strtof(text, &end);
        if (end == text || errno == ERANGE || !std::isfinite(parsed)) {
            Log("Invalid [GRAPHICS] %s=%s; ignored.", key, text);
            return false;
        }

        while (*end == ' ' || *end == '\t')
            ++end;
        if (*end != '\0') {
            Log("Invalid trailing characters in [GRAPHICS] %s=%s; ignored.",
                key, text);
            return false;
        }

        value = parsed;
        return true;
    }

    int ReadIniInt(const char* key, int defaultValue)
    {
        return GetPrivateProfileIntA("GRAPHICS", key, defaultValue, ".\\main.ini");
    }

    bool WriteFloatImmediate(uintptr_t address, float value)
    {
        void* target = reinterpret_cast<void*>(address);
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, sizeof(value), PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        std::memcpy(target, &value, sizeof(value));

        DWORD ignored = 0;
        VirtualProtect(target, sizeof(value), oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(value));
        return true;
    }

    bool WriteUInt32Immediate(uintptr_t address, unsigned int value)
    {
        void* target = reinterpret_cast<void*>(address);
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, sizeof(value), PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        std::memcpy(target, &value, sizeof(value));

        DWORD ignored = 0;
        VirtualProtect(target, sizeof(value), oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(value));
        return true;
    }

    bool WriteByteImmediate(uintptr_t address, unsigned char value)
    {
        void* target = reinterpret_cast<void*>(address);
        DWORD oldProtect = 0;
        if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        *reinterpret_cast<unsigned char*>(target) = value;

        DWORD ignored = 0;
        VirtualProtect(target, 1, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, 1);
        return true;
    }

    bool ReadIniOptionalInt(const char* key, int& value)
    {
        char text[64] = {};
        GetPrivateProfileStringA(
            "GRAPHICS", key, "", text,
            static_cast<DWORD>(sizeof(text)), ".\\main.ini");

        if (!text[0])
            return false;

        errno = 0;
        char* end = nullptr;
        const long parsed = std::strtol(text, &end, 10);
        if (end == text || errno == ERANGE) {
            Log("Invalid [GRAPHICS] %s=%s; ignored.", key, text);
            return false;
        }
        while (*end == ' ' || *end == '\t')
            ++end;
        if (*end != '\0') {
            Log("Invalid trailing characters in [GRAPHICS] %s=%s; ignored.", key, text);
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    }

    bool PatchPointer(void** target, void* replacement, void** original)
    {
        if (!target || !replacement)
            return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(target, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        if (original)
            *original = *target;
        *target = replacement;

        DWORD ignored = 0;
        VirtualProtect(target, sizeof(void*), oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(void*));
        return true;
    }

    bool HookImport(const char* dllName,
        const char* functionName,
        void* replacement,
        void** original)
    {
        HMODULE module = GetModuleHandleA(nullptr);
        if (!module)
            return false;

        auto* base = reinterpret_cast<unsigned char*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !dir.Size)
            return false;

        auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
            base + dir.VirtualAddress);

        for (; descriptor->Name; ++descriptor) {
            const char* importedDll =
                reinterpret_cast<const char*>(base + descriptor->Name);
            if (_stricmp(importedDll, dllName) != 0)
                continue;

            auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                base + descriptor->FirstThunk);
            auto* originalThunk = descriptor->OriginalFirstThunk
                ? reinterpret_cast<IMAGE_THUNK_DATA*>(
                    base + descriptor->OriginalFirstThunk)
                : thunk;

            for (; originalThunk->u1.AddressOfData; ++originalThunk, ++thunk) {
                if (IMAGE_SNAP_BY_ORDINAL(originalThunk->u1.Ordinal))
                    continue;

                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                    base + originalThunk->u1.AddressOfData);
                if (std::strcmp(
                    reinterpret_cast<const char*>(byName->Name),
                    functionName) != 0)
                    continue;

                return PatchPointer(
                    reinterpret_cast<void**>(&thunk->u1.Function),
                    replacement,
                    original);
            }
        }

        return false;
    }

    const char* TransparencyAABackendName()
    {
        switch (gTransparencyAABackend) {
        case TransparencyAABackend::NvidiaATOC:
            return "NVIDIA ATOC";
        case TransparencyAABackend::AmdA2M1:
            return "ATI/AMD A2M1";
        case TransparencyAABackend::NvidiaSSAA:
            return "NVIDIA transparency SSAA";
        default:
            return "none";
        }
    }

    bool ProbeFourCC(
        IDirect3D9* d3d,
        UINT adapter,
        D3DDEVTYPE deviceType,
        D3DFORMAT format)
    {
        if (!d3d)
            return false;

        // NVIDIA's SDK sample probes the vendor FOURCC as a surface format.
        // X8R8G8B8 is the documented adapter format for this extension.
        HRESULT hr = d3d->CheckDeviceFormat(
            adapter,
            deviceType,
            D3DFMT_X8R8G8B8,
            0,
            D3DRTYPE_SURFACE,
            format);

        if (SUCCEEDED(hr))
            return true;

        // Some desktop modes report A8R8G8B8 instead. Retry with the actual
        // adapter format before declaring the extension unavailable.
        D3DDISPLAYMODE mode = {};
        if (SUCCEEDED(d3d->GetAdapterDisplayMode(adapter, &mode))) {
            hr = d3d->CheckDeviceFormat(
                adapter,
                deviceType,
                mode.Format,
                0,
                D3DRTYPE_SURFACE,
                format);
        }

        return SUCCEEDED(hr);
    }

    void DisableTransparencyAA(IDirect3DDevice9* device)
    {
        if (!device)
            return;

        switch (gTransparencyAABackend) {
        case TransparencyAABackend::NvidiaATOC:
        case TransparencyAABackend::NvidiaSSAA:
            device->SetRenderState(
                D3DRS_ADAPTIVETESS_Y,
                static_cast<DWORD>(D3DFMT_UNKNOWN));
            break;

        case TransparencyAABackend::AmdA2M1:
            device->SetRenderState(D3DRS_POINTSIZE, kFormatA2M0);
            break;

        default:
            break;
        }
    }

    void ApplyTransparencyAA(IDirect3DDevice9* device)
    {
        if (!device || !gTransparencyAAEnabled || !gMsaaActive)
            return;

        switch (gTransparencyAABackend) {
        case TransparencyAABackend::NvidiaATOC:
            device->SetRenderState(
                D3DRS_ADAPTIVETESS_Y,
                static_cast<DWORD>(kFormatATOC));
            break;

        case TransparencyAABackend::AmdA2M1:
            device->SetRenderState(D3DRS_POINTSIZE, kFormatA2M1);
            break;

        case TransparencyAABackend::NvidiaSSAA:
            device->SetRenderState(
                D3DRS_ADAPTIVETESS_Y,
                static_cast<DWORD>(kFormatSSAA));
            break;

        default:
            break;
        }
    }

    void InitializeTransparencyAACourtState();

    void SelectTransparencyAABackend(
        IDirect3D9* d3d,
        UINT adapter,
        D3DDEVTYPE deviceType)
    {
        gTransparencyAABackend = TransparencyAABackend::None;

        if (!d3d || gTransparencyAAMode == 0)
            return;

        D3DADAPTER_IDENTIFIER9 id = {};
        const HRESULT idHr =
            d3d->GetAdapterIdentifier(adapter, 0, &id);

        const DWORD vendor =
            SUCCEEDED(idHr) ? id.VendorId : 0;

        Log("D3D9 adapter: vendor=%04X device=%04X description=%s.",
            static_cast<unsigned int>(vendor),
            static_cast<unsigned int>(SUCCEEDED(idHr) ? id.DeviceId : 0),
            SUCCEEDED(idHr) ? id.Description : "<unknown>");

        if (gTransparencyAAMode == 2) {
            // NVIDIA documented SSAA as a FOURCC extension. Do not send the
            // command unless the driver explicitly reports support.
            if (ProbeFourCC(d3d, adapter, deviceType, kFormatSSAA)) {
                gTransparencyAABackend =
                    TransparencyAABackend::NvidiaSSAA;
            }
            else {
                Log("COURT_TRANSPARENCY_AA=2 requested, but SSAA FOURCC is unsupported. Falling back to ATOC when available.");
                gTransparencyAAMode = 1;
            }
        }

        if (gTransparencyAAMode == 1) {
            // NVIDIA/compatible drivers expose ATOC through ADAPTIVETESS_Y and
            // report a synthetic ATOC surface format.
            if (ProbeFourCC(d3d, adapter, deviceType, kFormatATOC)) {
                gTransparencyAABackend =
                    TransparencyAABackend::NvidiaATOC;
            }
            // ATI/AMD's D3D9 A2M1 extension historically has no reliable
            // CheckDeviceFormat probe. Restrict it to the known AMD vendor ID.
            else if (vendor == 0x1002u) {
                gTransparencyAABackend =
                    TransparencyAABackend::AmdA2M1;
            }
        }

        gTransparencyAAEnabled =
            gTransparencyAABackend != TransparencyAABackend::None;

        Log("Transparency AA backend: %s.",
            TransparencyAABackendName());

        if (gTransparencyAAEnabled &&
            gTransparencyAAMode == 1 &&
            gMsaaActive) {
            InitializeTransparencyAACourtState();
        }
        else if (gTransparencyAAEnabled &&
            gTransparencyAAMode != 0 &&
            !gMsaaActive) {
            Log("Transparency AA backend is available, but the active device is not multisampled; NBACourt alpha state remains native.");
        }
    }

    void ApplyTextureFiltering(IDirect3DDevice9* device)
    {
        if (!device || !gFilteringEnabled || gEffectiveAnisotropy <= 1)
            return;

        // NBA Live uses several sampler slots.  Reapply after state-block restores
        // so the game's cached LINEAR / MAX_ANISO=1 state cannot immediately undo
        // the experiment.
        for (DWORD sampler = 0; sampler < 8; ++sampler) {
            if (gOriginalSetSamplerState) {
                gOriginalSetSamplerState(
                    device, sampler, D3DSAMP_MINFILTER,
                    static_cast<DWORD>(D3DTEXF_ANISOTROPIC));
                gOriginalSetSamplerState(
                    device, sampler, D3DSAMP_MAGFILTER,
                    static_cast<DWORD>(D3DTEXF_LINEAR));
                if (gForceTrilinear) {
                    gOriginalSetSamplerState(
                        device, sampler, D3DSAMP_MIPFILTER,
                        static_cast<DWORD>(D3DTEXF_LINEAR));
                }
                gOriginalSetSamplerState(
                    device, sampler, D3DSAMP_MAXANISOTROPY,
                    static_cast<DWORD>(gEffectiveAnisotropy));
            }
        }
    }

    HRESULT STDMETHODCALLTYPE HookSetSamplerState(
        IDirect3DDevice9* device,
        DWORD sampler,
        D3DSAMPLERSTATETYPE type,
        DWORD value)
    {
        if (gFilteringEnabled && sampler < 8 && gEffectiveAnisotropy > 1) {
            switch (type) {
            case D3DSAMP_MINFILTER:
                value = static_cast<DWORD>(D3DTEXF_ANISOTROPIC);
                break;
            case D3DSAMP_MAGFILTER:
                value = static_cast<DWORD>(D3DTEXF_LINEAR);
                break;
            case D3DSAMP_MIPFILTER:
                if (gForceTrilinear)
                    value = static_cast<DWORD>(D3DTEXF_LINEAR);
                break;
            case D3DSAMP_MAXANISOTROPY:
                value = static_cast<DWORD>(gEffectiveAnisotropy);
                break;
            default:
                break;
            }
        }

        return gOriginalSetSamplerState
            ? gOriginalSetSamplerState(device, sampler, type, value)
            : D3DERR_INVALIDCALL;
    }

    HRESULT STDMETHODCALLTYPE HookStateBlockApply(IDirect3DStateBlock9* block)
    {
        const HRESULT hr = gOriginalStateBlockApply
            ? gOriginalStateBlockApply(block)
            : D3DERR_INVALIDCALL;

        if (SUCCEEDED(hr) && block) {
            IDirect3DDevice9* device = nullptr;
            if (SUCCEEDED(block->GetDevice(&device)) && device) {
                ApplyTextureFiltering(device);
                ApplyTransparencyAA(device);
                device->Release();
            }
        }

        return hr;
    }

    void HookStateBlockObject(IDirect3DStateBlock9* block)
    {
        if (!block)
            return;

        void** vtable = *reinterpret_cast<void***>(block);
        if (!vtable)
            return;

        // IDirect3DStateBlock9::Apply is vtable slot 5.
        void* current = vtable[5];
        if (current == reinterpret_cast<void*>(&HookStateBlockApply))
            return;

        void* previous = nullptr;
        if (PatchPointer(
            &vtable[5],
            reinterpret_cast<void*>(&HookStateBlockApply),
            &previous)) {
            gOriginalStateBlockApply =
                reinterpret_cast<StateBlockApplyFn>(previous);
            Log("Hooked IDirect3DStateBlock9::Apply; filtering will be restored after state blocks.");
        }
    }

    HRESULT STDMETHODCALLTYPE HookCreateStateBlock(
        IDirect3DDevice9* device,
        D3DSTATEBLOCKTYPE type,
        IDirect3DStateBlock9** block)
    {
        const HRESULT hr = gOriginalCreateStateBlock
            ? gOriginalCreateStateBlock(device, type, block)
            : D3DERR_INVALIDCALL;

        if (SUCCEEDED(hr) && block && *block)
            HookStateBlockObject(*block);

        return hr;
    }

    HRESULT STDMETHODCALLTYPE HookReset(
        IDirect3DDevice9* device,
        D3DPRESENT_PARAMETERS* params)
    {
        const HRESULT hr = gOriginalReset
            ? gOriginalReset(device, params)
            : D3DERR_INVALIDCALL;

        if (SUCCEEDED(hr)) {
            gMsaaActive =
                params &&
                params->MultiSampleType != D3DMULTISAMPLE_NONE;
            ApplyTextureFiltering(device);
            ApplyTransparencyAA(device);
        }

        return hr;
    }

    void HookDevice(IDirect3DDevice9* device)
    {
        if (!device)
            return;

        D3DCAPS9 caps = {};
        if (SUCCEEDED(device->GetDeviceCaps(&caps))) {
            const int hardwareMax =
                caps.MaxAnisotropy > 0
                ? static_cast<int>(caps.MaxAnisotropy)
                : 1;
            gEffectiveAnisotropy = gRequestedAnisotropy;
            if (gEffectiveAnisotropy > hardwareMax)
                gEffectiveAnisotropy = hardwareMax;
            if (gEffectiveAnisotropy < 1)
                gEffectiveAnisotropy = 1;

            Log("D3D9 caps: MaxAnisotropy=%u; requested=%d effective=%d.",
                caps.MaxAnisotropy,
                gRequestedAnisotropy,
                gEffectiveAnisotropy);
        }
        else {
            gEffectiveAnisotropy = gRequestedAnisotropy;
        }

        void** vtable = *reinterpret_cast<void***>(device);
        if (!vtable)
            return;

        // IDirect3DDevice9 slots:
        //   Reset            = 16
        //   CreateStateBlock = 59
        //   SetSamplerState  = 69
        if (vtable[16] != reinterpret_cast<void*>(&HookReset)) {
            void* previous = nullptr;
            if (PatchPointer(&vtable[16], reinterpret_cast<void*>(&HookReset), &previous))
                gOriginalReset = reinterpret_cast<ResetFn>(previous);
        }

        if (vtable[59] != reinterpret_cast<void*>(&HookCreateStateBlock)) {
            void* previous = nullptr;
            if (PatchPointer(
                &vtable[59],
                reinterpret_cast<void*>(&HookCreateStateBlock),
                &previous)) {
                gOriginalCreateStateBlock =
                    reinterpret_cast<CreateStateBlockFn>(previous);
            }
        }

        if (vtable[69] != reinterpret_cast<void*>(&HookSetSamplerState)) {
            void* previous = nullptr;
            if (PatchPointer(
                &vtable[69],
                reinterpret_cast<void*>(&HookSetSamplerState),
                &previous)) {
                gOriginalSetSamplerState =
                    reinterpret_cast<SetSamplerStateFn>(previous);
            }
        }

        ApplyTextureFiltering(device);
        ApplyTransparencyAA(device);

        if (gFilteringEnabled)
            Log("D3D9 anisotropic filtering hook installed.");
        if (gTransparencyAAEnabled)
            Log("D3D9 transparency AA state applied: %s.",
                TransparencyAABackendName());
    }

    HRESULT STDMETHODCALLTYPE HookCreateDevice(
        IDirect3D9* d3d,
        UINT adapter,
        D3DDEVTYPE deviceType,
        HWND focusWindow,
        DWORD behaviorFlags,
        D3DPRESENT_PARAMETERS* params,
        IDirect3DDevice9** device)
    {
        const HRESULT hr = gOriginalCreateDevice
            ? gOriginalCreateDevice(
                d3d,
                adapter,
                deviceType,
                focusWindow,
                behaviorFlags,
                params,
                device)
            : D3DERR_INVALIDCALL;

        if (SUCCEEDED(hr) && device && *device) {
            gActiveAdapter = adapter;
            gActiveDeviceType = deviceType;
            gMsaaActive =
                params &&
                params->MultiSampleType != D3DMULTISAMPLE_NONE;

            SelectTransparencyAABackend(
                d3d,
                adapter,
                deviceType);

            Log("CreateDevice MSAA=%s type=%u.",
                gMsaaActive ? "on" : "off",
                params
                ? static_cast<unsigned int>(params->MultiSampleType)
                : 0u);

            HookDevice(*device);
        }

        return hr;
    }

    IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion)
    {
        IDirect3D9* d3d = gOriginalDirect3DCreate9
            ? gOriginalDirect3DCreate9(sdkVersion)
            : nullptr;

        if (!d3d)
            return nullptr;

        void** vtable = *reinterpret_cast<void***>(d3d);
        if (!vtable)
            return d3d;

        // IDirect3D9::CreateDevice is vtable slot 16.
        if (vtable[16] != reinterpret_cast<void*>(&HookCreateDevice)) {
            void* previous = nullptr;
            if (PatchPointer(
                &vtable[16],
                reinterpret_cast<void*>(&HookCreateDevice),
                &previous)) {
                gOriginalCreateDevice = reinterpret_cast<CreateDeviceFn>(previous);
                Log("Hooked IDirect3D9::CreateDevice for texture filtering experiment.");
            }
        }

        return d3d;
    }

    bool InstallFilteringHooks()
    {
        void* original = nullptr;
        if (!HookImport(
            "d3d9.dll",
            "Direct3DCreate9",
            reinterpret_cast<void*>(&HookDirect3DCreate9),
            &original)) {
            Log("Could not hook d3d9.dll!Direct3DCreate9; D3D court experiments disabled.");
            return false;
        }

        gOriginalDirect3DCreate9 = reinterpret_cast<Direct3DCreate9Fn>(original);
        Log("D3D9 court-render interception installed.");
        return true;
    }

    void InitializeCourtLodPatch()
    {
        float requestedBias = kNativeCourtLodBias;
        if (!ReadIniFloat("COURT_LOD_BIAS", requestedBias)) {
            Log("COURT_LOD_BIAS not specified; native Live 06 value %.1f retained.",
                kNativeCourtLodBias);
            return;
        }

        if (requestedBias < -8.0f)
            requestedBias = -8.0f;
        else if (requestedBias > 8.0f)
            requestedBias = 8.0f;

        if (!ValidateCourtLodPatchSite()) {
            Log("Live 06 detected, but gNBACourt_RMRuntime signature validation failed at %08lX. LOD patch skipped.",
                static_cast<unsigned long>(kCourtLodPushAddress));
            return;
        }

        const float before = patch::GetFloat(kCourtLodValueAddress);
        if (!WriteFloatImmediate(kCourtLodValueAddress, requestedBias)) {
            Log("Could not patch court float state at %08lX.",
                static_cast<unsigned long>(kCourtLodValueAddress));
            return;
        }

        const float after = patch::GetFloat(kCourtLodValueAddress);
        Log("Live 06 NBACourt float state patched: %.3f -> %.3f at %08lX.",
            before,
            after,
            static_cast<unsigned long>(kCourtLodValueAddress));
    }

    bool ValidateCourtAlphaPatchSites()
    {
        return
            std::memcmp(
                reinterpret_cast<const void*>(kCourtAlphaRefPushAddress),
                kCourtAlphaRefSignature,
                sizeof(kCourtAlphaRefSignature)) == 0 &&
            std::memcmp(
                reinterpret_cast<const void*>(kCourtAlphaTestPushAddress),
                kCourtAlphaTestSignature,
                sizeof(kCourtAlphaTestSignature)) == 0 &&
            std::memcmp(
                reinterpret_cast<const void*>(kCourtAlphaBlendPushAddress),
                kCourtAlphaBlendSignature,
                sizeof(kCourtAlphaBlendSignature)) == 0;
    }

    void InitializeTransparencyAACourtState()
    {
        if (gTransparencyAAMode != 1 ||
            !gTransparencyAAEnabled)
            return;

        if (!ValidateCourtAlphaPatchSites()) {
            Log("Transparency AA requested, but NBACourt alpha-state signature validation failed. Native court alpha state retained.");
            return;
        }

        // Alpha-to-coverage needs the texture's intermediate alpha values to
        // survive until the multisample coverage step. Native Live 06 uses
        // ALPHATEST GREATER 254, which discards those values first.
        //
        // For this experiment, ATOC mode changes only NBACourt:
        //   ALPHATESTENABLE  1 -> 0
        //   ALPHABLENDENABLE 1 -> 0
        //
        // Opaque texels still produce full coverage; intermediate alpha produces
        // partial MSAA coverage. Other render methods keep their native states.
        const bool testOk = WriteByteImmediate(
            kCourtAlphaTestPushAddress + 1,
            0);

        const bool blendOk = WriteByteImmediate(
            kCourtAlphaBlendPushAddress + 1,
            0);

        if (testOk && blendOk) {
            Log("ATOC court state enabled: NBACourt ALPHATEST=0 ALPHABLEND=0. Native ALPHAFUNC/ALPHAREF bytes remain unchanged.");
        }
        else {
            Log("Failed to patch NBACourt alpha state for ATOC experiment.");
        }
    }

    void InitializeCourtAlphaPatch()
    {
        if (gTransparencyAAMode == 1) {
            Log("COURT_TRANSPARENCY_AA=1 owns NBACourt alpha state; COURT_ALPHA_REF/COURT_ALPHA_TEST overrides are ignored for this run.");
            return;
        }

        int alphaRef = static_cast<int>(kNativeCourtAlphaRef);
        int alphaTest = static_cast<int>(kNativeCourtAlphaTest);

        const bool hasRef = ReadIniOptionalInt("COURT_ALPHA_REF", alphaRef);
        const bool hasTest = ReadIniOptionalInt("COURT_ALPHA_TEST", alphaTest);

        if (!hasRef && !hasTest) {
            Log("No verified court alpha overrides specified; native ALPHAREF=%u and ALPHATEST=1 retained.",
                kNativeCourtAlphaRef);
            return;
        }

        if (!ValidateCourtAlphaPatchSites()) {
            Log("Live 06 detected, but verified NBACourt alpha signature validation failed. Alpha patch skipped.");
            return;
        }

        if (alphaRef < 0) alphaRef = 0;
        if (alphaRef > 255) alphaRef = 255;
        alphaTest = alphaTest ? 1 : 0;

        bool ok = true;

        if (hasRef) {
            const unsigned int before = *reinterpret_cast<const unsigned int*>(
                kCourtAlphaRefValueAddress);
            if (!WriteUInt32Immediate(
                kCourtAlphaRefValueAddress,
                static_cast<unsigned int>(alphaRef))) {
                Log("Failed to write NBACourt ALPHAREF at %08lX.",
                    static_cast<unsigned long>(kCourtAlphaRefValueAddress));
                ok = false;
            }
            else {
                const unsigned int after = *reinterpret_cast<const unsigned int*>(
                    kCourtAlphaRefValueAddress);
                Log("NBACourt ALPHAREF patched: %u -> %u at %08lX.",
                    before, after,
                    static_cast<unsigned long>(kCourtAlphaRefValueAddress));
            }
        }

        if (hasTest) {
            const unsigned char before = patch::GetUChar(kCourtAlphaTestPushAddress + 1);
            if (!WriteByteImmediate(
                kCourtAlphaTestPushAddress + 1,
                static_cast<unsigned char>(alphaTest))) {
                Log("Failed to write NBACourt ALPHATESTENABLE at %08lX.",
                    static_cast<unsigned long>(kCourtAlphaTestPushAddress + 1));
                ok = false;
            }
            else {
                const unsigned char after = patch::GetUChar(kCourtAlphaTestPushAddress + 1);
                Log("NBACourt ALPHATESTENABLE patched: %u -> %u at %08lX.",
                    static_cast<unsigned int>(before),
                    static_cast<unsigned int>(after),
                    static_cast<unsigned long>(kCourtAlphaTestPushAddress + 1));
            }
        }

        if (ok) {
            Log("Verified native alpha state left intact otherwise: ALPHAFUNC=GREATER, ALPHABLENDENABLE=1.");
        }
    }

} // namespace

void InitializeCourtTextureRendering()
{
    if (!IsLive06Build())
        return;

    InitializeCourtLodPatch();

    gTransparencyAAMode =
        ReadIniInt("COURT_TRANSPARENCY_AA", 0);
    if (gTransparencyAAMode < 0 ||
        gTransparencyAAMode > 2) {
        Log("Invalid COURT_TRANSPARENCY_AA=%d; using 0.",
            gTransparencyAAMode);
        gTransparencyAAMode = 0;
    }

    // Mode 1 (ATOC) will bypass NBACourt's native GREATER 254 cutout only
    // after the D3D9 driver confirms an ATOC backend is available.
    // Mode 2 (NVIDIA transparency SSAA) intentionally keeps the native alpha
    // test because the vendor feature supersamples alpha-tested fragments.
    InitializeCourtAlphaPatch();

    gRequestedAnisotropy = ReadIniInt("COURT_ANISOTROPY", 0);
    gForceTrilinear = ReadIniInt("COURT_TRILINEAR", 1) != 0;

    if (gRequestedAnisotropy <= 1) {
        gRequestedAnisotropy = 0;
        gEffectiveAnisotropy = 1;
        gFilteringEnabled = false;
        Log("COURT_ANISOTROPY is 0/1; D3D filtering override disabled.");
    }
    else {
        // Keep the experimental range conventional and deterministic.
        if (gRequestedAnisotropy <= 2)
            gRequestedAnisotropy = 2;
        else if (gRequestedAnisotropy <= 4)
            gRequestedAnisotropy = 4;
        else if (gRequestedAnisotropy <= 8)
            gRequestedAnisotropy = 8;
        else
            gRequestedAnisotropy = 16;

        gEffectiveAnisotropy = gRequestedAnisotropy;
        gFilteringEnabled = true;

        Log("Requested filtering: anisotropy=%d, trilinear=%d.",
            gRequestedAnisotropy,
            gForceTrilinear ? 1 : 0);
    }

    if (gTransparencyAAMode != 0) {
        Log("Requested transparency AA mode=%d (1=ATOC, 2=NVIDIA SSAA).",
            gTransparencyAAMode);
    }

    if (!gFilteringEnabled &&
        gTransparencyAAMode == 0) {
        return;
    }

    if (!InstallFilteringHooks()) {
        gFilteringEnabled = false;
        gTransparencyAAEnabled = false;
        return;
    }
}
