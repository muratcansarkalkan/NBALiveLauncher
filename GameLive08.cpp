#include "plugin-std.h"
#include "Resolutions.h"

using namespace plugin;

const unsigned int RES_X = GetPrivateProfileIntW(L"DISPLAY", L"RES_X", 640, L".\\main.ini");
const unsigned int RES_Y = GetPrivateProfileIntW(L"DISPLAY", L"RES_Y", 480, L".\\main.ini");
const unsigned int INTRO = GetPrivateProfileIntW(L"BOOTUP", L"INTRO", 1, L".\\main.ini");
const float ASPECT_RATIO = static_cast<float>(RES_X) / static_cast<float>(RES_Y);
const float ASPECT_DIFF = ASPECT_RATIO / 1.33333f;
static float gAspectScale08 = 1.0f;

namespace live08 {
    // Logical menu order. NBA Live 08, like 07, uses physical
    // resolution-table slot 1 as the startup/current mode.
    //
    // PrepareResolutionSlots08() keeps that native invariant by swapping the
    // configured RES_X/RES_Y mode into physical slot 1, then builds a separate
    // logical -> physical index map for the Display Settings menu.
    ResolutionID ids[] = {
        { 640, 480, 32, 0 },
        { 800, 600, 32, 1 },
        { 1024, 768, 32, 2 },
        { 1280, 720, 32, 3 },
        { 1280, 1024, 32, 4 },
        { 1366, 768, 32, 5 },
        { 1440, 900, 32, 6 },
        { 1600, 900, 32, 7 },
        { 1600, 1200, 32, 8 },
        { 1680, 1050, 32, 9 },
        { 1920, 1080, 32, 10 },
        { 2560, 1440, 32, 11 },
        { 3440, 1440, 32, 12 },
        { 3840, 1080, 32, 13 },
        { 3840, 1200, 32, 14 },
        { 3840, 1600, 32, 15 },
    };

    static unsigned int gResolutionMenuOrder08[16] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 9, 10, 11, 12, 13, 14, 15
    };

    static void PrepareResolutionSlots08()
    {
        int configuredLogicalIndex = -1;

        for (int i = 0; i < 16; ++i)
        {
            if (ids[i].width == RES_X &&
                ids[i].height == RES_Y)
            {
                configuredLogicalIndex = i;
                break;
            }
        }

        // Preserve the game's native startup/current-mode slot.
        // Swap only width/height/depth; id remains the physical table slot.
        if (configuredLogicalIndex >= 0 &&
            configuredLogicalIndex != 1)
        {
            const unsigned int width = ids[1].width;
            const unsigned int height = ids[1].height;
            const unsigned int depth = ids[1].depth;

            ids[1].width = ids[configuredLogicalIndex].width;
            ids[1].height = ids[configuredLogicalIndex].height;
            ids[1].depth = ids[configuredLogicalIndex].depth;

            ids[configuredLogicalIndex].width = width;
            ids[configuredLogicalIndex].height = height;
            ids[configuredLogicalIndex].depth = depth;
        }

        static const unsigned int logicalWidth[16] = {
            640, 800, 1024, 1280,
            1280, 1366, 1440, 1600,
            1600, 1680, 1920, 2560,
            3440, 3840, 3840, 3840
        };

        static const unsigned int logicalHeight[16] = {
            480, 600, 768, 720,
            1024, 768, 900, 900,
            1200, 1050, 1080, 1440,
            1440, 1080, 1200, 1600
        };

        for (int logical = 0; logical < 16; ++logical)
        {
            gResolutionMenuOrder08[logical] =
                static_cast<unsigned int>(logical);

            for (int physical = 0; physical < 16; ++physical)
            {
                if (ids[physical].width == logicalWidth[logical] &&
                    ids[physical].height == logicalHeight[logical])
                {
                    gResolutionMenuOrder08[logical] =
                        static_cast<unsigned int>(physical);
                    break;
                }
            }
        }
    }

    // 0055B7C0 builds the supported-resolution index vector.
    //
    // Stock append:
    //   0055B840  mov [edi], esi
    //   0055B842  add edi, 4
    //
    // Replace only the value appended to the vector. ESI remains the native
    // physical-table loop counter and EBP continues walking the 20-byte mode
    // records unchanged.
    static DWORD gResolutionMenuAppendReturn08 = 0x0055B845;

    __declspec(naked) static void AppendResolutionInLogicalOrder08()
    {
        __asm
        {
            mov edx, dword ptr [gResolutionMenuOrder08 + esi * 4]
            mov [edi], edx
            add edi, 4
            jmp dword ptr [gResolutionMenuAppendReturn08]
        }
    }

    // ---------------------------------------------------------------------
    // NBA Live 08 localized resolution labels
    // ---------------------------------------------------------------------
    //
    // 08's locale lookup is index based:
    //
    //   0041B710(outString, localeIndex)
    //       -> D84710 localization object
    //       -> 0041A3D0
    //       -> E1A762
    //
    // E1A762 proves the loaded locale representation:
    //   [localization]      -> locale table object
    //   [table + 0x08]      = entry count
    //   [table + 0x10]      = char ** source-string array A
    //   [table + 0x14]      = char ** source-string array B
    //
    // Instead of intercepting every localized lookup, patch the source-string
    // pointers after locale.eng/gameloc.eng have been loaded. This is
    // independent of locale IDs: we identify the stock resolution captions by
    // their actual source text, then replace only those entries.
    //
    // This also fixes the 08 ordering problem. The replacement text for native
    // physical slot N is generated from ids[N] AFTER PrepareResolutionSlots08()
    // has performed the required slot-1 swap.
    static const char* kNativeResolutionTexts08[16] =
    {
        "640x480x16",
        "640x480x32",
        "800x600x16",
        "800x600x32",
        "1024x768x16",
        "1024x768x32",
        "1280x720x16",
        "1280x720x32",
        "1280x1024x16",
        "1280x1024x32",
        "1440x900x16",
        "1440x900x32",
        "1600x1200x16",
        "1600x1200x32",
        "1680x1050x16",
        "1680x1050x32",
    };

    static char gResolutionLocaleText08[16][32] = {};

    static bool IsReadable08(const void* p, size_t bytes = sizeof(void*))
    {
        if (!p || bytes == 0)
            return false;

        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            return false;

        if (mbi.State != MEM_COMMIT)
            return false;

        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            return false;

        const uintptr_t begin =
            reinterpret_cast<uintptr_t>(p);
        const uintptr_t end =
            begin + bytes;
        const uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

        return end >= begin && end <= regionEnd;
    }

    static void BuildResolutionLocaleText08()
    {
        for (int physicalSlot = 0; physicalSlot < 16; ++physicalSlot)
        {
            sprintf_s(
                gResolutionLocaleText08[physicalSlot],
                sizeof(gResolutionLocaleText08[physicalSlot]),
                "%ux%u",
                ids[physicalSlot].width,
                ids[physicalSlot].height);
        }
    }

    static bool PatchLocaleArray08(char** entries, unsigned int count)
    {
        if (!entries || !IsReadable08(entries, sizeof(char*) * count))
            return false;

        bool touched = false;

        for (unsigned int entry = 0; entry < count; ++entry)
        {
            const char* text = entries[entry];

            if (!text || !IsReadable08(text, 1))
                continue;

            for (int physicalSlot = 0; physicalSlot < 16; ++physicalSlot)
            {
                const char* stock = kNativeResolutionTexts08[physicalSlot];

                // Resolution strings are tiny, so compare only after validating
                // enough readable bytes for the stock text plus its terminator.
                const size_t stockLen = strlen(stock);
                if (!IsReadable08(text, stockLen + 1))
                    continue;

                if (strcmp(text, stock) != 0)
                    continue;

                entries[entry] =
                    gResolutionLocaleText08[physicalSlot];

                touched = true;
                break;
            }
        }

        return touched;
    }

    static bool PatchLoadedResolutionLocale08()
    {
        void* localization =
            *reinterpret_cast<void**>(0x00D84710);

        if (!IsReadable08(localization))
            return false;

        void* table =
            *reinterpret_cast<void**>(localization);

        if (!IsReadable08(
                reinterpret_cast<unsigned char*>(table) + 0x14,
                sizeof(void*)))
            return false;

        const unsigned int count =
            *reinterpret_cast<unsigned int*>(
                reinterpret_cast<unsigned char*>(table) + 0x08);

        if (count == 0 || count > 100000)
            return false;

        char** entriesA =
            *reinterpret_cast<char***>(
                reinterpret_cast<unsigned char*>(table) + 0x10);

        char** entriesB =
            *reinterpret_cast<char***>(
                reinterpret_cast<unsigned char*>(table) + 0x14);

        const bool patchedA =
            PatchLocaleArray08(entriesA, count);

        const bool patchedB =
            PatchLocaleArray08(entriesB, count);

        return patchedA || patchedB;
    }

    static DWORD WINAPI ResolutionLocaleWatcher08(LPVOID)
    {
        // The ASI can initialize before locale.eng is loaded. Wait for the
        // localization table, patch it once, then exit.
        for (int attempt = 0; attempt < 600; ++attempt)
        {
            if (PatchLoadedResolutionLocale08())
                return 0;

            Sleep(50);
        }

        return 0;
    }

    static void InstallResolutionLocalePatch08()
    {
        BuildResolutionLocaleText08();

        // Handle both cases: locale already loaded, or locale loading later.
        if (PatchLoadedResolutionLocale08())
            return;

        HANDLE thread = CreateThread(
            nullptr,
            0,
            ResolutionLocaleWatcher08,
            nullptr,
            0,
            nullptr);

        if (thread)
            CloseHandle(thread);
    }

    // NBA Live 08
    DWORD* METHOD SetPerspectiveProjection08(
        float* _this, DUMMY_ARG,
        DWORD* a2,
        float fovY,
        float aspectRatio,
        float nearClip,
        float farClip)
    {
        const float w =
            static_cast<float>(*reinterpret_cast<unsigned int*>(_this + 2));

        const float h =
            static_cast<float>(*reinterpret_cast<unsigned int*>(_this + 3));

        if (h > 0.0f)
        {
            aspectRatio = w / h;

            gAspectScale08 =
                aspectRatio / (4.0f / 3.0f);

            if (gAspectScale08 < 1.0f)
                gAspectScale08 = 1.0f;
        }
        else
        {
            gAspectScale08 = 1.0f;
        }

        _this[15] = 0.0f;
        _this[6] = fovY;
        _this[7] = aspectRatio;
        _this[8] = nearClip;
        _this[9] = farClip;

        memcpy(_this + 36, (void*)0xC3EB00, 0x40u);

        const double fovR =
            1.0 / tan(static_cast<double>(fovY) * 0.0087266462);

        _this[47] = -1.0f;

        _this[36] = static_cast<float>(
            0.5 * static_cast<double>(w) /
            static_cast<double>(aspectRatio) *
            fovR
            );

        _this[41] = static_cast<float>(
            -(static_cast<double>(h) * 0.5 * fovR)
            );

        _this[44] = static_cast<float>(
            static_cast<double>(w) * -0.5 -
            static_cast<double>(*reinterpret_cast<int*>(_this))
            );

        _this[45] = static_cast<float>(
            static_cast<double>(h) * -0.5 -
            static_cast<double>(*reinterpret_cast<int*>(_this + 1))
            );

        const double dist =
            static_cast<double>(nearClip) -
            static_cast<double>(farClip);

        _this[46] = static_cast<float>(
            static_cast<double>(nearClip) / dist - 1.0
            );

        _this[50] = static_cast<float>(
            static_cast<double>(nearClip) *
            static_cast<double>(farClip) / dist
            );

        CallMethod<0x43C844>(_this, a2);

        return a2;
    }


    // NBA Live 08
    int SetTestInConicalFrustum08(
        float* a1,
        DUMMY_ARG,
        float radius,
        bool cameraclip)
    {
        const float dx =
            a1[0] - patch::GetFloat(0xDAC768);

        const float dy =
            a1[1] - patch::GetFloat(0xDAC76C);

        const float dz =
            a1[2] - patch::GetFloat(0xDAC770);

        float testZ = dz;

        if (cameraclip && radius > 2.0f && radius < 10.0f)
            testZ += radius;

        const float distanceSq =
            dx * dx +
            dy * dy +
            testZ * testZ;

        const float distance =
            sqrtf(distanceSq);

        if (distance <= 0.000001f)
            return 1;

        const float depth = -(
            dx * patch::GetFloat(0xDAC774) +
            dy * patch::GetFloat(0xDAC778) +
            testZ * patch::GetFloat(0xDAC77C)
            );

        if (patch::GetFloat(0xDAC758) - radius <= depth &&
            depth <= patch::GetFloat(0xDAC75C) + radius)
        {
            const float originalConeSine =
                patch::GetFloat(0xDAC760);

            const float originalConeInvCosine =
                patch::GetFloat(0xDAC764);

            const float originalTan =
                originalConeSine *
                originalConeInvCosine;

            /*
                08 previously used:

                    coneRadius� > radialDistance� * 0.3

                Equivalent cone expansion:

                    1 / sqrt(0.3) = ~1.825742

                Keep that existing 08 baseline, then apply the
                widescreen aspect expansion on top.
            */
            constexpr float BASE_CULL_SCALE_08 =
                1.825741858f;

            const float widenedTan =
                originalTan *
                BASE_CULL_SCALE_08 *
                gAspectScale08;

            const float widenedConeInvCosine =
                sqrtf(
                    1.0f +
                    widenedTan * widenedTan
                );

            const float widenedConeSine =
                widenedTan /
                widenedConeInvCosine;

            const float coneRadius =
                (depth * widenedConeSine + radius) *
                widenedConeInvCosine;

            float radialDistanceSq =
                distanceSq -
                depth * depth;

            // Avoid tiny floating-point negatives.
            if (radialDistanceSq < 0.0f)
                radialDistanceSq = 0.0f;

            if (coneRadius * coneRadius > radialDistanceSq)
            {
                if (!cameraclip)
                    return 1;

                if (radius >= 10.0f)
                {
                    radius += 5.0f;
                }
                else if (
                    0.25f *
                    coneRadius *
                    coneRadius >
                    radialDistanceSq)
                {
                    return 1;
                }

                if (distance >= radius)
                    return 1;
            }
        }

        return 0;
    }

    static float gRealFontRasterScale08 = 1.0f;
    static DWORD gRealFontBoundsReturn08 = 0x008CCA81;

    __declspec(naked) void RealFontLogicalAptBounds08()
    {
        __asm
        {
            fild dword ptr[esp + 0x1C]
            fdiv dword ptr[gRealFontRasterScale08]
                fadd dword ptr[edi + 4]
                    fstp dword ptr[edi + 0x0C]

                    fild dword ptr[esp + 0x34]
                    fld st(0)
                    fdiv dword ptr[gRealFontRasterScale08]
                    fadd dword ptr[edi + 8]
                        fstp dword ptr[edi + 0x10]

                        jmp dword ptr[gRealFontBoundsReturn08]
        }
    }

    void InstallHighResolutionAptFonts08()
    {
        gRealFontRasterScale08 =
            (RES_Y > 0)
            ? static_cast<float>(RES_Y) / 480.0f
            : 1.0f;

        if (gRealFontRasterScale08 <= 0.0f)
            gRealFontRasterScale08 = 1.0f;

        const float fontTextureScale =
            1.0f / gRealFontRasterScale08;


        // RealFont scaling
        patch::SetUInt(0xD5CC84, 0);
        patch::SetFloat(0xD5CC7C, gRealFontRasterScale08);
        patch::SetFloat(0xD5CC80, gRealFontRasterScale08);


        // Keep generated quad at logical size
        patch::Nop(0x8CCCA7, 4);
        patch::Nop(0x8CCCC4, 4);


        // TextureScaleApt
        patch::SetFloat(0x8CD26F, fontTextureScale);
        patch::SetFloat(0x8CD276, fontTextureScale);


        // Logical APT bounds
        patch::RedirectJump(
            0x8CCA6B,
            RealFontLogicalAptBounds08
        );

        patch::Nop(
            0x8CCA70,
            0x11
        );

        // FFN isolation:
        // 8CC1D2: mov eax, [D5CC7C]
        patch::SetUChar(0x8CC1D2, 0xB8);
        patch::SetUInt(0x8CC1D3, 0x3F800000);

        // 8CC1EF: mov edx, [D5CC80]
        patch::SetUChar(0x8CC1EF, 0xBA);
        patch::SetUInt(0x8CC1F0, 0x3F800000);
        patch::Nop(0x8CC1F4, 1);
    }

    float loadYPos = ((400.0f / 480.0f) * RES_Y);
    float loadXPos(float xPos) {
        return ((xPos / 640.0f) * (RES_Y * 1.33333f)) + ((RES_X - (RES_Y * 1.33333f)) / 2);
    }
    // CreationZone
    int czXPos = (int)(((362.0f / 640.0f) * (RES_Y * 1.33333f)) + ((RES_X - (RES_Y * 1.33333f)) / 2));
    int czYPos = (int)(((84.0f / 480.0f) * RES_Y));
    int czWidth = (int)((225.0f * (RES_Y / 480.0f)));
    int czHeight = (int)((303.0f * (RES_Y / 480.0f)));

    static void METHOD ProgressBarCreate(int* _t, DUMMY_ARG, DWORD* a2, int xLocation, int yLocation, int width, int height, int a7, int a8) {
        CallMethod<0x43BC94>(_t, a2, czXPos, czYPos, czWidth, czHeight, a7, a8);
    }

}

void Install_LIVE08() {
    using namespace live08;
    // resolutions
    patch::RedirectJump(0x43B341, SetPerspectiveProjection08);
    patch::RedirectJump(0x69F780, SetTestInConicalFrustum08);
    InstallHighResolutionAptFonts08();

    // ---------------------------------------------------------------------
    // APT / frontend timing
    // ---------------------------------------------------------------------
    // GameDirector's frontend loop clamps its timer-derived delta:
    //
    //   00669974 cmp edi, 80h
    //   00669980 mov edi, 80h
    //   00669987 cmp edi, 1
    //   0066998A jge ...
    //   0066998C mov edi, 1
    //
    // With an uncapped frontend, elapsed time below one unit gets promoted to
    // 1 every render iteration, making APT/menu animation run too quickly.
    // Permit a zero delta instead. The separate intentional preload loop at
    // 00669ADF (1000 x FESystem::Update(1)) is left untouched.
    patch::SetUChar(0x66998C, 0x31); // xor edi, edi
    patch::SetUChar(0x66998D, 0xFF);
    patch::Nop(0x66998E, 3);

    // Keep physical slot 1 as the configured startup/current mode, but make
    // the Display Settings menu enumerate modes in logical resolution order.
    PrepareResolutionSlots08();

    // Patch the loaded locale source arrays after the physical slot map is
    // finalized. This replaces the failed post-lookup hook.
    InstallResolutionLocalePatch08();

    // 55B840: mov [edi], esi / add edi, 4
    patch::RedirectJump(0x55B840, AppendResolutionInLogicalOrder08);
for (const auto& resolution : ids) {
        patch::SetUInt(0xD21F60 + 20 * resolution.id + 4, resolution.width);
        patch::SetUInt(0xD21F60 + 20 * resolution.id + 8, resolution.height);
        patch::SetUInt(0xD21F60 + 20 * resolution.id + 12, resolution.depth);
    }
    patch::SetUInt(0x4145DC + 1, RES_Y);
    patch::SetUInt(0x4145E1 + 1, RES_X);
    patch::SetUInt(0x41F4E3 + 1, RES_Y);
    patch::SetUInt(0x41F4E8 + 1, RES_X);
    patch::SetUInt(0x66AFF0 + 1, RES_Y);
    patch::SetUInt(0x66AFF5 + 1, RES_X);
    patch::SetUInt(0xA12A12 + 1, RES_Y);
    patch::SetUInt(0xA12A17 + 1, RES_X);
    patch::SetUInt(0xE1E48C + 1, RES_Y);
    patch::SetUInt(0xE1E491 + 1, RES_X);
    // loadbar
    patch::SetFloat(0x65F110 + 6, loadYPos);
    patch::SetFloat(0x65F124 + 6, loadYPos);
    patch::SetFloat(0x65F138 + 6, loadYPos);
    patch::SetFloat(0x65F14C + 6, loadYPos);
    patch::SetFloat(0x65F160 + 6, loadYPos);
    patch::SetFloat(0x65F174 + 6, loadYPos);
    patch::SetFloat(0x65F188 + 6, loadYPos);
    patch::SetFloat(0x65F19C + 6, loadXPos(201.0f));
    patch::SetFloat(0x65F1A6 + 6, loadXPos(233.0f));
    patch::SetFloat(0x65F1B0 + 6, loadXPos(265.0f));
    patch::SetFloat(0x65F1BA + 6, loadXPos(297.0f));
    patch::SetFloat(0x65F1C4 + 6, loadXPos(329.0f));
    patch::SetFloat(0x65F1CE + 6, loadXPos(361.0f));
    patch::SetFloat(0x65F1D8 + 6, loadXPos(393.0f));
    // enable/disable intro
    if (INTRO != 1) {
        patch::SetPointer(0x56D8F5 + 1, "DUMMY");
        patch::SetPointer(0x56DAA3 + 1, "DUMMY");
    }
    // CreateZone
    patch::RedirectCall(0x66C667, ProgressBarCreate);
    patch::RedirectCall(0x66C6B7, ProgressBarCreate);
}
