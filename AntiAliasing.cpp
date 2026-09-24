#include "plugin-std.h"
#include <Windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <new>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <string>
#include <vector>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")


using namespace plugin;

namespace {

enum class LiveGame
{
    Unknown,
    Live05,
    Live06,
    Live07,
    Live08
};

LiveGame gGame = LiveGame::Unknown;
bool gEnabled = false;
unsigned int gRequestedSamples = 4;
unsigned int gCreateDeviceCount = 0;
unsigned int gResetCount = 0;

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


typedef IDirect3D9* (WINAPI* Direct3DCreate9Fn)(UINT);
typedef HRESULT (STDMETHODCALLTYPE* CreateDeviceFn)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE* ResetFn)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

typedef HRESULT (STDMETHODCALLTYPE* CreateTextureFn)(
    IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
    IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE* CreateRenderTargetFn)(
    IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE,
    DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE* CreateDepthStencilSurfaceFn)(
    IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE,
    DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE* StretchRectFn)(
    IDirect3DDevice9*, IDirect3DSurface9*, const RECT*,
    IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT (STDMETHODCALLTYPE* SetRenderTargetFn)(
    IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT (STDMETHODCALLTYPE* SetDepthStencilSurfaceFn)(
    IDirect3DDevice9*, IDirect3DSurface9*);

typedef HRESULT (STDMETHODCALLTYPE* SetRenderStateFn)(
    IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);

typedef HRESULT (STDMETHODCALLTYPE* CreateStateBlockFn)(
    IDirect3DDevice9*, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**);
typedef HRESULT (STDMETHODCALLTYPE* BeginStateBlockFn)(
    IDirect3DDevice9*);
typedef HRESULT (STDMETHODCALLTYPE* EndStateBlockFn)(
    IDirect3DDevice9*, IDirect3DStateBlock9**);
typedef HRESULT (STDMETHODCALLTYPE* StateBlockApplyFn)(
    IDirect3DStateBlock9*);

Direct3DCreate9Fn gOriginalDirect3DCreate9 = nullptr;
CreateDeviceFn gOriginalCreateDevice = nullptr;
ResetFn gOriginalReset = nullptr;

CreateTextureFn gOriginalCreateTexture = nullptr;
CreateRenderTargetFn gOriginalCreateRenderTarget = nullptr;
CreateDepthStencilSurfaceFn gOriginalCreateDepthStencilSurface = nullptr;
StretchRectFn gOriginalStretchRect = nullptr;
SetRenderTargetFn gOriginalSetRenderTarget = nullptr;
SetDepthStencilSurfaceFn gOriginalSetDepthStencilSurface = nullptr;

SetRenderStateFn gOriginalSetRenderState = nullptr;
unsigned int gMsaaStateWriteCount = 0;

CreateStateBlockFn gOriginalCreateStateBlock = nullptr;
BeginStateBlockFn gOriginalBeginStateBlock = nullptr;
EndStateBlockFn gOriginalEndStateBlock = nullptr;
StateBlockApplyFn gOriginalStateBlockApply = nullptr;
unsigned int gStateBlockCreateCount = 0;
unsigned int gStateBlockBeginCount = 0;
unsigned int gStateBlockEndCount = 0;
unsigned int gStateBlockApplyCount = 0;

constexpr uintptr_t kLive05TakeScreenshot = 0x005F2EC0;
constexpr size_t kLive05TakeScreenshotPatchSize = 6;
constexpr uintptr_t kLive06TakeScreenshot = 0x00600530;
constexpr size_t kLive06TakeScreenshotPatchSize = 6;
constexpr uintptr_t kLive07TakeScreenshot = 0x0064AE90;
constexpr size_t kLive07TakeScreenshotPatchSize = 6;
constexpr uintptr_t kLive08TakeScreenshot = 0x0066B370;
constexpr size_t kLive08TakeScreenshotPatchSize = 6;

// The game's bulk render-state restore contains a SetRenderState call for
// D3DRS_MULTISAMPLEANTIALIAS (0xA1). 06 and 07 use the same logical sequence,
// but not necessarily the same absolute addresses. v15 finds it by signature.

IDirect3DSurface9* gLastRT[4] = { nullptr, nullptr, nullptr, nullptr };
IDirect3DSurface9* gLastDepth = nullptr;
unsigned int gStretchRectLogCount = 0;
constexpr unsigned int kStretchRectLogLimit = 120;

// NBA Live 2005 renderer globals mapped directly from nba2005.exe.
constexpr uintptr_t kLive05PresentParams     = 0x00C56BC8;
constexpr uintptr_t kLive05D3D9Ptr           = 0x00C56CD0;
constexpr uintptr_t kLive05DevicePtr         = 0x00C56CD4;
constexpr uintptr_t kLive05DeviceType        = 0x00BF213C;

constexpr uintptr_t kLive05CreatePrepSites[] = {
    0x006DBA24,
    0x006DBA4F,
    0x006DBA72,
    0x006DBAD4,
    0x006DCBCD
};

constexpr uintptr_t kLive05ResetPrepSites[] = {
    0x006DD462,
    0x006DD4C5
};

// 006E4C92  FF 35 50 89 C5 00   push dword ptr [00C58950]
// ...
// 006E4C9F  68 A1 00 00 00      push D3DRS_MULTISAMPLEANTIALIAS
constexpr uintptr_t kLive05NativeMsaaRestore = 0x006E4C92;

// NBA Live 07 1.1 NOCD renderer globals mapped directly from nbalive07.exe.
constexpr uintptr_t kLive07PresentParams     = 0x00CC95D8;
constexpr uintptr_t kLive07D3D9Ptr           = 0x00CC96E0;
constexpr uintptr_t kLive07DevicePtr         = 0x00CC96E4;
constexpr uintptr_t kLive07DeviceType        = 0x00C55BA4;

// Game-owned CreateDevice preparation sites. Each original instruction is:
//     A1 E0 96 CC 00    mov eax, dword ptr [00CC96E0]
constexpr uintptr_t kLive07CreatePrepSites[] = {
    0x00429F43,
    0x0042BBAA,
    0x0042BC5A,
    0x0042CD46
};

// Game-owned Reset preparation sites. Each original instruction is:
//     A1 E4 96 CC 00    mov eax, dword ptr [00CC96E4]
constexpr uintptr_t kLive07ResetPrepSites[] = {
    0x0042D5D1,
    0x0042D634
};

// Confirmed Live 07 bulk renderer-state replay for
// D3DRS_MULTISAMPLEANTIALIAS (0xA1):
// 0043698E  FF 35 90 CF CC 00   push dword ptr [00CCCF90]
// ...
// 0043699B  68 A1 00 00 00      push 0A1h
constexpr uintptr_t kLive07NativeMsaaRestore = 0x0043698E;

// NBA Live 08 1.0 NOCD renderer globals mapped directly from nbalive08.exe.
constexpr uintptr_t kLive08PresentParams     = 0x00D84F68;
constexpr uintptr_t kLive08D3D9Ptr           = 0x00D85070;
constexpr uintptr_t kLive08DevicePtr         = 0x00D85074;
constexpr uintptr_t kLive08DeviceType        = 0x00D12BC4;

constexpr uintptr_t kLive08CreatePrepSites[] = {
    0x0042C773,
    0x0042E3DA,
    0x0042E48A,
    0x0042F576
};

constexpr uintptr_t kLive08ResetPrepSites[] = {
    0x0042FE01,
    0x0042FE64
};

// 004391BE  FF 35 20 89 D8 00   push dword ptr [00D88920]
// ...
// 004391CB  68 A1 00 00 00      push D3DRS_MULTISAMPLEANTIALIAS
constexpr uintptr_t kLive08NativeMsaaRestore = 0x004391BE;

void Log(const char* fmt, ...)
{
    if (!gDebugLoggingEnabled)
        return;

    char message[1024] = {};
    va_list args;
    va_start(args, fmt);
#if defined(_MSC_VER)
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);
#else
    vsnprintf(message, sizeof(message), fmt, args);
#endif
    va_end(args);

    std::printf("[MSAA] %s\n", message);
    std::fflush(stdout);

    char dbg[1100] = {};
#if defined(_MSC_VER)
    sprintf_s(dbg, "[MSAA] %s\n", message);
#else
    snprintf(dbg, sizeof(dbg), "[MSAA] %s\n", message);
#endif
    OutputDebugStringA(dbg);

    char logPath[MAX_PATH] = {};
    if (!BuildDebugLogPath(logPath, sizeof(logPath), "msaa_debug.log"))
        return;

    FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, logPath, "a") == 0 && f) {
#else
    f = fopen(logPath, "a");
    if (f) {
#endif
        std::fprintf(f, "[MSAA] %s\n", message);
        std::fclose(f);
    }
}

const char* SampleName(D3DMULTISAMPLE_TYPE type)
{
    switch (type) {
    case D3DMULTISAMPLE_NONE: return "NONE";
    case D3DMULTISAMPLE_NONMASKABLE: return "NONMASKABLE";
    case D3DMULTISAMPLE_2_SAMPLES: return "2x";
    case D3DMULTISAMPLE_3_SAMPLES: return "3x";
    case D3DMULTISAMPLE_4_SAMPLES: return "4x";
    case D3DMULTISAMPLE_5_SAMPLES: return "5x";
    case D3DMULTISAMPLE_6_SAMPLES: return "6x";
    case D3DMULTISAMPLE_7_SAMPLES: return "7x";
    case D3DMULTISAMPLE_8_SAMPLES: return "8x";
    case D3DMULTISAMPLE_9_SAMPLES: return "9x";
    case D3DMULTISAMPLE_10_SAMPLES: return "10x";
    case D3DMULTISAMPLE_11_SAMPLES: return "11x";
    case D3DMULTISAMPLE_12_SAMPLES: return "12x";
    case D3DMULTISAMPLE_13_SAMPLES: return "13x";
    case D3DMULTISAMPLE_14_SAMPLES: return "14x";
    case D3DMULTISAMPLE_15_SAMPLES: return "15x";
    case D3DMULTISAMPLE_16_SAMPLES: return "16x";
    default: return "UNKNOWN";
    }
}

D3DMULTISAMPLE_TYPE RequestedType()
{
    switch (gRequestedSamples) {
    case 2: return D3DMULTISAMPLE_2_SAMPLES;
    case 4: return D3DMULTISAMPLE_4_SAMPLES;
    case 8: return D3DMULTISAMPLE_8_SAMPLES;
    case 16: return D3DMULTISAMPLE_16_SAMPLES;
    default: return D3DMULTISAMPLE_4_SAMPLES;
    }
}

void DumpPP(const char* where, const D3DPRESENT_PARAMETERS& pp)
{
    Log("%s: %lux%lu fmt=%lu count=%lu MSAA=%s(%lu) q=%lu swap=%lu windowed=%lu depth=%lu depthFmt=%lu flags=0x%08lX interval=%lu",
        where,
        (unsigned long)pp.BackBufferWidth,
        (unsigned long)pp.BackBufferHeight,
        (unsigned long)pp.BackBufferFormat,
        (unsigned long)pp.BackBufferCount,
        SampleName(pp.MultiSampleType),
        (unsigned long)pp.MultiSampleType,
        (unsigned long)pp.MultiSampleQuality,
        (unsigned long)pp.SwapEffect,
        (unsigned long)pp.Windowed,
        (unsigned long)pp.EnableAutoDepthStencil,
        (unsigned long)pp.AutoDepthStencilFormat,
        (unsigned long)pp.Flags,
        (unsigned long)pp.PresentationInterval);
}

bool CheckType(
    IDirect3D9* d3d,
    UINT adapter,
    D3DDEVTYPE deviceType,
    D3DPRESENT_PARAMETERS* pp,
    D3DMULTISAMPLE_TYPE type)
{
    if (!d3d || !pp)
        return false;

    D3DFORMAT colorFmt = pp->BackBufferFormat;
    if (colorFmt == D3DFMT_UNKNOWN) {
        D3DDISPLAYMODE mode = {};
        if (FAILED(d3d->GetAdapterDisplayMode(adapter, &mode)))
            return false;
        colorFmt = mode.Format;
    }

    DWORD colorLevels = 0;
    HRESULT hrColor = d3d->CheckDeviceMultiSampleType(
        adapter, deviceType, colorFmt, pp->Windowed,
        type, &colorLevels);

    Log("Check %s color: adapter=%u fmt=%lu windowed=%lu hr=0x%08lX levels=%lu",
        SampleName(type), adapter, (unsigned long)colorFmt,
        (unsigned long)pp->Windowed, (unsigned long)hrColor,
        (unsigned long)colorLevels);

    if (FAILED(hrColor) || colorLevels == 0)
        return false;

    if (pp->EnableAutoDepthStencil) {
        DWORD depthLevels = 0;
        HRESULT hrDepth = d3d->CheckDeviceMultiSampleType(
            adapter, deviceType, pp->AutoDepthStencilFormat,
            pp->Windowed, type, &depthLevels);

        Log("Check %s depth: fmt=%lu hr=0x%08lX levels=%lu",
            SampleName(type),
            (unsigned long)pp->AutoDepthStencilFormat,
            (unsigned long)hrDepth,
            (unsigned long)depthLevels);

        if (FAILED(hrDepth) || depthLevels == 0)
            return false;
    }

    return true;
}

D3DMULTISAMPLE_TYPE ChooseType(
    IDirect3D9* d3d,
    UINT adapter,
    D3DDEVTYPE deviceType,
    D3DPRESENT_PARAMETERS* pp)
{
    D3DMULTISAMPLE_TYPE requested = RequestedType();
    if (CheckType(d3d, adapter, deviceType, pp, requested))
        return requested;

    if (gRequestedSamples > 4 &&
        CheckType(d3d, adapter, deviceType, pp, D3DMULTISAMPLE_4_SAMPLES))
        return D3DMULTISAMPLE_4_SAMPLES;

    if (gRequestedSamples > 2 &&
        CheckType(d3d, adapter, deviceType, pp, D3DMULTISAMPLE_2_SAMPLES))
        return D3DMULTISAMPLE_2_SAMPLES;

    return D3DMULTISAMPLE_NONE;
}

void ApplyMsaa(
    const char* reason,
    IDirect3D9* d3d,
    UINT adapter,
    D3DDEVTYPE deviceType,
    D3DPRESENT_PARAMETERS* pp)
{
    if (!gEnabled || !d3d || !pp)
        return;

    DumpPP(reason, *pp);

    D3DMULTISAMPLE_TYPE type = ChooseType(d3d, adapter, deviceType, pp);
    if (type == D3DMULTISAMPLE_NONE) {
        Log("%s: no supported requested/fallback MSAA mode; leaving NONE.", reason);
        pp->MultiSampleType = D3DMULTISAMPLE_NONE;
        pp->MultiSampleQuality = 0;
        return;
    }

    // D3D9 requires DISCARD for a multisampled primary swap chain.
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;

    // D3DPRESENTFLAG_LOCKABLE_BACKBUFFER is incompatible with MSAA.
    pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;

    pp->MultiSampleType = type;
    pp->MultiSampleQuality = 0;

    Log("%s: applying %s MSAA.", reason, SampleName(type));
    DumpPP("modified PP", *pp);
}


IDirect3D9* __cdecl PrepareCreateDevice07()
{
    IDirect3D9* d3d =
        *reinterpret_cast<IDirect3D9**>(kLive07D3D9Ptr);

    if (!gEnabled || !d3d)
        return d3d;

    D3DPRESENT_PARAMETERS* pp =
        reinterpret_cast<D3DPRESENT_PARAMETERS*>(kLive07PresentParams);

    const D3DDEVTYPE deviceType =
        static_cast<D3DDEVTYPE>(
            *reinterpret_cast<DWORD*>(kLive07DeviceType));

    ApplyMsaa(
        "Live07 CreateDevice BEFORE",
        d3d,
        D3DADAPTER_DEFAULT,
        deviceType,
        pp);

    return d3d;
}

IDirect3DDevice9* __cdecl PrepareReset07()
{
    IDirect3DDevice9* device =
        *reinterpret_cast<IDirect3DDevice9**>(kLive07DevicePtr);

    if (!gEnabled || !device)
        return device;

    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};

    HRESULT hrD3D = device->GetDirect3D(&d3d);
    HRESULT hrCP  = device->GetCreationParameters(&cp);

    if (SUCCEEDED(hrD3D) && d3d && SUCCEEDED(hrCP)) {
        D3DPRESENT_PARAMETERS* pp =
            reinterpret_cast<D3DPRESENT_PARAMETERS*>(kLive07PresentParams);

        ApplyMsaa(
            "Live07 Reset BEFORE",
            d3d,
            cp.AdapterOrdinal,
            cp.DeviceType,
            pp);
    }

    if (d3d)
        d3d->Release();

    return device;
}



IDirect3D9* __cdecl PrepareCreateDevice05()
{
    IDirect3D9* d3d =
        *reinterpret_cast<IDirect3D9**>(kLive05D3D9Ptr);

    if (!gEnabled || !d3d)
        return d3d;

    D3DPRESENT_PARAMETERS* pp =
        reinterpret_cast<D3DPRESENT_PARAMETERS*>(kLive05PresentParams);

    const D3DDEVTYPE deviceType =
        static_cast<D3DDEVTYPE>(
            *reinterpret_cast<DWORD*>(kLive05DeviceType));

    ApplyMsaa(
        "Live05 CreateDevice BEFORE",
        d3d,
        D3DADAPTER_DEFAULT,
        deviceType,
        pp);

    return d3d;
}

IDirect3DDevice9* __cdecl PrepareReset05()
{
    IDirect3DDevice9* device =
        *reinterpret_cast<IDirect3DDevice9**>(kLive05DevicePtr);

    if (!gEnabled || !device)
        return device;

    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};

    HRESULT hrD3D = device->GetDirect3D(&d3d);
    HRESULT hrCP = device->GetCreationParameters(&cp);

    if (SUCCEEDED(hrD3D) && d3d && SUCCEEDED(hrCP)) {
        D3DPRESENT_PARAMETERS* pp =
            reinterpret_cast<D3DPRESENT_PARAMETERS*>(kLive05PresentParams);

        ApplyMsaa(
            "Live05 Reset BEFORE",
            d3d,
            cp.AdapterOrdinal,
            cp.DeviceType,
            pp);
    }

    if (d3d)
        d3d->Release();

    return device;
}

IDirect3D9* __cdecl PrepareCreateDevice08()
{
    IDirect3D9* d3d =
        *reinterpret_cast<IDirect3D9**>(kLive08D3D9Ptr);

    if (!gEnabled || !d3d)
        return d3d;

    D3DPRESENT_PARAMETERS* pp =
        reinterpret_cast<D3DPRESENT_PARAMETERS*>(kLive08PresentParams);

    const D3DDEVTYPE deviceType =
        static_cast<D3DDEVTYPE>(
            *reinterpret_cast<DWORD*>(kLive08DeviceType));

    ApplyMsaa(
        "Live08 CreateDevice BEFORE",
        d3d,
        D3DADAPTER_DEFAULT,
        deviceType,
        pp);

    return d3d;
}

IDirect3DDevice9* __cdecl PrepareReset08()
{
    IDirect3DDevice9* device =
        *reinterpret_cast<IDirect3DDevice9**>(kLive08DevicePtr);

    if (!gEnabled || !device)
        return device;

    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};

    HRESULT hrD3D = device->GetDirect3D(&d3d);
    HRESULT hrCP  = device->GetCreationParameters(&cp);

    if (SUCCEEDED(hrD3D) && d3d && SUCCEEDED(hrCP)) {
        D3DPRESENT_PARAMETERS* pp =
            reinterpret_cast<D3DPRESENT_PARAMETERS*>(kLive08PresentParams);

        ApplyMsaa(
            "Live08 Reset BEFORE",
            d3d,
            cp.AdapterOrdinal,
            cp.DeviceType,
            pp);
    }

    if (d3d)
        d3d->Release();

    return device;
}

bool PatchFiveByteMovToCall(
    uintptr_t address,
    const BYTE expected[5],
    void* replacement,
    const char* label)
{
    BYTE* target = reinterpret_cast<BYTE*>(address);

    if (std::memcmp(target, expected, 5) != 0) {
        Log("%s patch FAILED at %08lX: unexpected bytes.",
            label,
            (unsigned long)address);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            target,
            5,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        Log("%s patch FAILED at %08lX: VirtualProtect error=%lu.",
            label,
            (unsigned long)address,
            (unsigned long)GetLastError());
        return false;
    }

    target[0] = 0xE8;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(replacement) -
            (address + 5));

    DWORD ignored = 0;
    VirtualProtect(target, 5, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 5);

    Log("%s patch installed at %08lX.",
        label,
        (unsigned long)address);

    return true;
}

bool PatchLive07NativeMsaaRestore()
{
    BYTE* target =
        reinterpret_cast<BYTE*>(kLive07NativeMsaaRestore);

    const BYTE expected[6] = {
        0xFF, 0x35, 0x90, 0xCF, 0xCC, 0x00
    };

    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        Log("Live07 native MSAA restore patch FAILED: unexpected bytes at %08lX.",
            (unsigned long)kLive07NativeMsaaRestore);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            target,
            sizeof(expected),
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
        return false;

    // push 1 + NOP padding
    target[0] = 0x6A;
    target[1] = 0x01;
    target[2] = 0x90;
    target[3] = 0x90;
    target[4] = 0x90;
    target[5] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, sizeof(expected), oldProtect, &ignored);
    FlushInstructionCache(
        GetCurrentProcess(),
        target,
        sizeof(expected));

    Log("Live07 native MSAA state restore patched at %08lX.",
        (unsigned long)kLive07NativeMsaaRestore);

    return true;
}

bool InstallLive07DirectMsaaHooks()
{
    const BYTE expectedCreate[5] = {
        0xA1, 0xE0, 0x96, 0xCC, 0x00
    };

    const BYTE expectedReset[5] = {
        0xA1, 0xE4, 0x96, 0xCC, 0x00
    };

    bool ok = true;

    for (size_t i = 0;
         i < sizeof(kLive07CreatePrepSites) / sizeof(kLive07CreatePrepSites[0]);
         ++i)
    {
        if (!PatchFiveByteMovToCall(
                kLive07CreatePrepSites[i],
                expectedCreate,
                reinterpret_cast<void*>(&PrepareCreateDevice07),
                "Live07 CreateDevice prep"))
            ok = false;
    }

    for (size_t i = 0;
         i < sizeof(kLive07ResetPrepSites) / sizeof(kLive07ResetPrepSites[0]);
         ++i)
    {
        if (!PatchFiveByteMovToCall(
                kLive07ResetPrepSites[i],
                expectedReset,
                reinterpret_cast<void*>(&PrepareReset07),
                "Live07 Reset prep"))
            ok = false;
    }

    if (!PatchLive07NativeMsaaRestore())
        ok = false;

    if (ok) {
        Log("NBA Live 07 direct MSAA hooks installed without touching D3D9 COM vtables.");
    }

    return ok;
}



bool PatchLive05NativeMsaaRestore()
{
    BYTE* target =
        reinterpret_cast<BYTE*>(kLive05NativeMsaaRestore);

    const BYTE expected[6] = {
        0xFF, 0x35, 0x50, 0x89, 0xC5, 0x00
    };

    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        Log("Live05 native MSAA restore patch FAILED: unexpected bytes at %08lX.",
            (unsigned long)kLive05NativeMsaaRestore);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            target,
            sizeof(expected),
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
        return false;

    target[0] = 0x6A;
    target[1] = 0x01;
    target[2] = 0x90;
    target[3] = 0x90;
    target[4] = 0x90;
    target[5] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, sizeof(expected), oldProtect, &ignored);
    FlushInstructionCache(
        GetCurrentProcess(),
        target,
        sizeof(expected));

    Log("Live05 native MSAA state restore patched at %08lX.",
        (unsigned long)kLive05NativeMsaaRestore);

    return true;
}

bool InstallLive05DirectMsaaHooks()
{
    const BYTE expectedCreate[5] = {
        0xA1, 0xD0, 0x6C, 0xC5, 0x00
    };

    const BYTE expectedReset[5] = {
        0xA1, 0xD4, 0x6C, 0xC5, 0x00
    };

    bool ok = true;

    for (size_t i = 0;
         i < sizeof(kLive05CreatePrepSites) / sizeof(kLive05CreatePrepSites[0]);
         ++i)
    {
        if (!PatchFiveByteMovToCall(
                kLive05CreatePrepSites[i],
                expectedCreate,
                reinterpret_cast<void*>(&PrepareCreateDevice05),
                "Live05 CreateDevice prep"))
            ok = false;
    }

    for (size_t i = 0;
         i < sizeof(kLive05ResetPrepSites) / sizeof(kLive05ResetPrepSites[0]);
         ++i)
    {
        if (!PatchFiveByteMovToCall(
                kLive05ResetPrepSites[i],
                expectedReset,
                reinterpret_cast<void*>(&PrepareReset05),
                "Live05 Reset prep"))
            ok = false;
    }

    if (!PatchLive05NativeMsaaRestore())
        ok = false;

    if (ok) {
        Log("NBA Live 2005 direct MSAA hooks installed without touching D3D9 COM vtables.");
    }

    return ok;
}

bool PatchLive08NativeMsaaRestore()
{
    BYTE* target =
        reinterpret_cast<BYTE*>(kLive08NativeMsaaRestore);

    const BYTE expected[6] = {
        0xFF, 0x35, 0x20, 0x89, 0xD8, 0x00
    };

    if (std::memcmp(target, expected, sizeof(expected)) != 0) {
        Log("Live08 native MSAA restore patch FAILED: unexpected bytes at %08lX.",
            (unsigned long)kLive08NativeMsaaRestore);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            target,
            sizeof(expected),
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
        return false;

    target[0] = 0x6A;
    target[1] = 0x01;
    target[2] = 0x90;
    target[3] = 0x90;
    target[4] = 0x90;
    target[5] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, sizeof(expected), oldProtect, &ignored);
    FlushInstructionCache(
        GetCurrentProcess(),
        target,
        sizeof(expected));

    Log("Live08 native MSAA state restore patched at %08lX.",
        (unsigned long)kLive08NativeMsaaRestore);

    return true;
}

bool InstallLive08DirectMsaaHooks()
{
    const BYTE expectedCreate[5] = {
        0xA1, 0x70, 0x50, 0xD8, 0x00
    };

    const BYTE expectedReset[5] = {
        0xA1, 0x74, 0x50, 0xD8, 0x00
    };

    bool ok = true;

    for (size_t i = 0;
         i < sizeof(kLive08CreatePrepSites) / sizeof(kLive08CreatePrepSites[0]);
         ++i)
    {
        if (!PatchFiveByteMovToCall(
                kLive08CreatePrepSites[i],
                expectedCreate,
                reinterpret_cast<void*>(&PrepareCreateDevice08),
                "Live08 CreateDevice prep"))
            ok = false;
    }

    for (size_t i = 0;
         i < sizeof(kLive08ResetPrepSites) / sizeof(kLive08ResetPrepSites[0]);
         ++i)
    {
        if (!PatchFiveByteMovToCall(
                kLive08ResetPrepSites[i],
                expectedReset,
                reinterpret_cast<void*>(&PrepareReset08),
                "Live08 Reset prep"))
            ok = false;
    }

    if (!PatchLive08NativeMsaaRestore())
        ok = false;

    if (ok) {
        Log("NBA Live 08 direct MSAA hooks installed without touching D3D9 COM vtables.");
    }

    return ok;
}

void ProbeDevice(const char* reason, IDirect3DDevice9* device)
{
    if (!device) {
        Log("%s: device=null", reason);
        return;
    }

    IDirect3DSwapChain9* chain = nullptr;
    HRESULT hr = device->GetSwapChain(0, &chain);
    if (SUCCEEDED(hr) && chain) {
        D3DPRESENT_PARAMETERS pp = {};
        if (SUCCEEDED(chain->GetPresentParameters(&pp)))
            DumpPP(reason, pp);
        chain->Release();
    } else {
        Log("%s: GetSwapChain failed hr=0x%08lX", reason, (unsigned long)hr);
    }

    IDirect3DSurface9* back = nullptr;
    hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    if (SUCCEEDED(hr) && back) {
        D3DSURFACE_DESC d = {};
        if (SUCCEEDED(back->GetDesc(&d))) {
            Log("%s BACKBUFFER: %ux%u fmt=%lu MSAA=%s(%lu) q=%lu",
                reason, d.Width, d.Height, (unsigned long)d.Format,
                SampleName(d.MultiSampleType),
                (unsigned long)d.MultiSampleType,
                (unsigned long)d.MultiSampleQuality);
        }
        back->Release();
    }

    DWORD state = 0;
    if (SUCCEEDED(device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &state)))
        Log("%s D3DRS_MULTISAMPLEANTIALIAS=%lu", reason, (unsigned long)state);
}


bool PatchPointer(void** slot, void* replacement, void** originalOut);

void DescribeSurface(const char* prefix, IDirect3DSurface9* surface)
{
    if (!surface) {
        Log("%s surface=null", prefix);
        return;
    }

    D3DSURFACE_DESC d = {};
    HRESULT hr = surface->GetDesc(&d);
    if (FAILED(hr)) {
        Log("%s surface=%p GetDesc failed hr=0x%08lX",
            prefix, surface, (unsigned long)hr);
        return;
    }

    Log("%s surface=%p %ux%u fmt=%lu usage=0x%08lX pool=%lu MSAA=%s(%lu) q=%lu",
        prefix,
        surface,
        d.Width,
        d.Height,
        (unsigned long)d.Format,
        (unsigned long)d.Usage,
        (unsigned long)d.Pool,
        SampleName(d.MultiSampleType),
        (unsigned long)d.MultiSampleType,
        (unsigned long)d.MultiSampleQuality);
}

bool IsMainSizedSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface)
{
    if (!device || !surface)
        return false;

    D3DSURFACE_DESC s = {};
    if (FAILED(surface->GetDesc(&s)))
        return false;

    IDirect3DSurface9* back = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back)
        return false;

    D3DSURFACE_DESC b = {};
    bool match = SUCCEEDED(back->GetDesc(&b)) &&
        s.Width == b.Width && s.Height == b.Height;
    back->Release();
    return match;
}

HRESULT STDMETHODCALLTYPE HookCreateTexture(
    IDirect3DDevice9* self,
    UINT width,
    UINT height,
    UINT levels,
    DWORD usage,
    D3DFORMAT format,
    D3DPOOL pool,
    IDirect3DTexture9** outTexture,
    HANDLE* sharedHandle)
{
    HRESULT hr = gOriginalCreateTexture(
        self, width, height, levels, usage, format, pool, outTexture, sharedHandle);

    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0) {
        Log("CreateTexture: %ux%u levels=%u usage=0x%08lX fmt=%lu pool=%lu hr=0x%08lX tex=%p",
            width, height, levels, (unsigned long)usage, (unsigned long)format,
            (unsigned long)pool, (unsigned long)hr,
            (SUCCEEDED(hr) && outTexture) ? *outTexture : nullptr);

        if (SUCCEEDED(hr) && outTexture && *outTexture) {
            IDirect3DSurface9* level0 = nullptr;
            if (SUCCEEDED((*outTexture)->GetSurfaceLevel(0, &level0)) && level0) {
                DescribeSurface("CreateTexture level0", level0);
                level0->Release();
            }
        }
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateRenderTarget(
    IDirect3DDevice9* self,
    UINT width,
    UINT height,
    D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample,
    DWORD quality,
    BOOL lockable,
    IDirect3DSurface9** outSurface,
    HANDLE* sharedHandle)
{
    Log("CreateRenderTarget REQUEST: %ux%u fmt=%lu MSAA=%s(%lu) q=%lu lockable=%lu",
        width, height, (unsigned long)format, SampleName(multisample),
        (unsigned long)multisample, (unsigned long)quality, (unsigned long)lockable);

    HRESULT hr = gOriginalCreateRenderTarget(
        self, width, height, format, multisample, quality,
        lockable, outSurface, sharedHandle);

    Log("CreateRenderTarget RESULT: hr=0x%08lX surface=%p",
        (unsigned long)hr,
        (SUCCEEDED(hr) && outSurface) ? *outSurface : nullptr);

    if (SUCCEEDED(hr) && outSurface && *outSurface)
        DescribeSurface("CreateRenderTarget ACTUAL", *outSurface);

    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateDepthStencilSurface(
    IDirect3DDevice9* self,
    UINT width,
    UINT height,
    D3DFORMAT format,
    D3DMULTISAMPLE_TYPE multisample,
    DWORD quality,
    BOOL discard,
    IDirect3DSurface9** outSurface,
    HANDLE* sharedHandle)
{
    Log("CreateDepthStencilSurface REQUEST: %ux%u fmt=%lu MSAA=%s(%lu) q=%lu discard=%lu",
        width, height, (unsigned long)format, SampleName(multisample),
        (unsigned long)multisample, (unsigned long)quality, (unsigned long)discard);

    HRESULT hr = gOriginalCreateDepthStencilSurface(
        self, width, height, format, multisample, quality,
        discard, outSurface, sharedHandle);

    Log("CreateDepthStencilSurface RESULT: hr=0x%08lX surface=%p",
        (unsigned long)hr,
        (SUCCEEDED(hr) && outSurface) ? *outSurface : nullptr);

    if (SUCCEEDED(hr) && outSurface && *outSurface)
        DescribeSurface("CreateDepthStencilSurface ACTUAL", *outSurface);

    return hr;
}

HRESULT STDMETHODCALLTYPE HookSetRenderTarget(
    IDirect3DDevice9* self,
    DWORD index,
    IDirect3DSurface9* surface)
{
    if (index < 4 && gLastRT[index] != surface) {
        gLastRT[index] = surface;
        char label[64] = {};
#if defined(_MSC_VER)
        sprintf_s(label, "SetRenderTarget[%lu]", (unsigned long)index);
#else
        snprintf(label, sizeof(label), "SetRenderTarget[%lu]", (unsigned long)index);
#endif
        DescribeSurface(label, surface);

        if (index == 0 && surface) {
            Log("SetRenderTarget[0] main-sized=%s",
                IsMainSizedSurface(self, surface) ? "yes" : "no");
        }
    }

    return gOriginalSetRenderTarget(self, index, surface);
}

HRESULT STDMETHODCALLTYPE HookSetDepthStencilSurface(
    IDirect3DDevice9* self,
    IDirect3DSurface9* surface)
{
    if (gLastDepth != surface) {
        gLastDepth = surface;
        DescribeSurface("SetDepthStencilSurface", surface);
        if (surface) {
            Log("SetDepthStencilSurface main-sized=%s",
                IsMainSizedSurface(self, surface) ? "yes" : "no");
        }
    }

    return gOriginalSetDepthStencilSurface(self, surface);
}

HRESULT STDMETHODCALLTYPE HookStretchRect(
    IDirect3DDevice9* self,
    IDirect3DSurface9* src,
    const RECT* srcRect,
    IDirect3DSurface9* dst,
    const RECT* dstRect,
    D3DTEXTUREFILTERTYPE filter)
{
    if (gStretchRectLogCount < kStretchRectLogLimit) {
        bool srcMain = IsMainSizedSurface(self, src);
        bool dstMain = IsMainSizedSurface(self, dst);

        // Prefer transitions involving a full-size render surface; those are
        // the most interesting candidates for the final gameplay composite.
        if (srcMain || dstMain) {
            ++gStretchRectLogCount;
            Log("StretchRect #%u filter=%lu srcMain=%s dstMain=%s",
                gStretchRectLogCount,
                (unsigned long)filter,
                srcMain ? "yes" : "no",
                dstMain ? "yes" : "no");
            DescribeSurface("  StretchRect SRC", src);
            DescribeSurface("  StretchRect DST", dst);
        }
    }

    return gOriginalStretchRect(
        self, src, srcRect, dst, dstRect, filter);
}


HRESULT STDMETHODCALLTYPE HookSetRenderState(
    IDirect3DDevice9* self,
    D3DRENDERSTATETYPE state,
    DWORD value)
{
    if (state == D3DRS_MULTISAMPLEANTIALIAS) {
        ++gMsaaStateWriteCount;

        Log("SetRenderState MSAA write #%u: requested=%lu (%s)",
            gMsaaStateWriteCount,
            (unsigned long)value,
            value ? "ON" : "OFF");

        if (gEnabled && value == FALSE) {
            Log("SetRenderState MSAA write #%u: overriding OFF -> ON",
                gMsaaStateWriteCount);
            value = TRUE;
        }
    }
    else if (state == D3DRS_ANTIALIASEDLINEENABLE) {
        Log("SetRenderState ANTIALIASEDLINEENABLE requested=%lu",
            (unsigned long)value);
    }

    return gOriginalSetRenderState(self, state, value);
}


void HookStateBlockApply(IDirect3DStateBlock9* block);

HRESULT STDMETHODCALLTYPE HookStateBlockApplyImpl(
    IDirect3DStateBlock9* self)
{
    ++gStateBlockApplyCount;

    IDirect3DDevice9* device = nullptr;
    HRESULT getDeviceHr = self->GetDevice(&device);

    DWORD before = 0xFFFFFFFF;
    if (SUCCEEDED(getDeviceHr) && device)
        device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &before);

    Log("StateBlock::Apply #%u block=%p BEFORE MSAA=%lu",
        gStateBlockApplyCount, self, (unsigned long)before);

    HRESULT hr = gOriginalStateBlockApply
        ? gOriginalStateBlockApply(self)
        : D3DERR_INVALIDCALL;

    DWORD after = 0xFFFFFFFF;
    if (SUCCEEDED(getDeviceHr) && device) {
        device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &after);

        Log("StateBlock::Apply #%u returned hr=0x%08lX AFTER MSAA=%lu",
            gStateBlockApplyCount,
            (unsigned long)hr,
            (unsigned long)after);

        if (gEnabled && SUCCEEDED(hr) && after == FALSE) {
            Log("StateBlock::Apply #%u disabled MSAA; forcing it back ON.",
                gStateBlockApplyCount);

            // This routes through HookSetRenderState, which also records the write.
            device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);

            DWORD repaired = 0xFFFFFFFF;
            device->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &repaired);
            Log("StateBlock::Apply #%u repaired MSAA=%lu",
                gStateBlockApplyCount, (unsigned long)repaired);
        }

        device->Release();
    } else {
        Log("StateBlock::Apply #%u GetDevice failed hr=0x%08lX",
            gStateBlockApplyCount, (unsigned long)getDeviceHr);
    }

    return hr;
}

void HookStateBlockApply(IDirect3DStateBlock9* block)
{
    if (!block)
        return;

    void** vtbl = *reinterpret_cast<void***>(block);
    // IDirect3DStateBlock9::Apply is vtable index 5.
    void** slot = &vtbl[5];

    if (*slot == reinterpret_cast<void*>(&HookStateBlockApplyImpl))
        return;

    void* original = nullptr;
    if (PatchPointer(
            slot,
            reinterpret_cast<void*>(&HookStateBlockApplyImpl),
            &original))
    {
        if (!gOriginalStateBlockApply)
            gOriginalStateBlockApply =
                reinterpret_cast<StateBlockApplyFn>(original);

        Log("Hooked IDirect3DStateBlock9::Apply vtable=%p original=%p",
            vtbl, original);
    } else {
        Log("FAILED to hook IDirect3DStateBlock9::Apply vtable=%p", vtbl);
    }
}

HRESULT STDMETHODCALLTYPE HookCreateStateBlock(
    IDirect3DDevice9* self,
    D3DSTATEBLOCKTYPE type,
    IDirect3DStateBlock9** outBlock)
{
    ++gStateBlockCreateCount;
    Log("CreateStateBlock #%u type=%lu",
        gStateBlockCreateCount, (unsigned long)type);

    HRESULT hr = gOriginalCreateStateBlock
        ? gOriginalCreateStateBlock(self, type, outBlock)
        : D3DERR_INVALIDCALL;

    IDirect3DStateBlock9* block =
        (SUCCEEDED(hr) && outBlock) ? *outBlock : nullptr;

    Log("CreateStateBlock #%u returned hr=0x%08lX block=%p",
        gStateBlockCreateCount, (unsigned long)hr, block);

    if (block)
        HookStateBlockApply(block);

    return hr;
}

HRESULT STDMETHODCALLTYPE HookBeginStateBlock(
    IDirect3DDevice9* self)
{
    ++gStateBlockBeginCount;

    DWORD msaa = 0xFFFFFFFF;
    self->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &msaa);

    Log("BeginStateBlock #%u current MSAA=%lu",
        gStateBlockBeginCount, (unsigned long)msaa);

    return gOriginalBeginStateBlock
        ? gOriginalBeginStateBlock(self)
        : D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE HookEndStateBlock(
    IDirect3DDevice9* self,
    IDirect3DStateBlock9** outBlock)
{
    ++gStateBlockEndCount;

    HRESULT hr = gOriginalEndStateBlock
        ? gOriginalEndStateBlock(self, outBlock)
        : D3DERR_INVALIDCALL;

    IDirect3DStateBlock9* block =
        (SUCCEEDED(hr) && outBlock) ? *outBlock : nullptr;

    Log("EndStateBlock #%u returned hr=0x%08lX block=%p",
        gStateBlockEndCount, (unsigned long)hr, block);

    if (block)
        HookStateBlockApply(block);

    return hr;
}

void HookRenderTargetMethods(IDirect3DDevice9* device)
{
    if (!device)
        return;

    void** vtbl = *reinterpret_cast<void***>(device);

    struct HookSpec {
        unsigned int index;
        void* replacement;
        void** originalStorage;
        const char* name;
    };

    HookSpec specs[] = {
        { 23, reinterpret_cast<void*>(&HookCreateTexture),
          reinterpret_cast<void**>(&gOriginalCreateTexture), "CreateTexture" },
        { 28, reinterpret_cast<void*>(&HookCreateRenderTarget),
          reinterpret_cast<void**>(&gOriginalCreateRenderTarget), "CreateRenderTarget" },
        { 29, reinterpret_cast<void*>(&HookCreateDepthStencilSurface),
          reinterpret_cast<void**>(&gOriginalCreateDepthStencilSurface), "CreateDepthStencilSurface" },
        { 34, reinterpret_cast<void*>(&HookStretchRect),
          reinterpret_cast<void**>(&gOriginalStretchRect), "StretchRect" },
        { 37, reinterpret_cast<void*>(&HookSetRenderTarget),
          reinterpret_cast<void**>(&gOriginalSetRenderTarget), "SetRenderTarget" },
        { 39, reinterpret_cast<void*>(&HookSetDepthStencilSurface),
          reinterpret_cast<void**>(&gOriginalSetDepthStencilSurface), "SetDepthStencilSurface" },
        { 57, reinterpret_cast<void*>(&HookSetRenderState),
          reinterpret_cast<void**>(&gOriginalSetRenderState), "SetRenderState" },
        { 59, reinterpret_cast<void*>(&HookCreateStateBlock),
          reinterpret_cast<void**>(&gOriginalCreateStateBlock), "CreateStateBlock" },
        { 60, reinterpret_cast<void*>(&HookBeginStateBlock),
          reinterpret_cast<void**>(&gOriginalBeginStateBlock), "BeginStateBlock" },
        { 61, reinterpret_cast<void*>(&HookEndStateBlock),
          reinterpret_cast<void**>(&gOriginalEndStateBlock), "EndStateBlock" },
    };

    for (const HookSpec& spec : specs) {
        void** slot = &vtbl[spec.index];

        if (*slot == spec.replacement)
            continue;

        void* original = nullptr;
        if (PatchPointer(slot, spec.replacement, &original)) {
            if (*spec.originalStorage == nullptr)
                *spec.originalStorage = original;
            Log("Hooked IDirect3DDevice9::%s index=%u original=%p",
                spec.name, spec.index, original);
        } else {
            Log("FAILED to hook IDirect3DDevice9::%s index=%u",
                spec.name, spec.index);
        }
    }
}

bool PatchPointer(void** slot, void* replacement, void** originalOut)
{
    if (!slot || !replacement)
        return false;

    if (*slot == replacement)
        return true;

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    if (originalOut && !*originalOut)
        *originalOut = *slot;

    *slot = replacement;

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

HRESULT STDMETHODCALLTYPE HookReset(
    IDirect3DDevice9* self,
    D3DPRESENT_PARAMETERS* pp);

HRESULT STDMETHODCALLTYPE HookCreateDevice(
    IDirect3D9* self,
    UINT adapter,
    D3DDEVTYPE deviceType,
    HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* pp,
    IDirect3DDevice9** outDevice);

void HookDeviceReset(IDirect3DDevice9* device)
{
    if (!device)
        return;

    void** vtbl = *reinterpret_cast<void***>(device);
    // IDirect3DDevice9::Reset is vtable index 16.
    void** slot = &vtbl[16];

    if (*slot == reinterpret_cast<void*>(&HookReset))
        return;

    void* original = nullptr;
    if (PatchPointer(slot, reinterpret_cast<void*>(&HookReset), &original)) {
        if (!gOriginalReset)
            gOriginalReset = reinterpret_cast<ResetFn>(original);
        Log("Hooked IDirect3DDevice9::Reset vtable=%p original=%p", vtbl, original);
    } else {
        Log("FAILED to hook IDirect3DDevice9::Reset vtable=%p", vtbl);
    }
}

void HookD3D9CreateDevice(IDirect3D9* d3d)
{
    if (!d3d)
        return;

    void** vtbl = *reinterpret_cast<void***>(d3d);
    // IDirect3D9::CreateDevice is vtable index 16.
    void** slot = &vtbl[16];

    if (*slot == reinterpret_cast<void*>(&HookCreateDevice))
        return;

    void* original = nullptr;
    if (PatchPointer(slot, reinterpret_cast<void*>(&HookCreateDevice), &original)) {
        if (!gOriginalCreateDevice)
            gOriginalCreateDevice = reinterpret_cast<CreateDeviceFn>(original);
        Log("Hooked IDirect3D9::CreateDevice vtable=%p original=%p", vtbl, original);
    } else {
        Log("FAILED to hook IDirect3D9::CreateDevice vtable=%p", vtbl);
    }
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion)
{
    Log("Direct3DCreate9(%u) intercepted.", sdkVersion);

    IDirect3D9* d3d = gOriginalDirect3DCreate9
        ? gOriginalDirect3DCreate9(sdkVersion)
        : nullptr;

    Log("Direct3DCreate9 returned %p", d3d);
    HookD3D9CreateDevice(d3d);
    return d3d;
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(
    IDirect3D9* self,
    UINT adapter,
    D3DDEVTYPE deviceType,
    HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* pp,
    IDirect3DDevice9** outDevice)
{
    ++gCreateDeviceCount;
    Log("CreateDevice #%u intercepted: d3d=%p adapter=%u type=%lu behavior=0x%08lX out=%p",
        gCreateDeviceCount, self, adapter, (unsigned long)deviceType,
        (unsigned long)behaviorFlags, outDevice);

    ApplyMsaa("CreateDevice BEFORE", self, adapter, deviceType, pp);

    if (!gOriginalCreateDevice) {
        Log("CreateDevice #%u FAILED: original function pointer is null.", gCreateDeviceCount);
        return D3DERR_INVALIDCALL;
    }

    HRESULT hr = gOriginalCreateDevice(
        self, adapter, deviceType, focusWindow,
        behaviorFlags, pp, outDevice);

    IDirect3DDevice9* device =
        (SUCCEEDED(hr) && outDevice) ? *outDevice : nullptr;

    Log("CreateDevice #%u returned hr=0x%08lX device=%p",
        gCreateDeviceCount, (unsigned long)hr, device);

    if (SUCCEEDED(hr) && device) {
        HookDeviceReset(device);
        HookRenderTargetMethods(device);
        device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
        ProbeDevice("CreateDevice ACTUAL", device);
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE HookReset(
    IDirect3DDevice9* self,
    D3DPRESENT_PARAMETERS* pp)
{
    ++gResetCount;
    Log("Reset #%u intercepted: device=%p", gResetCount, self);

    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};

    HRESULT hrD3D = self->GetDirect3D(&d3d);
    HRESULT hrCP = self->GetCreationParameters(&cp);

    Log("Reset #%u context: GetDirect3D=0x%08lX d3d=%p GetCreationParameters=0x%08lX adapter=%u type=%lu",
        gResetCount,
        (unsigned long)hrD3D, d3d,
        (unsigned long)hrCP,
        cp.AdapterOrdinal,
        (unsigned long)cp.DeviceType);

    if (SUCCEEDED(hrD3D) && d3d && SUCCEEDED(hrCP))
        ApplyMsaa("Reset BEFORE", d3d, cp.AdapterOrdinal, cp.DeviceType, pp);

    if (d3d)
        d3d->Release();

    if (!gOriginalReset) {
        Log("Reset #%u FAILED: original function pointer is null.", gResetCount);
        return D3DERR_INVALIDCALL;
    }

    HRESULT hr = gOriginalReset(self, pp);
    Log("Reset #%u returned hr=0x%08lX", gResetCount, (unsigned long)hr);

    if (SUCCEEDED(hr)) {
        self->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
        ProbeDevice("Reset ACTUAL", self);
    }

    return hr;
}

bool HookImport(
    const char* dllName,
    const char* functionName,
    void* replacement,
    void** originalOut)
{
    if (!dllName || !functionName || !replacement)
        return false;

    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) {
        Log("IAT hook: GetModuleHandle(NULL) failed.");
        return false;
    }

    HMODULE importedModule = GetModuleHandleA(dllName);
    if (!importedModule)
        importedModule = LoadLibraryA(dllName);

    if (!importedModule) {
        Log("IAT hook: failed to load/find %s (error=%lu).",
            dllName, (unsigned long)GetLastError());
        return false;
    }

    FARPROC targetProc = GetProcAddress(importedModule, functionName);
    if (!targetProc) {
        Log("IAT hook: GetProcAddress(%s!%s) failed.",
            dllName, functionName);
        return false;
    }

    Log("IAT hook: resolved %s!%s = %p",
        dllName, functionName, targetProc);

    BYTE* base = reinterpret_cast<BYTE*>(exe);

    __try {
        IMAGE_DOS_HEADER* dos =
            reinterpret_cast<IMAGE_DOS_HEADER*>(base);

        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            Log("IAT hook: invalid DOS header.");
            return false;
        }

        IMAGE_NT_HEADERS* nt =
            reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            Log("IAT hook: invalid NT header.");
            return false;
        }

        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

        if (!dir.VirtualAddress || !dir.Size) {
            Log("IAT hook: executable has no import directory.");
            return false;
        }

        IMAGE_IMPORT_DESCRIPTOR* desc =
            reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                base + dir.VirtualAddress);

        const BYTE* importDirEnd =
            base + dir.VirtualAddress + dir.Size;

        for (;
             reinterpret_cast<BYTE*>(desc) + sizeof(*desc) <= importDirEnd &&
             desc->Name;
             ++desc)
        {
            const char* importedDll =
                reinterpret_cast<const char*>(base + desc->Name);

            if (!importedDll)
                continue;

            if (_stricmp(importedDll, dllName) != 0)
                continue;

            Log("IAT hook: found import descriptor for %s.", importedDll);

            if (!desc->FirstThunk) {
                Log("IAT hook: %s descriptor has no FirstThunk.", importedDll);
                return false;
            }

            IMAGE_THUNK_DATA* thunk =
                reinterpret_cast<IMAGE_THUNK_DATA*>(
                    base + desc->FirstThunk);

            // We deliberately do NOT walk OriginalFirstThunk or
            // IMAGE_IMPORT_BY_NAME here. Some older/bound executables have
            // layouts that make that unsafe after loader processing.
            //
            // Instead, FirstThunk already contains the resolved function
            // addresses. Find the slot whose value equals the real
            // d3d9!Direct3DCreate9 address.
            for (size_t i = 0; i < 4096; ++i, ++thunk) {
                if (thunk->u1.Function == 0)
                    break;

                void** slot =
                    reinterpret_cast<void**>(&thunk->u1.Function);

                void* current = *slot;

                if (current != reinterpret_cast<void*>(targetProc))
                    continue;

                Log("IAT hook: matched %s!%s at slot=%p current=%p",
                    dllName, functionName, slot, current);

                if (!PatchPointer(
                        slot,
                        replacement,
                        originalOut))
                {
                    Log("IAT hook: PatchPointer failed for slot=%p.", slot);
                    return false;
                }

                Log("IAT hook %s!%s slot=%p original=%p replacement=%p",
                    dllName,
                    functionName,
                    slot,
                    current,
                    replacement);

                return true;
            }

            Log("IAT hook: descriptor found but resolved function pointer was not present.");
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("IAT hook: exception while parsing import table.");
        return false;
    }

    Log("IAT hook: no import descriptor found for %s.", dllName);
    return false;
}


uintptr_t FindPatternInExecutable(
    const BYTE* pattern,
    const char* mask,
    size_t length)
{
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe || !pattern || !mask || length == 0)
        return 0;

    BYTE* base = reinterpret_cast<BYTE*>(exe);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    IMAGE_NT_HEADERS* nt =
        reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);

    for (unsigned int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        if ((section[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;

        BYTE* begin = base + section[s].VirtualAddress;
        size_t size = section[s].Misc.VirtualSize;

        if (size < length)
            continue;

        for (size_t i = 0; i <= size - length; ++i) {
            bool match = true;

            for (size_t j = 0; j < length; ++j) {
                if (mask[j] == 'x' && begin[i + j] != pattern[j]) {
                    match = false;
                    break;
                }
            }

            if (match)
                return reinterpret_cast<uintptr_t>(begin + i);
        }
    }

    return 0;
}

bool PatchNativeMsaaRestore()
{
    const BYTE pattern[] = {
        0xFF, 0x35, 0,0,0,0,
        0xA1, 0,0,0,0,
        0x8B, 0x08,
        0x68, 0xA1, 0x00, 0x00, 0x00,
        0x50,
        0xFF, 0x91, 0xE4, 0x00, 0x00, 0x00
    };
    const char mask[] = "xx????x????xxxxxxxxxxxxxx";

    uintptr_t site = FindPatternInExecutable(
        pattern,
        mask,
        sizeof(pattern));

    if (!site) {
        Log("Native MSAA restore patch: matching renderer sequence not found.");
        return false;
    }

    BYTE* target = reinterpret_cast<BYTE*>(site);

    if (target[0] != 0xFF || target[1] != 0x35) {
        Log("Native MSAA restore patch: signature validation failed at %08lX.",
            (unsigned long)site);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            target,
            6,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        Log("Native MSAA restore patch: VirtualProtect failed error=%lu.",
            (unsigned long)GetLastError());
        return false;
    }

    target[0] = 0x6A;
    target[1] = 0x01;
    target[2] = 0x90;
    target[3] = 0x90;
    target[4] = 0x90;
    target[5] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(target, 6, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 6);

    Log("Patched native MSAA state restore at %08lX: cached value -> forced ON.",
        (unsigned long)site);

    return true;
}


#pragma pack(push, 1)
struct TgaHeader
{
    BYTE idLength;
    BYTE colorMapType;
    BYTE imageType;
    WORD colorMapFirst;
    WORD colorMapLength;
    BYTE colorMapDepth;
    WORD xOrigin;
    WORD yOrigin;
    WORD width;
    WORD height;
    BYTE pixelDepth;
    BYTE imageDescriptor;
};
#pragma pack(pop)

bool WriteTga24(
    const char* filename,
    UINT width,
    UINT height,
    const D3DLOCKED_RECT& locked,
    D3DFORMAT format)
{
    if (!filename || width == 0 || height == 0)
        return false;

    if (width > 65535 || height > 65535)
        return false;

    if (format != D3DFMT_A8R8G8B8 &&
        format != D3DFMT_X8R8G8B8)
        return false;

    FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, filename, "wb") != 0 || !f)
        return false;
#else
    f = fopen(filename, "wb");
    if (!f)
        return false;
#endif

    TgaHeader h = {};
    h.imageType = 2; // uncompressed true-color
    h.width = static_cast<WORD>(width);
    h.height = static_cast<WORD>(height);
    h.pixelDepth = 24;

    // Bit 5 = top-left origin.
    // Deliberately advertise ZERO alpha bits.
    h.imageDescriptor = 0x20;

    bool ok = std::fwrite(&h, sizeof(h), 1, f) == 1;

    // D3DFMT_A8R8G8B8 / X8R8G8B8 in memory is B,G,R,A/X.
    // The alpha byte is NOT a reliable final-frame opacity value in this game.
    // Write only B,G,R so screenshots are always opaque.
    BYTE* outputRow = nullptr;
    if (ok) {
        outputRow = new (std::nothrow) BYTE[static_cast<size_t>(width) * 3];
        if (!outputRow)
            ok = false;
    }

    if (ok) {
        const BYTE* srcBase = static_cast<const BYTE*>(locked.pBits);

        for (UINT y = 0; y < height && ok; ++y) {
            const BYTE* src = srcBase + static_cast<size_t>(y) * locked.Pitch;

            for (UINT x = 0; x < width; ++x) {
                outputRow[x * 3 + 0] = src[x * 4 + 0]; // B
                outputRow[x * 3 + 1] = src[x * 4 + 1]; // G
                outputRow[x * 3 + 2] = src[x * 4 + 2]; // R
            }

            ok = std::fwrite(
                outputRow,
                static_cast<size_t>(width) * 3,
                1,
                f) == 1;
        }
    }

    delete[] outputRow;
    std::fclose(f);
    return ok;
}

enum class ScreenshotFormat
{
    PNG,
    TGA
};

bool DirectoryExists(const char* path)
{
    if (!path || !*path)
        return false;

    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectoryTree(const std::string& path)
{
    if (path.empty())
        return false;

    if (DirectoryExists(path.c_str()))
        return true;

    std::string current;
    current.reserve(path.size());

    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        current.push_back(c);

        if (c != '\\' && c != '/')
            continue;

        // Skip "C:\" root and UNC prefix pieces.
        if (current.size() <= 3)
            continue;

        while (!current.empty() &&
               (current.back() == '\\' || current.back() == '/'))
            current.pop_back();

        if (!current.empty() &&
            !DirectoryExists(current.c_str()) &&
            !CreateDirectoryA(current.c_str(), nullptr))
        {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS)
                return false;
        }

        current.push_back('\\');
    }

    if (!DirectoryExists(path.c_str()) &&
        !CreateDirectoryA(path.c_str(), nullptr))
    {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
            return false;
    }

    return DirectoryExists(path.c_str());
}

const char* CurrentGameDocumentsFolder()
{
    switch (gGame) {
    case LiveGame::Live05: return "NBA LIVE 2005";
    case LiveGame::Live06: return "NBA LIVE 06";
    case LiveGame::Live07: return "NBA LIVE 07";
    case LiveGame::Live08: return "NBA LIVE 08";
    default: return "NBA LIVE";
    }
}

bool GetDefaultScreenshotDirectory(std::string& outDir)
{
    char documents[MAX_PATH] = {};

    // CSIDL_PERSONAL keeps this compatible with the older SDK/toolset used
    // by the launcher while resolving the user's Documents folder.
    HRESULT hr = SHGetFolderPathA(
        nullptr,
        CSIDL_PERSONAL | CSIDL_FLAG_CREATE,
        nullptr,
        SHGFP_TYPE_CURRENT,
        documents);

    if (FAILED(hr) || documents[0] == '\0')
        return false;

    outDir = documents;
    outDir += "\\";
    outDir += CurrentGameDocumentsFolder();
    outDir += "\\Screenshots";
    return true;
}

std::string ExpandPathVariables(const char* value)
{
    if (!value || !*value)
        return std::string();

    DWORD required = ExpandEnvironmentStringsA(value, nullptr, 0);
    if (required == 0)
        return std::string(value);

    std::vector<char> buffer(required + 1, '\0');
    if (ExpandEnvironmentStringsA(
            value,
            buffer.data(),
            static_cast<DWORD>(buffer.size())) == 0)
        return std::string(value);

    return std::string(buffer.data());
}

bool GetScreenshotDirectory(std::string& outDir)
{
    char configured[1024] = {};
    GetPrivateProfileStringA(
        "SCREENSHOT",
        "DIRECTORY",
        "",
        configured,
        static_cast<DWORD>(sizeof(configured)),
        ".\\main.ini");

    if (configured[0] != '\0') {
        outDir = ExpandPathVariables(configured);

        // If a relative custom path is supplied, resolve it under Documents.
        if (PathIsRelativeA(outDir.c_str())) {
            char documents[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathA(
                    nullptr,
                    CSIDL_PERSONAL | CSIDL_FLAG_CREATE,
                    nullptr,
                    SHGFP_TYPE_CURRENT,
                    documents)))
            {
                outDir = std::string(documents) + "\\" + outDir;
            }
        }
    }
    else {
        if (!GetDefaultScreenshotDirectory(outDir))
            return false;
    }

    while (!outDir.empty() &&
           (outDir.back() == '\\' || outDir.back() == '/'))
        outDir.pop_back();

    if (outDir.empty())
        return false;

    if (!EnsureDirectoryTree(outDir)) {
        Log("Screenshot: failed to create directory: %s", outDir.c_str());
        return false;
    }

    return true;
}

ScreenshotFormat GetScreenshotFormat()
{
    char format[32] = {};
    GetPrivateProfileStringA(
        "SCREENSHOT",
        "FORMAT",
        "png",
        format,
        static_cast<DWORD>(sizeof(format)),
        ".\\main.ini");

    if (_stricmp(format, "tga") == 0)
        return ScreenshotFormat::TGA;

    if (_stricmp(format, "png") != 0)
        Log("Screenshot: unsupported FORMAT=%s; falling back to png.", format);

    return ScreenshotFormat::PNG;
}

const char* ScreenshotExtension(ScreenshotFormat format)
{
    return format == ScreenshotFormat::TGA ? "tga" : "png";
}

bool FindScreenshotFilename(
    char* outName,
    size_t outSize,
    ScreenshotFormat format)
{
    if (!outName || outSize == 0)
        return false;

    std::string directory;
    if (!GetScreenshotDirectory(directory))
        return false;

    const char* extension = ScreenshotExtension(format);

    for (unsigned int i = 0; i <= 9999; ++i) {
#if defined(_MSC_VER)
        sprintf_s(
            outName,
            outSize,
            "%s\\screenshot%04u.%s",
            directory.c_str(),
            i,
            extension);
#else
        snprintf(
            outName,
            outSize,
            "%s\\screenshot%04u.%s",
            directory.c_str(),
            i,
            extension);
#endif

        DWORD attrs = GetFileAttributesA(outName);
        if (attrs == INVALID_FILE_ATTRIBUTES)
            return true;
    }

    return false;
}

bool WritePng24(
    const char* filename,
    UINT width,
    UINT height,
    const D3DLOCKED_RECT& locked,
    D3DFORMAT format)
{
    if (!filename || width == 0 || height == 0)
        return false;

    if (format != D3DFMT_A8R8G8B8 &&
        format != D3DFMT_X8R8G8B8)
        return false;

    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize =
        SUCCEEDED(initHr) && initHr != S_FALSE;

    // RPC_E_CHANGED_MODE means COM is already initialized on this thread in a
    // different apartment. WIC can still be used through the existing COM setup.
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE) {
        Log("Screenshot PNG: CoInitializeEx failed hr=0x%08lX",
            (unsigned long)initHr);
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;

    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory,
        reinterpret_cast<void**>(&factory));

    if (SUCCEEDED(hr))
        hr = factory->CreateStream(&stream);

    wchar_t widePath[MAX_PATH * 4] = {};
    if (SUCCEEDED(hr)) {
        int converted = MultiByteToWideChar(
            CP_ACP, 0, filename, -1,
            widePath, static_cast<int>(_countof(widePath)));

        if (converted <= 0)
            hr = E_FAIL;
    }

    if (SUCCEEDED(hr))
        hr = stream->InitializeFromFilename(widePath, GENERIC_WRITE);

    if (SUCCEEDED(hr))
        hr = factory->CreateEncoder(
            GUID_ContainerFormatPng,
            nullptr,
            &encoder);

    if (SUCCEEDED(hr))
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);

    if (SUCCEEDED(hr))
        hr = encoder->CreateNewFrame(&frame, &props);

    if (SUCCEEDED(hr))
        hr = frame->Initialize(props);

    if (SUCCEEDED(hr))
        hr = frame->SetSize(width, height);

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr))
        hr = frame->SetPixelFormat(&pixelFormat);

    if (SUCCEEDED(hr) &&
        pixelFormat != GUID_WICPixelFormat24bppBGR)
        hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;

    std::vector<BYTE> pixels;
    if (SUCCEEDED(hr)) {
        const UINT stride = width * 3;
        pixels.resize(static_cast<size_t>(stride) * height);

        const BYTE* srcBase =
            static_cast<const BYTE*>(locked.pBits);

        for (UINT y = 0; y < height; ++y) {
            const BYTE* src =
                srcBase + static_cast<size_t>(y) * locked.Pitch;
            BYTE* dst =
                pixels.data() + static_cast<size_t>(y) * stride;

            for (UINT x = 0; x < width; ++x) {
                // Ignore the game's backbuffer alpha/X byte.
                dst[x * 3 + 0] = src[x * 4 + 0]; // B
                dst[x * 3 + 1] = src[x * 4 + 1]; // G
                dst[x * 3 + 2] = src[x * 4 + 2]; // R
            }
        }

        hr = frame->WritePixels(
            height,
            stride,
            static_cast<UINT>(pixels.size()),
            pixels.data());
    }

    if (SUCCEEDED(hr))
        hr = frame->Commit();

    if (SUCCEEDED(hr))
        hr = encoder->Commit();

    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();

    if (shouldUninitialize)
        CoUninitialize();

    if (FAILED(hr)) {
        Log("Screenshot PNG: WIC encode failed hr=0x%08lX",
            (unsigned long)hr);
        return false;
    }

    return true;
}

bool CaptureMsaaScreenshot()
{
    uintptr_t devicePtrAddress = 0;

    switch (gGame) {
    case LiveGame::Live05:
        devicePtrAddress = kLive05DevicePtr;
        break;
    case LiveGame::Live06:
        devicePtrAddress = 0x00CBFC3C;
        break;
    case LiveGame::Live07:
        devicePtrAddress = kLive07DevicePtr;
        break;
    case LiveGame::Live08:
        devicePtrAddress = kLive08DevicePtr;
        break;
    default:
        Log("Screenshot: unsupported game.");
        return false;
    }

    IDirect3DDevice9* device =
        *reinterpret_cast<IDirect3DDevice9**>(devicePtrAddress);

    if (!device) {
        Log("Screenshot: no current D3D9 device.");
        return false;
    }

    IDirect3DSurface9* backBuffer = nullptr;
    HRESULT hr = device->GetBackBuffer(
        0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);

    if (FAILED(hr) || !backBuffer) {
        Log("Screenshot: GetBackBuffer failed hr=0x%08lX",
            (unsigned long)hr);
        return false;
    }

    D3DSURFACE_DESC desc = {};
    hr = backBuffer->GetDesc(&desc);
    if (FAILED(hr)) {
        Log("Screenshot: backbuffer GetDesc failed hr=0x%08lX",
            (unsigned long)hr);
        backBuffer->Release();
        return false;
    }

    Log("Screenshot: source %ux%u fmt=%lu MSAA=%s(%lu) q=%lu",
        desc.Width,
        desc.Height,
        (unsigned long)desc.Format,
        SampleName(desc.MultiSampleType),
        (unsigned long)desc.MultiSampleType,
        (unsigned long)desc.MultiSampleQuality);

    if (desc.Format != D3DFMT_A8R8G8B8 &&
        desc.Format != D3DFMT_X8R8G8B8)
    {
        Log("Screenshot: unsupported backbuffer format %lu.",
            (unsigned long)desc.Format);
        backBuffer->Release();
        return false;
    }

    IDirect3DSurface9* resolved = nullptr;

    // GetRenderTargetData cannot read a multisampled render target directly.
    // Resolve it first into a matching non-MSAA default-pool render target.
    hr = device->CreateRenderTarget(
        desc.Width,
        desc.Height,
        desc.Format,
        D3DMULTISAMPLE_NONE,
        0,
        FALSE,
        &resolved,
        nullptr);

    if (FAILED(hr) || !resolved) {
        Log("Screenshot: CreateRenderTarget(resolve) failed hr=0x%08lX",
            (unsigned long)hr);
        backBuffer->Release();
        return false;
    }

    hr = device->StretchRect(
        backBuffer,
        nullptr,
        resolved,
        nullptr,
        D3DTEXF_NONE);

    if (FAILED(hr)) {
        Log("Screenshot: MSAA resolve StretchRect failed hr=0x%08lX",
            (unsigned long)hr);
        resolved->Release();
        backBuffer->Release();
        return false;
    }

    IDirect3DSurface9* systemSurface = nullptr;
    hr = device->CreateOffscreenPlainSurface(
        desc.Width,
        desc.Height,
        desc.Format,
        D3DPOOL_SYSTEMMEM,
        &systemSurface,
        nullptr);

    if (FAILED(hr) || !systemSurface) {
        Log("Screenshot: CreateOffscreenPlainSurface failed hr=0x%08lX",
            (unsigned long)hr);
        resolved->Release();
        backBuffer->Release();
        return false;
    }

    hr = device->GetRenderTargetData(resolved, systemSurface);
    if (FAILED(hr)) {
        Log("Screenshot: GetRenderTargetData failed hr=0x%08lX",
            (unsigned long)hr);
        systemSurface->Release();
        resolved->Release();
        backBuffer->Release();
        return false;
    }

    D3DLOCKED_RECT locked = {};
    hr = systemSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        Log("Screenshot: system surface LockRect failed hr=0x%08lX",
            (unsigned long)hr);
        systemSurface->Release();
        resolved->Release();
        backBuffer->Release();
        return false;
    }

    const ScreenshotFormat screenshotFormat = GetScreenshotFormat();

    char filename[MAX_PATH * 4] = {};
    bool haveName = FindScreenshotFilename(
        filename,
        sizeof(filename),
        screenshotFormat);

    bool saved = false;

    if (haveName) {
        if (screenshotFormat == ScreenshotFormat::PNG) {
            saved = WritePng24(
                filename,
                desc.Width,
                desc.Height,
                locked,
                desc.Format);
        }
        else {
            saved = WriteTga24(
                filename,
                desc.Width,
                desc.Height,
                locked,
                desc.Format);
        }
    }

    systemSurface->UnlockRect();

    systemSurface->Release();
    resolved->Release();
    backBuffer->Release();

    if (!haveName) {
        Log("Screenshot: unable to allocate output filename.");
        return false;
    }

    Log("Screenshot: absolute output path = %s", filename);
    Log("Screenshot: %s", saved ? "saved successfully" : "WRITE FAILED");

    return saved;
}

// Original Live 06 TakeScreenshot ends with RET 4, so model its entry as a
// __thiscall member with one stack argument. __fastcall lets us receive ECX
// plus a dummy EDX and still emits RET 4 for the remaining stack argument.

bool __fastcall HookTakeScreenshot05(
    void* /*self*/,
    void* /*edxDummy*/,
    void* /*arg*/)
{
    Log("Live 2005 TakeScreenshot intercepted.");
    return CaptureMsaaScreenshot();
}

bool InstallTakeScreenshotHook05()
{
    const BYTE expected[kLive05TakeScreenshotPatchSize] = {
        0x55,
        0x8B, 0xEC,
        0x83, 0xE4, 0xF8
    };

    if (std::memcmp(
            reinterpret_cast<const void*>(kLive05TakeScreenshot),
            expected,
            sizeof(expected)) != 0)
    {
        Log("Live 2005 screenshot hook FAILED: unexpected bytes at %08lX.",
            (unsigned long)kLive05TakeScreenshot);
        return false;
    }

    BYTE* target = reinterpret_cast<BYTE*>(kLive05TakeScreenshot);
    DWORD oldProtect = 0;

    if (!VirtualProtect(
            target,
            kLive05TakeScreenshotPatchSize,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        Log("Live 2005 screenshot hook FAILED: VirtualProtect error=%lu.",
            (unsigned long)GetLastError());
        return false;
    }

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookTakeScreenshot05) -
            (kLive05TakeScreenshot + 5));
    target[5] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(
        target,
        kLive05TakeScreenshotPatchSize,
        oldProtect,
        &ignored);

    FlushInstructionCache(
        GetCurrentProcess(),
        target,
        kLive05TakeScreenshotPatchSize);

    Log("Live 2005 TakeScreenshot hook installed at %08lX.",
        (unsigned long)kLive05TakeScreenshot);
    return true;
}

bool __fastcall HookTakeScreenshot06(
    void* /*self*/,
    void* /*edxDummy*/,
    void* /*arg*/)
{
    Log("Live 06 TakeScreenshot intercepted.");
    return CaptureMsaaScreenshot();
}

bool InstallTakeScreenshotHook06()
{
    const BYTE expected[kLive06TakeScreenshotPatchSize] = {
        0x55,             // push ebp
        0x8B, 0xEC,       // mov ebp, esp
        0x83, 0xE4, 0xF8  // and esp, -8
    };

    if (std::memcmp(
            reinterpret_cast<const void*>(kLive06TakeScreenshot),
            expected,
            sizeof(expected)) != 0)
    {
        Log("Screenshot hook FAILED: unexpected bytes at %08lX.",
            (unsigned long)kLive06TakeScreenshot);
        return false;
    }

    BYTE* target = reinterpret_cast<BYTE*>(kLive06TakeScreenshot);
    DWORD oldProtect = 0;

    if (!VirtualProtect(
            target,
            kLive06TakeScreenshotPatchSize,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        Log("Screenshot hook FAILED: VirtualProtect error=%lu.",
            (unsigned long)GetLastError());
        return false;
    }

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookTakeScreenshot06) -
            (kLive06TakeScreenshot + 5));
    target[5] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(
        target,
        kLive06TakeScreenshotPatchSize,
        oldProtect,
        &ignored);

    FlushInstructionCache(
        GetCurrentProcess(),
        target,
        kLive06TakeScreenshotPatchSize);

    Log("Live 06 TakeScreenshot hook installed at %08lX.",
        (unsigned long)kLive06TakeScreenshot);
    return true;
}

bool __fastcall HookTakeScreenshot07(
    void* /*self*/,
    void* /*edxDummy*/,
    void* /*arg*/)
{
    Log("Live 07 TakeScreenshot intercepted.");
    return CaptureMsaaScreenshot();
}

bool InstallTakeScreenshotHook07()
{
    const BYTE expected[kLive07TakeScreenshotPatchSize] = {
        0x55,             // push ebp
        0x8B, 0xEC,       // mov ebp, esp
        0x83, 0xE4, 0xF8  // and esp, -8
    };

    if (std::memcmp(
            reinterpret_cast<const void*>(kLive07TakeScreenshot),
            expected,
            sizeof(expected)) != 0)
    {
        Log("Live 07 screenshot hook FAILED: unexpected bytes at %08lX.",
            (unsigned long)kLive07TakeScreenshot);
        return false;
    }

    BYTE* target = reinterpret_cast<BYTE*>(kLive07TakeScreenshot);
    DWORD oldProtect = 0;

    if (!VirtualProtect(
            target,
            kLive07TakeScreenshotPatchSize,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        Log("Live 07 screenshot hook FAILED: VirtualProtect error=%lu.",
            (unsigned long)GetLastError());
        return false;
    }

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookTakeScreenshot07) -
            (kLive07TakeScreenshot + 5));
    target[5] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(
        target,
        kLive07TakeScreenshotPatchSize,
        oldProtect,
        &ignored);

    FlushInstructionCache(
        GetCurrentProcess(),
        target,
        kLive07TakeScreenshotPatchSize);

    Log("Live 07 TakeScreenshot hook installed at %08lX.",
        (unsigned long)kLive07TakeScreenshot);
    return true;
}


bool __fastcall HookTakeScreenshot08(
    void* /*self*/,
    void* /*edxDummy*/,
    void* /*arg*/)
{
    Log("Live 08 TakeScreenshot intercepted.");
    return CaptureMsaaScreenshot();
}

bool InstallTakeScreenshotHook08()
{
    const BYTE expected[kLive08TakeScreenshotPatchSize] = {
        0x55,
        0x8B, 0xEC,
        0x83, 0xE4, 0xF8
    };

    if (std::memcmp(
            reinterpret_cast<const void*>(kLive08TakeScreenshot),
            expected,
            sizeof(expected)) != 0)
    {
        Log("Live 08 screenshot hook FAILED: unexpected bytes at %08lX.",
            (unsigned long)kLive08TakeScreenshot);
        return false;
    }

    BYTE* target = reinterpret_cast<BYTE*>(kLive08TakeScreenshot);
    DWORD oldProtect = 0;

    if (!VirtualProtect(
            target,
            kLive08TakeScreenshotPatchSize,
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        Log("Live 08 screenshot hook FAILED: VirtualProtect error=%lu.",
            (unsigned long)GetLastError());
        return false;
    }

    target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(target + 1) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&HookTakeScreenshot08) -
            (kLive08TakeScreenshot + 5));
    target[5] = 0x90;

    DWORD ignored = 0;
    VirtualProtect(
        target,
        kLive08TakeScreenshotPatchSize,
        oldProtect,
        &ignored);

    FlushInstructionCache(
        GetCurrentProcess(),
        target,
        kLive08TakeScreenshotPatchSize);

    Log("Live 08 TakeScreenshot hook installed at %08lX.",
        (unsigned long)kLive08TakeScreenshot);
    return true;
}


LiveGame DetectGame()
{
    const uintptr_t ep = FM::GetEntryPoint();

    // NBA Live 2005 uses a different executable entry point. Verify the
    // renderer and screenshot signatures instead of relying on 06-08 markers.
    if (ep == 0x00CD8005) {
        const BYTE live05CreateSig[5] = {
            0xA1, 0xD0, 0x6C, 0xC5, 0x00
        };
        const BYTE live05ScreenshotSig[6] = {
            0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8
        };

        if (std::memcmp(
                reinterpret_cast<const void*>(0x006DBA24),
                live05CreateSig,
                sizeof(live05CreateSig)) == 0 &&
            std::memcmp(
                reinterpret_cast<const void*>(kLive05TakeScreenshot),
                live05ScreenshotSig,
                sizeof(live05ScreenshotSig)) == 0)
        {
            return LiveGame::Live05;
        }

        return LiveGame::Unknown;
    }

    if (ep != 0x40109F)
        return LiveGame::Unknown;

    // NBA Live 08 1.0 NOCD: verify two independent renderer signatures.
    const BYTE live08CreateSig[5] = {
        0xA1, 0x70, 0x50, 0xD8, 0x00
    };
    const BYTE live08ScreenshotSig[6] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8
    };

    if (std::memcmp(
            reinterpret_cast<const void*>(0x0042C773),
            live08CreateSig,
            sizeof(live08CreateSig)) == 0 &&
        std::memcmp(
            reinterpret_cast<const void*>(kLive08TakeScreenshot),
            live08ScreenshotSig,
            sizeof(live08ScreenshotSig)) == 0)
    {
        return LiveGame::Live08;
    }

    if (patch::GetFloat(0xBD832C) == 1.3333334f)
        return LiveGame::Live06;

    if (patch::GetFloat(0xBBBC3C) == 1.3333334f)
        return LiveGame::Live07;

    return LiveGame::Unknown;
}

const char* GameName()
{
    switch (gGame) {
    case LiveGame::Live05: return "NBA Live 2005";
    case LiveGame::Live06: return "NBA Live 06";
    case LiveGame::Live07: return "NBA Live 07";
    case LiveGame::Live08: return "NBA Live 08";
    default: return "Unknown";
    }
}

} // namespace

void InitializeAntiAliasing()
{
    InitializeDebugLogging();

    if (gDebugLoggingEnabled) {
        char logPath[MAX_PATH] = {};
        if (BuildDebugLogPath(logPath, sizeof(logPath), "msaa_debug.log"))
            std::remove(logPath);
    }

    Log("InitializeAntiAliasing v24 direct 2005/06/07/08 + independent screenshots entered.");

    gGame = DetectGame();
    if (gGame == LiveGame::Unknown) {
        Log("Unsupported game/build; graphics hooks not installed.");
        return;
    }

    Log("Detected %s.", GameName());

    // Screenshot redirection is independent of anti-aliasing.
    // Install it before checking [DISPLAY] ANTI_ALIASING.
    switch (gGame) {
    case LiveGame::Live05:
        InstallTakeScreenshotHook05();
        break;

    case LiveGame::Live06:
        InstallTakeScreenshotHook06();
        break;

    case LiveGame::Live07:
        InstallTakeScreenshotHook07();
        break;

    case LiveGame::Live08:
        InstallTakeScreenshotHook08();
        break;

    default:
        break;
    }

    int enabled = GetPrivateProfileIntA(
        "DISPLAY", "ANTI_ALIASING", 0, ".\\main.ini");
    int samples = GetPrivateProfileIntA(
        "DISPLAY", "MSAA_SAMPLES", 4, ".\\main.ini");

    Log("main.ini: ANTI_ALIASING=%d MSAA_SAMPLES=%d",
        enabled,
        samples);

    if (enabled != 1) {
        gEnabled = false;
        Log("MSAA disabled; rewritten screenshot path remains active.");
        return;
    }

    if (samples != 2 &&
        samples != 4 &&
        samples != 8 &&
        samples != 16)
        samples = 4;

    gEnabled = true;
    gRequestedSamples =
        static_cast<unsigned int>(samples);

    if (gGame == LiveGame::Live05) {
        // Direct game-owned renderer patches only.
        InstallLive05DirectMsaaHooks();
        return;
    }

    if (gGame == LiveGame::Live07) {
        // Direct game-owned renderer patches only.
        InstallLive07DirectMsaaHooks();
        return;
    }

    if (gGame == LiveGame::Live08) {
        // Direct game-owned renderer patches only.
        InstallLive08DirectMsaaHooks();
        return;
    }

    // NBA Live 06: retain the proven MSAA implementation.
    PatchNativeMsaaRestore();

    void* original = nullptr;
    bool ok = HookImport(
        "d3d9.dll",
        "Direct3DCreate9",
        reinterpret_cast<void*>(&HookDirect3DCreate9),
        &original);

    if (ok) {
        gOriginalDirect3DCreate9 =
            reinterpret_cast<Direct3DCreate9Fn>(original);

        Log("Global D3D9 interception installed. Every future CreateDevice and Reset will be patched.");
    }
    else {
        Log("FAILED to hook d3d9.dll!Direct3DCreate9 import.");
    }
}

