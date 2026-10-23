#include "plugin-std.h"
#include "Resolutions.h"
#include "AdjustWSUI.h"
#include <Windows.h>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <climits>
#include <cwchar>

using namespace plugin;

const unsigned int RES_X = GetPrivateProfileIntW(L"DISPLAY", L"RES_X", 640, L".\\main.ini");
const unsigned int RES_Y = GetPrivateProfileIntW(L"DISPLAY", L"RES_Y", 480, L".\\main.ini");
const unsigned int INTRO = GetPrivateProfileIntW(L"BOOTUP", L"INTRO", 1, L".\\main.ini");
const float ASPECT_RATIO = static_cast<float>(RES_X) / static_cast<float>(RES_Y);

namespace live06 {
    
    // An array of resolutions
    ResolutionID ids[] = {
        { 640,  480, 32, 0 }, // 640x480x16
        { 800,  600, 32, 1 }, // 640x480x32
        { 1024,  768, 32, 2 }, // 800x600x16
        { 1280,  720, 32, 3 }, // 800x600x32
        { 1280, 1024, 32, 4 },
        { 1366,  768, 32, 5 },
        { 1440,  900, 32, 6 },
        { 1600,  900, 32, 7 },
        { 1920, 1080, 32, 8 },
        { 2560, 1440, 32, 9 },
    };

    // Function modified to set aspect ratio based on height and width
// NBA Live 06
// Correct widescreen projection:
// - preserves vertical FOV
// - uses actual viewport aspect ratio
// - preserves original near/far planes
    DWORD* METHOD SetPerspectiveProjection06(
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
            aspectRatio = w / h;

        _this[15] = 0.0f;        // mGuardBandScale
        _this[6] = fovY;        // mFrustum.mFov
        _this[7] = aspectRatio; // mFrustum.mAspect
        _this[8] = nearClip;    // mFrustum.mNearPlane
        _this[9] = farClip;     // mFrustum.mFarPlane

        memcpy(_this + 36, (void*)0xBE9DC0, 0x40u);

        const double fovR =
            1.0 / tan(static_cast<double>(fovY) * 0.0087266462);

        _this[47] = -1.0f; // mProjectionMatrix.m44[2][3]

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

        CallMethod<0x702DF1>(_this, a2);

        return a2;
    }


    // NBA Live 06
    // Correct widescreen conservative conical-frustum culling.
    //
    // Original StaticFastCull is based on a circular cone. Widescreen
    // exposes a wider horizontal FOV, so widen the cone itself rather
    // than modifying near/far clipping distances.
    int METHOD SetTestInConicalFrustum06(
        float* _this, DUMMY_ARG,
        float* a2,
        float radius,
        bool cameraclip)
    {
        float v8 = a2[0] - _this[4];
        float v9 = a2[1] - _this[5];
        float v10 = a2[2] - _this[6];

        float v7 = v10;

        if (cameraclip && radius > 2.0f && radius < 10.0f)
            v7 += radius;

        const float distanceSq =
            v8 * v8 +
            v9 * v9 +
            v7 * v7;

        const float a2a = sqrtf(distanceSq);

        if (a2a <= 0.000001f)
            return 1;

        /*
            Game resolution.

            0xCBFB30 = width
            0xCBFB34 = height
        */
        const float width =
            static_cast<float>(patch::GetUInt(0xCBFB30));

        const float height =
            static_cast<float>(patch::GetUInt(0xCBFB34));

        float aspectScale = 1.0f;

        if (height > 0.0f)
        {
            const float aspect = width / height;

            /*
                NBA Live was designed around 4:3.
            */
            aspectScale = aspect / (4.0f / 3.0f);

            // Never make the original culling cone narrower.
            if (aspectScale < 1.0f)
                aspectScale = 1.0f;
        }

        /*
            Original expression simplifies to the negative dot product
            with the cone axis.

            _this[7..9] = mConeAxis.xyz
        */
        const float cameraclipa = -(
            v8 * _this[7] +
            v9 * _this[8] +
            v7 * _this[9]
            );

        /*
            Keep the original near/far depth test unchanged.

            _this[0] = mNearPlane
            _this[1] = mFarPlane
        */
        if (_this[0] - radius <= cameraclipa &&
            cameraclipa <= _this[1] + radius)
        {
            /*
                Original values:

                _this[2] = mConeSine
                _this[3] = mConeInvCosine

                tan(theta) = sin(theta) / cos(theta)
                           = mConeSine * mConeInvCosine
            */
            const float originalTan =
                _this[2] * _this[3];

            const float widescreenTan =
                originalTan * aspectScale;

            /*
                sec(theta) = sqrt(1 + tan^2(theta))
                sin(theta) = tan(theta) / sec(theta)
            */
            const float coneInvCosine =
                sqrtf(1.0f + widescreenTan * widescreenTan);

            const float coneSine =
                widescreenTan / coneInvCosine;

            /*
                Same role as original v6:
                    (depth * mConeSine + radius) * mConeInvCosine
            */
            const float v6 =
                (cameraclipa * coneSine + radius) *
                coneInvCosine;

            /*
                Squared distance perpendicular to cone axis.
            */
            const float cameraclipb =
                distanceSq -
                cameraclipa * cameraclipa;

            if (v6 * v6 > cameraclipb)
            {
                if (!cameraclip)
                    return 1;

                if (radius >= 10.0f)
                {
                    radius += 5.0f;
                }
                else if (0.25f * v6 * v6 > cameraclipb)
                {
                    /*
                        Original constant = 0.5f:
                        0.5^2 = 0.25
                    */
                    return 1;
                }

                if (a2a >= radius)
                    return 1;
            }
        }

        return 0;
    }


    // -------------------------------------------------------------------------
    // High-resolution APT RealFont rendering
    //
    // NBA Live 06 renders APT RealFont text through FontFusion into temporary
    // Text_N textures. The original game couples raster resolution, text-quad
    // size, and texture scale around a 480-line reference resolution.
    //
    // For higher resolutions:
    //   - rasterize the font at RES_Y / 480 resolution,
    //   - keep the APT text quad at its original logical size,
    //   - apply the reciprocal TextureScaleApt value,
    //   - report logical text bounds to controls that auto-size from text.
    //
    // Relevant original globals:
    //   0xC874C4 = size threshold (64)
    //   0xC874BC = RealFont X raster scale (~0.7071 above threshold)
    //   0xC874C0 = RealFont Y raster scale (~0.7071 above threshold)
    //
    // Relevant sub_7BDC00 instructions:
    //   0x7BE237 = fmul [var_27C]  (quad width)
    //   0x7BE254 = fmul [var_260]  (quad height)
    //   0x7BE7FC = mov [ebx+2Ch], 1.0f
    //   0x7BE803 = mov [ebx+30h], 1.0f
    // -------------------------------------------------------------------------
    static float gRealFontRasterScale06 = 1.0f;
    static DWORD gRealFontBoundsReturn06 = 0x007BE011;

    __declspec(naked) void RealFontLogicalAptBounds06()
    {
        __asm
        {
            // Original:
            // fild [esp+290h+var_274]
            //
            // var_274 is at ESP + 0x1C here.
            fild dword ptr[esp + 0x1C]

            // Convert supersampled measurement back to logical APT units.
            fdiv dword ptr[gRealFontRasterScale06]

                // Original positioning.
                fadd dword ptr[edi + 4]
                    fstp dword ptr[edi + 0x0C]

                    // Original:
                    // fild [esp+290h+var_25C]
                    //
                    // var_25C is at ESP + 0x34 here.
                    fild dword ptr[esp + 0x34]

                    // Important: preserve one copy of the ORIGINAL height on the FPU
                    // stack because stock code at 0x7BE01E subsequently scales it
                    // for the physical raster.
                    fld st(0)

                    // Only the copy used for the APT bottom bound is logicalized.
                    fdiv dword ptr[gRealFontRasterScale06]
                    fadd dword ptr[edi + 8]
                        fstp dword ptr[edi + 0x10]

                        // ST(0) is now still the original measured height, exactly as
                        // stock code expects when execution resumes at 0x7BE011.
                        jmp dword ptr[gRealFontBoundsReturn06]
        }
    }
    void InstallHighResolutionAptFonts06()
    {
        gRealFontRasterScale06 =
            static_cast<float>(RES_Y) / 480.0f;

        if (gRealFontRasterScale06 <= 0.0f)
            gRealFontRasterScale06 = 1.0f;

        const float fontTextureScale =
            1.0f / gRealFontRasterScale06;

        // Force all RealFonts through EA's scalable path.
        patch::SetUInt(0xC874C4, 0);

        // Higher-resolution FontFusion raster.
        patch::SetFloat(0xC874BC, gRealFontRasterScale06);
        patch::SetFloat(0xC874C0, gRealFontRasterScale06);

        // Keep rendered text quad at logical size.
        patch::Nop(0x7BE237, 4);
        patch::Nop(0x7BE254, 4);

        // Map the supersampled texture back onto the logical-size quad.
        patch::SetFloat(
            0x7BE7FF,
            fontTextureScale
        );

        patch::SetFloat(
            0x7BE806,
            fontTextureScale
        );

        // 007BD762:
        patch::SetUChar(0x7BD762, 0xB8);
        patch::SetUInt(0x7BD763, 0x3F800000);

        // 007BD77F:
        patch::SetUChar(0x7BD77F, 0xBA);
        patch::SetUInt(0x7BD780, 0x3F800000);
        patch::Nop(0x7BD784, 1);
        // Keep APT's reported text bounds at logical size so buttons,
        // backgrounds and other auto-sized elements don't grow with
        // the supersampled font texture.
        //
        // Replaces 0x7BDFFB through 0x7BE010 (0x16 bytes).
        patch::RedirectJump(
            0x7BDFFB,
            RealFontLogicalAptBounds06
        );

        // RedirectJump consumes the first 5 bytes; clear the rest.
        patch::Nop(0x7BE000, 0x11);
    }

    // -------------------------------------------------------------------------
    // NBA Live 06 APT runtime inspector
    //
    // Read-only first test of the native APT API.  The game itself uses
    // sub_78B650(0) as the active level-0 object and resolves properties
    // through AptExtObject::GetVariable.
    // -------------------------------------------------------------------------
    namespace apt06
    {
        struct AptValue;

        struct EAStringC
        {
            void* data;
        };

        using GetLevelFn = AptValue* (__cdecl*)(int);
        using MakeStringFn = void (__thiscall*)(EAStringC*, const char*);
        using GetVariableFn = AptValue* (__cdecl*)(AptValue*, const EAStringC*);
        using SetVariableFn = bool (__cdecl*)(AptValue*, const EAStringC*, AptValue*);
        using CreateFloatFn = AptValue* (__cdecl*)(float);
        using ToFloatFn = float (__thiscall*)(AptValue*);
        using ToIntFn = int (__thiscall*)(AptValue*);

        static AptValue* GetLevel(int level)
        {
            return reinterpret_cast<GetLevelFn>(0x0078B650)(level);
        }

        static void ReleaseString(EAStringC& value)
        {
            if (!value.data)
                return;

            auto* refCount = reinterpret_cast<unsigned short*>(value.data);
            if (*refCount != 0)
                --(*refCount);

            if (*refCount == 0)
            {
                void* callbacks = *reinterpret_cast<void**>(0x00CE270C);
                if (callbacks)
                {
                    using FreeFn = void (__cdecl*)(void*);
                    FreeFn freeFn = *reinterpret_cast<FreeFn*>(
                        reinterpret_cast<unsigned char*>(callbacks) + 4);
                    if (freeFn)
                        freeFn(value.data);
                }
            }

            value.data = nullptr;
        }

        static AptValue* GetVariable(AptValue* object, const char* name)
        {
            if (!object || !name || !*name)
                return nullptr;

            EAStringC string{};
            reinterpret_cast<MakeStringFn>(0x0077E750)(&string, name);
            AptValue* result = reinterpret_cast<GetVariableFn>(0x007AB930)(object, &string);
            ReleaseString(string);
            return result;
        }

        static bool SetFloat(AptValue* object, const char* name, float value)
        {
            if (!object || !name || !*name)
                return false;

            EAStringC string{};
            reinterpret_cast<MakeStringFn>(0x0077E750)(&string, name);

            AptValue* aptValue =
                reinterpret_cast<CreateFloatFn>(0x0078B390)(value);

            bool result = false;
            if (aptValue)
            {
                result = reinterpret_cast<SetVariableFn>(0x007AB960)(
                    object, &string, aptValue);

                // Deliberately do not release aptValue in this first write test.
                // Only two AptFloat objects are created for the whole session
                // (move + restore), avoiding any uncertainty about ownership.
            }

            ReleaseString(string);
            return result;
        }

        static unsigned int Flags(AptValue* value)
        {
            if (!value)
                return 0;
            return *reinterpret_cast<unsigned int*>(
                reinterpret_cast<unsigned char*>(value) + 4);
        }

        static unsigned int Type(AptValue* value)
        {
            return Flags(value) & 0x3F;
        }

        static float ToFloat(AptValue* value)
        {
            return value
                ? reinterpret_cast<ToFloatFn>(0x0077D680)(value)
                : 0.0f;
        }

        static int ToInt(AptValue* value)
        {
            return value
                ? reinterpret_cast<ToIntFn>(0x0077D5B0)(value)
                : 0;
        }

        static void Log(const char* format, ...)
        {
            char buffer[1024]{};
            va_list args;
            va_start(args, format);
            _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
            va_end(args);

            OutputDebugStringA(buffer);

            HANDLE file = CreateFileA(
                ".\\APT06.log",
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(file, buffer, static_cast<DWORD>(strlen(buffer)), &written, nullptr);
                CloseHandle(file);
            }
        }

        static void DumpProperty(AptValue* object, const char* name)
        {
            AptValue* value = GetVariable(object, name);
            if (!value)
            {
                Log("[APT06] %-13s = NULL\n", name);
                return;
            }

            Log(
                "[APT06] %-13s = %08X type=%u flags=%08X int=%d float=%.4f\n",
                name,
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(value)),
                Type(value),
                Flags(value),
                ToInt(value),
                ToFloat(value));
        }

        static void DumpRootOnce()
        {
            static bool dumped = false;
            if (dumped)
                return;

            AptValue* root = GetLevel(0);
            if (!root)
                return;

            dumped = true;

            Log("\n============================================================\n");
            Log("[APT06] ROOT INSPECTOR\n");
            Log("[APT06] _level0=%08X type=%u flags=%08X\n",
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(root)),
                Type(root),
                Flags(root));

            DumpProperty(root, "_x");
            DumpProperty(root, "_y");
            DumpProperty(root, "_xscale");
            DumpProperty(root, "_yscale");
            DumpProperty(root, "_width");
            DumpProperty(root, "_height");
            DumpProperty(root, "_rotation");
            DumpProperty(root, "_alpha");
            DumpProperty(root, "_visible");

            Log("============================================================\n\n");
        }

        static const char* StringValueText(AptValue* value)
        {
            if (!value)
                return nullptr;

            unsigned int type = Type(value);

            // AptValue::ToInt/ToFloat show string values as either a direct
            // AptString (type 1) or a wrapper (type 42) whose underlying
            // string value is at +0x1C.
            if (type == 42)
            {
                value = *reinterpret_cast<AptValue**>(
                    reinterpret_cast<unsigned char*>(value) + 0x1C);
                if (!value)
                    return nullptr;
                type = Type(value);
            }

            if (type != 1)
                return nullptr;

            void* stringData = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(value) + 8);
            if (!stringData)
                return nullptr;

            return reinterpret_cast<const char*>(
                reinterpret_cast<unsigned char*>(stringData) + 8);
        }

        static bool IsReadablePointer(const void* p, size_t size = sizeof(void*))
        {
            if (!p)
                return false;

            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
                return false;

            if (mbi.State != MEM_COMMIT ||
                (mbi.Protect & PAGE_GUARD) ||
                (mbi.Protect & PAGE_NOACCESS))
                return false;

            const uintptr_t begin = reinterpret_cast<uintptr_t>(p);
            const uintptr_t end = begin + size;
            const uintptr_t regionEnd =
                reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

            return end >= begin && end <= regionEnd;
        }

        static AptValue* ReadPtr(const void* base, size_t offset)
        {
            const unsigned char* address =
                reinterpret_cast<const unsigned char*>(base) + offset;

            if (!IsReadablePointer(address, sizeof(void*)))
                return nullptr;

            return *reinterpret_cast<AptValue* const*>(address);
        }

        static void* ReadVoidPtr(const void* base, size_t offset)
        {
            const unsigned char* address =
                reinterpret_cast<const unsigned char*>(base) + offset;

            if (!IsReadablePointer(address, sizeof(void*)))
                return nullptr;

            return *reinterpret_cast<void* const*>(address);
        }

        static void DumpCIHLine(AptValue* object, unsigned int depth)
        {
            if (!object)
                return;

            AptValue* nameValue = GetVariable(object, "_name");
            const char* objectName = StringValueText(nameValue);

            AptValue* xValue = GetVariable(object, "_x");
            AptValue* yValue = GetVariable(object, "_y");
            AptValue* widthValue = GetVariable(object, "_width");
            AptValue* heightValue = GetVariable(object, "_height");
            AptValue* visibleValue = GetVariable(object, "_visible");

            char indent[65]{};
            const unsigned int spaces = (depth * 2 < 64) ? depth * 2 : 64;
            memset(indent, ' ', spaces);
            indent[spaces] = '\0';

            Log(
                "[APT06] %sobj=%08X type=%u name=\"%s\" "
                "x=%.2f y=%.2f w=%.2f h=%.2f visible=%d\n",
                indent,
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(object)),
                Type(object),
                (objectName && *objectName) ? objectName : "<unnamed>",
                ToFloat(xValue),
                ToFloat(yValue),
                ToFloat(widthValue),
                ToFloat(heightValue),
                ToInt(visibleValue));
        }

        static unsigned int DumpDisplayListRecursive(
            AptValue* parent,
            unsigned int depth,
            unsigned int& safety)
        {
            if (!parent || depth > 16 || safety >= 4096)
                return 0;

            // For display-container CIHs, +0x48 points at type-specific
            // metadata.  The native child-management code repeatedly uses
            // metadata+0x24 as the child-list object.  Its first dword is the
            // first child CIH, and siblings are chained through CIH+0x50.
            void* metadata = ReadVoidPtr(parent, 0x48);
            if (!metadata || metadata == reinterpret_cast<void*>(0xBAADF00D))
                return 0;

            void* childList = ReadVoidPtr(metadata, 0x24);
            if (!childList)
                return 0;

            AptValue* child = ReadPtr(childList, 0x00);
            unsigned int count = 0;
            unsigned int siblingSafety = 0;

            while (child && safety < 4096 && siblingSafety++ < 1024)
            {
                if (!IsReadablePointer(child, 0x5C))
                {
                    Log("[APT06] %*sINVALID child pointer=%08X; stopping list.\n",
                        static_cast<int>(depth * 2), "",
                        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(child)));
                    break;
                }

                ++safety;
                ++count;

                DumpCIHLine(child, depth);

                const unsigned int type = Type(child);
                if (type >= 0x0C && type <= 0x13)
                    count += DumpDisplayListRecursive(child, depth + 1, safety);

                AptValue* next = ReadPtr(child, 0x50);
                if (next == child)
                {
                    Log("[APT06] %*sself-linked sibling at %08X; stopping list.\n",
                        static_cast<int>(depth * 2), "",
                        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(child)));
                    break;
                }

                child = next;
            }

            return count;
        }


        static AptValue* FindNamedDisplayObjectRecursive(
            AptValue* parent,
            const char* wantedName,
            unsigned int depth,
            unsigned int& safety)
        {
            if (!parent || !wantedName || !*wantedName || depth > 16 || safety >= 4096)
                return nullptr;

            void* metadata = ReadVoidPtr(parent, 0x48);
            if (!metadata || metadata == reinterpret_cast<void*>(0xBAADF00D))
                return nullptr;

            void* childList = ReadVoidPtr(metadata, 0x24);
            if (!childList)
                return nullptr;

            AptValue* child = ReadPtr(childList, 0x00);
            unsigned int siblingSafety = 0;

            while (child && safety < 4096 && siblingSafety++ < 1024)
            {
                if (!IsReadablePointer(child, 0x5C))
                    return nullptr;

                ++safety;

                AptValue* nameValue = GetVariable(child, "_name");
                const char* objectName = StringValueText(nameValue);

                if (objectName && _stricmp(objectName, wantedName) == 0)
                    return child;

                const unsigned int type = Type(child);
                if (type >= 0x0C && type <= 0x13)
                {
                    AptValue* found = FindNamedDisplayObjectRecursive(
                        child, wantedName, depth + 1, safety);

                    if (found)
                        return found;
                }

                AptValue* next = ReadPtr(child, 0x50);
                if (next == child)
                    break;

                child = next;
            }

            return nullptr;
        }

        static AptValue* FindNamedDisplayObject(const char* wantedName)
        {
            AptValue* root = GetLevel(0);
            if (!root)
                return nullptr;

            unsigned int safety = 0;
            return FindNamedDisplayObjectRecursive(root, wantedName, 0, safety);
        }

        static void PersistentNamedWriteTest()
        {
            static unsigned int delay = 0;
            static bool attempted = false;

            if (attempted)
                return;

            // Wait until the screen tree is fully instantiated.
            if (++delay < 150)
                return;

            AptValue* target = FindNamedDisplayObject("gBackground");
            if (!target)
                return;

            attempted = true;

            AptValue* beforeValue = GetVariable(target, "_x");
            const float before = ToFloat(beforeValue);
            const float requested = before + 40.0f;

            const bool ok = SetFloat(target, "_x", requested);

            AptValue* afterValue = GetVariable(target, "_x");
            const float after = ToFloat(afterValue);

            Log("\n============================================================\n");
            Log("[APT06] PERSISTENT NAMED WRITE TEST\n");
            Log("[APT06] target=gBackground obj=%08X oldX=%.2f requestedX=%.2f ok=%d readbackX=%.2f\n",
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(target)),
                before,
                requested,
                ok ? 1 : 0,
                after);
            Log("[APT06] This write is intentionally NOT restored.\n");
            Log("============================================================\n\n");
        }


        struct ResolutionTextRemap
        {
            const char* oldText;
            const char* newText;
        };

        static const ResolutionTextRemap kResolutionTextRemaps[] =
        {
            { "640x480x16",   "640x480"   },
            { "640x480x32",   "800x600"   },
            { "800x600x16",   "1024x768"  },
            { "800x600x32",   "1280x720"  },
            { "1024x768x16",  "1280x1024" },
            { "1024x768x32",  "1366x768"  },
            { "1280x1024x16", "1440x900"  },
            { "1280x1024x32", "1600x900"  },
            { "1600x1200x16", "1920x1080" },
            { "1600x1200x32", "2560x1440" },
        };

        static void ReleaseEAStringData(void* data)
        {
            if (!data)
                return;

            // Never attempt to free the runtime's static empty-string object.
            if (data == reinterpret_cast<void*>(0x00C65BD8))
                return;

            auto* refCount = reinterpret_cast<unsigned short*>(data);

            if (*refCount != 0)
                --(*refCount);

            if (*refCount == 0)
            {
                void* callbacks = *reinterpret_cast<void**>(0x00CE270C);
                if (callbacks)
                {
                    using FreeFn = void (__cdecl*)(void*);
                    FreeFn freeFn = *reinterpret_cast<FreeFn*>(
                        reinterpret_cast<unsigned char*>(callbacks) + 4);

                    if (freeFn)
                        freeFn(data);
                }
            }
        }

        static bool SetText(AptValue* object, const char* newText)
        {
            if (!object || !newText)
                return false;

            // Ask EA for the object's current TextField.text value.  This gives
            // us a correctly constructed AptString value that can be passed
            // straight back through AptExtObject::SetVariable.
            AptValue* value = GetVariable(object, "text");
            if (!value)
                return false;

            AptValue* stringValue = value;

            // Some APT getters can return a wrapper whose underlying AptValue
            // lives at +0x1C.
            if (Type(stringValue) == 42)
            {
                stringValue = *reinterpret_cast<AptValue**>(
                    reinterpret_cast<unsigned char*>(stringValue) + 0x1C);

                if (!stringValue)
                    return false;
            }

            if (Type(stringValue) != 1)
                return false;

            EAStringC replacement{};
            reinterpret_cast<MakeStringFn>(0x0077E750)(
                &replacement,
                newText);

            if (!replacement.data)
                return false;

            // AptString stores its EAStringC backing pointer at +8. Transfer
            // the newly-created string into the already-valid AptString value.
            void*& stringSlot = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(stringValue) + 8);

            void* oldData = stringSlot;
            stringSlot = replacement.data;
            replacement.data = nullptr; // ownership transferred to AptString

            ReleaseEAStringData(oldData);

            EAStringC property{};
            reinterpret_cast<MakeStringFn>(0x0077E750)(
                &property,
                "text");

            const bool ok =
                property.data &&
                reinterpret_cast<SetVariableFn>(0x007AB960)(
                    object,
                    &property,
                    stringValue);

            ReleaseString(property);
            return ok;
        }

        static bool NormalizeResolutionText(
            const char* input,
            char* output,
            size_t outputSize)
        {
            if (!input || !output || outputSize == 0)
                return false;

            size_t out = 0;
            bool sawDigit = false;
            bool sawX = false;

            for (const unsigned char* p =
                     reinterpret_cast<const unsigned char*>(input);
                 *p;
                 ++p)
            {
                const unsigned char c = *p;

                if (c >= '0' && c <= '9')
                {
                    if (out + 1 >= outputSize)
                        return false;

                    output[out++] = static_cast<char>(c);
                    sawDigit = true;
                    continue;
                }

                if (c == 'x' || c == 'X')
                {
                    if (out + 1 >= outputSize)
                        return false;

                    output[out++] = 'x';
                    sawX = true;
                    continue;
                }

                if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                    continue;

                // Not a plain resolution/depth label.
                return false;
            }

            output[out] = '\0';
            return sawDigit && sawX;
        }

        static const ResolutionTextRemap* FindResolutionTextRemap(
            const char* textValue)
        {
            if (!textValue || !*textValue)
                return nullptr;

            char normalized[64]{};
            if (!NormalizeResolutionText(
                    textValue,
                    normalized,
                    sizeof(normalized)))
                return nullptr;

            for (const auto& remap : kResolutionTextRemaps)
            {
                if (strcmp(normalized, remap.oldText) == 0)
                    return &remap;
            }

            return nullptr;
        }

        static unsigned int PatchResolutionTextRecursive(
            AptValue* parent,
            unsigned int depth,
            unsigned int& safety)
        {
            if (!parent || depth > 16 || safety >= 4096)
                return 0;

            void* metadata = ReadVoidPtr(parent, 0x48);
            if (!metadata || metadata == reinterpret_cast<void*>(0xBAADF00D))
                return 0;

            void* childList = ReadVoidPtr(metadata, 0x24);
            if (!childList)
                return 0;

            AptValue* child = ReadPtr(childList, 0x00);
            unsigned int siblingSafety = 0;
            unsigned int patched = 0;

            while (child && safety < 4096 && siblingSafety++ < 1024)
            {
                if (!IsReadablePointer(child, 0x5C))
                    break;

                ++safety;

                const unsigned int type = Type(child);

                if (type == 0x0F)
                {
                    AptValue* textValue = GetVariable(child, "text");
                    const char* currentText = StringValueText(textValue);

                    if (const ResolutionTextRemap* remap =
                            FindResolutionTextRemap(currentText))
                    {
                        char before[64]{};
                        strcpy_s(before, currentText);

                        const bool ok = SetText(child, remap->newText);

                        AptValue* readbackValue = GetVariable(child, "text");
                        const char* readback = StringValueText(readbackValue);

                        Log(
                            "[APT06] RESOLUTION TEXT \"%s\" -> \"%s\" "
                            "obj=%08X ok=%d readback=\"%s\"\n",
                            before,
                            remap->newText,
                            static_cast<unsigned int>(
                                reinterpret_cast<uintptr_t>(child)),
                            ok ? 1 : 0,
                            readback ? readback : "<null>");

                        if (ok)
                            ++patched;
                    }
                }

                if (type >= 0x0C && type <= 0x13)
                    patched += PatchResolutionTextRecursive(
                        child, depth + 1, safety);

                AptValue* next = ReadPtr(child, 0x50);
                if (next == child)
                    break;

                child = next;
            }

            return patched;
        }


        using TextPropertyLookupFn = void* (__cdecl*)(const char*, unsigned int);

        static int TextPropertyId(const char* name)
        {
            if (!name)
                return 0;

            auto fn = reinterpret_cast<TextPropertyLookupFn>(0x00775BB0);
            void* record = fn(name, static_cast<unsigned int>(strlen(name)));
            if (!record)
                return 0;

            return *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(record) + 4);
        }

        static const char* RawTextMetadataString(AptValue* object)
        {
            if (!object || Type(object) != 0x0F)
                return nullptr;

            void* metadata = ReadVoidPtr(object, 0x48);
            if (!metadata || metadata == reinterpret_cast<void*>(0xBAADF00D))
                return nullptr;

            void* str = ReadVoidPtr(metadata, 0x18);
            if (!str || !IsReadablePointer(str, 8))
                return nullptr;

            const unsigned short len =
                *reinterpret_cast<unsigned short*>(
                    reinterpret_cast<unsigned char*>(str) + 2);

            if (len > 1024 || !IsReadablePointer(
                    reinterpret_cast<unsigned char*>(str) + 8,
                    static_cast<size_t>(len) + 1))
                return nullptr;

            return reinterpret_cast<const char*>(
                reinterpret_cast<unsigned char*>(str) + 8);
        }

        static unsigned int ProbeResolutionCandidatesRecursive(
            AptValue* parent,
            unsigned int depth,
            unsigned int& safety,
            unsigned int& candidates)
        {
            if (!parent || depth > 16 || safety >= 4096)
                return candidates;

            void* metadata = ReadVoidPtr(parent, 0x48);
            if (!metadata || metadata == reinterpret_cast<void*>(0xBAADF00D))
                return candidates;

            void* childList = ReadVoidPtr(metadata, 0x24);
            if (!childList)
                return candidates;

            AptValue* child = ReadPtr(childList, 0x00);
            unsigned int siblingSafety = 0;

            while (child && safety < 4096 && siblingSafety++ < 1024)
            {
                if (!IsReadablePointer(child, 0x5C))
                    break;

                ++safety;

                const unsigned int type = Type(child);

                if (type == 0x0F)
                {
                    AptValue* textValue = GetVariable(child, "text");
                    const char* currentText = StringValueText(textValue);

                    char normalized[64]{};
                    if (currentText &&
                        NormalizeResolutionText(
                            currentText,
                            normalized,
                            sizeof(normalized)))
                    {
                        AptValue* nameValue = GetVariable(child, "_name");
                        const char* objectName = StringValueText(nameValue);

                        Log(
                            "[APT06] RESOLUTION CANDIDATE obj=%08X "
                            "name=\"%s\" text=\"%s\" normalized=\"%s\"\n",
                            static_cast<unsigned int>(
                                reinterpret_cast<uintptr_t>(child)),
                            objectName ? objectName : "",
                            currentText,
                            normalized);

                        ++candidates;
                    }
                }

                if (type >= 0x0C && type <= 0x13)
                    ProbeResolutionCandidatesRecursive(
                        child, depth + 1, safety, candidates);

                AptValue* next = ReadPtr(child, 0x50);
                if (next == child)
                    break;

                child = next;
            }

            return candidates;
        }

        static void ProbeTextFieldsAPT()
        {
            static unsigned int frameCounter = 0;
            static unsigned int snapshot = 0;

            // Keep running. The previous probe stopped after eight snapshots
            // even though _level0 remained the same while menus changed.
            if ((++frameCounter % 60) != 0)
                return;

            AptValue* root = GetLevel(0);
            if (!root)
                return;

            ++snapshot;

            if (snapshot == 1)
            {
                Log("\n============================================================\n");
                Log("[APT06] TEXT PROPERTY MAP\n");

                const char* candidates[] =
                {
                    "text", "htmlText", "variable", "textColor",
                    "background", "border", "autoSize",
                    "multiline", "wordWrap", "selectable",
                    "maxChars", "scroll", "hscroll"
                };

                for (const char* candidate : candidates)
                {
                    Log("[APT06] property-map \"%s\" -> id=%d\n",
                        candidate, TextPropertyId(candidate));
                }

                Log("============================================================\n\n");
            }

            unsigned int safety = 0;
            unsigned int found = 0;
            ProbeResolutionCandidatesRecursive(root, 0, safety, found);

            if (found)
            {
                Log(
                    "[APT06] RESOLUTION CANDIDATE SNAPSHOT=%u "
                    "found=%u visited=%u\n",
                    snapshot,
                    found,
                    safety);
            }
        }

        static void PatchResolutionTextsAPT()
        {
            static unsigned int frameCounter = 0;

            // The resolution screen is created on demand. Keep this lightweight
            // patch alive and check periodically so returning to the menu also
            // patches newly-created text fields.
            if ((++frameCounter % 10) != 0)
                return;

            AptValue* root = GetLevel(0);
            if (!root)
                return;

            unsigned int safety = 0;
            PatchResolutionTextRecursive(root, 0, safety);
        }

        static void DumpDisplayObjectsOnce()
        {
            static unsigned int delay = 0;
            static bool dumped = false;
            if (dumped)
                return;

            // Give the current frontend screen time to instantiate its clips.
            if (++delay < 120)
                return;

            AptValue* root = GetLevel(0);
            if (!root)
                return;

            dumped = true;

            Log("\n============================================================\n");
            Log("[APT06] LIVE DISPLAY TREE\n");
            Log("[APT06] root=%08X type=%u\n",
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(root)),
                Type(root));

            void* metadata = ReadVoidPtr(root, 0x48);
            void* childList = metadata ? ReadVoidPtr(metadata, 0x24) : nullptr;
            AptValue* firstChild = childList ? ReadPtr(childList, 0x00) : nullptr;

            Log("[APT06] root metadata=%08X childList=%08X firstChild=%08X\n",
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(metadata)),
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(childList)),
                static_cast<unsigned int>(reinterpret_cast<uintptr_t>(firstChild)));

            unsigned int safety = 0;
            const unsigned int total = DumpDisplayListRecursive(root, 0, safety);

            Log("[APT06] tree_objects=%u safety=%u\n", total, safety);
            Log("============================================================\n\n");
        }

    }

    // -------------------------------------------------------------------------
    // NBA Live 06 localized resolution-label override
    //
    // Proven path:
    //   sub_555890("$TOKEN")
    //      -> sub_41B710(out BBallWString*, token)
    //      -> Localization singleton @ 0x00CAEB14
    //      -> sub_41A430(this, out, token)
    //
    // sub_41A430 returns the localized UTF-16 string in the BBallWString
    // wrapper passed as arg0.  We preserve the native lookup completely and
    // only replace the ten stock resolution labels after localization.
    //
    // This avoids:
    //   - process-wide memory scanning
    //   - APT TextField traversal
    //   - editing locale.eng on disk
    // -------------------------------------------------------------------------

    struct LocalizedResolutionRemap06
    {
        const wchar_t* oldText;
        const char* newText;
    };

    static const LocalizedResolutionRemap06 kLocalizedResolutionRemaps06[] =
    {
        { L"640x480x16",   "640x480"   },
        { L"640x480x32",   "800x600"   },
        { L"800x600x16",   "1024x768"  },
        { L"800x600x32",   "1280x720"  },
        { L"1024x768x16",  "1280x1024" },
        { L"1024x768x32",  "1366x768"  },
        { L"1280x1024x16", "1440x900"  },
        { L"1280x1024x32", "1600x900"  },
        { L"1600x1200x16", "1920x1080" },
        { L"1600x1200x32", "2560x1440" },
    };

    static void* __cdecl LocalizeStringHook06(void* outString, const char* token)
    {
        // Reproduce sub_41B710 exactly first.
        void* localization =
            *reinterpret_cast<void**>(0x00CAEB14);

        using NativeLookupFn =
            void* (__thiscall*)(void*, void*, const char*);

        reinterpret_cast<NativeLookupFn>(0x0041A430)(
            localization,
            outString,
            token);

        if (!outString)
            return outString;

        const wchar_t* localized =
            *reinterpret_cast<const wchar_t**>(outString);

        if (!localized)
            return outString;

        for (const auto& remap : kLocalizedResolutionRemaps06)
        {
            if (std::wcscmp(localized, remap.oldText) != 0)
                continue;

            // sub_41A020 is the native BBallWString UTF-8 assignment helper.
            // It replaces the wrapper's current shared UTF-16 string and
            // preserves the game's own allocation/refcount behavior.
            using AssignUtf8Fn =
                void (__thiscall*)(void*, const char*);

            reinterpret_cast<AssignUtf8Fn>(0x0041A020)(
                outString,
                remap.newText);

            apt06::Log(
                "[LOC06] resolution label token=\"%s\" -> \"%s\"\n",
                token ? token : "<null>",
                remap.newText);

            break;
        }

        return outString;
    }

    // -------------------------------------------------------------------------
    // APT frontend timing probe
    //
    // 0x7BA230 is a public one-argument thunk that jumps directly to 0x7BA130.
    // Static analysis proves 0x7BA130 forwards arg0 through EAX to 0x7B9D40,
    // where it is accumulated against the movie/timeline step threshold.
    //
    // This probe preserves the native update exactly, but records the incoming
    // delta/tick values and call frequency once per wall-clock second.
    // -------------------------------------------------------------------------

    static void __cdecl APT_UpdateNormalized06(int delta)
    {
        // The FE loop can run at thousands of iterations/sec when presentation
        // is uncapped.  APT receives delta=1 on every iteration, so its movie
        // timeline advances thousands of logical ticks/sec.
        //
        // Keep explicit multi-tick advances (>1) intact, but rate-limit the
        // ordinary delta=1 timeline tick to a fixed real-time cadence.
        //
        // 60 is the first test target.  If stock-feel animation is still too
        // fast, change this to 30 and compare.
        static constexpr double kFrontendTimelineHz = 60.0;

        using NativeAptUpdateFn = void (__cdecl*)(int);
        auto NativeUpdate =
            reinterpret_cast<NativeAptUpdateFn>(0x007BA130);

        if (delta != 1)
        {
            NativeUpdate(delta);
            return;
        }

        static LARGE_INTEGER frequency{};
        static LARGE_INTEGER lastTick{};
        static bool initialized = false;

        LARGE_INTEGER now{};

        if (!initialized)
        {
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&lastTick);
            initialized = true;

            // Preserve the first logical tick immediately.
            NativeUpdate(1);
            return;
        }

        QueryPerformanceCounter(&now);

        if (frequency.QuadPart <= 0)
        {
            NativeUpdate(1);
            lastTick = now;
            return;
        }

        const double elapsedSeconds =
            static_cast<double>(now.QuadPart - lastTick.QuadPart) /
            static_cast<double>(frequency.QuadPart);

        const double intervalSeconds = 1.0 / kFrontendTimelineHz;

        if (elapsedSeconds < intervalSeconds)
            return;

        // Advance exactly one APT logical tick.  Do not "catch up" thousands
        // of calls: the runaway input itself is what made the FE too fast.
        NativeUpdate(1);

        // Move forward by the ideal interval rather than snapping to now.
        // If the process stalls badly, reset instead of generating a burst.
        const LONGLONG intervalCounts =
            static_cast<LONGLONG>(
                static_cast<double>(frequency.QuadPart) /
                kFrontendTimelineHz);

        if (intervalCounts > 0 &&
            (now.QuadPart - lastTick.QuadPart) <
                (intervalCounts * 4))
        {
            lastTick.QuadPart += intervalCounts;
        }
        else
        {
            lastTick = now;
        }
    }

    static void PatchResolutionMenuStrings06();

    // Function that scales UI components properly for widescreen
    DWORD METHOD FEAptInterface_Render(DWORD* t, DUMMY_ARG, char a1, int a2)
    {
        // Native APT inspection. The previous +80 render-space proof has
        // been removed; rendering is back at its normal position.

        // Resolution labels are sourced through localization. Do not scan
        // process memory and do not patch APT TextFields here.
        // PatchResolutionMenuStrings06();

        int v3;
        float v4[17];
        float v5;
        char v6[64];
        float invX;
        float invY;
        float v7;
        float v9;
        float v10;

        v3 = CallAndReturn<int, 0x7D0D10>(); // 480
        v5 = CallAndReturn<int, 0x7D0D40>(); // 640
        invX = (double)v3 * 0.0015625f; // 640
        invY = (double)v5 * 0.0020833334f; // 480
        v10 = -0.5f;
        if (v3 / v5 > 1.3333334f)
        {
            v9 = invY;
            invX = (double)v3 * 0.0011709601f;
            v10 = 107.0 * invX - 0.5;
        }
        v4[0] = invX;
        v4[5] = invY;
        memset(&v4[1], 0, 16);
        memset(&v4[6], 0, 16);
        v4[10] = 1.0f;
        v4[11] = 0.0f;
        v4[12] = v10;
        v4[13] = -0.5f;
        v4[14] = -100.0f;
        v4[15] = 1.0f; // scale, it is scaled by 1/v4[15]
        void* camera = *raw_ptr<void*>(*(void**)0xCB2A6C, 28);
        CallMethod<0x6FBF14>(camera, &v3, v6);
        CallMethod<0x702731>(camera, &v3, v4);
        if (a1)
        {
            CallMethod<0x702684>(camera, &v3, 0xFFFFFFFF, 1);
        }
        Call<0x7ABA00>();
        return CallMethodAndReturn<DWORD, 0x702731>(camera, &v3, v6);
    }

    // Function that scales mouse position properly for widescreen
    int METHOD BroadcastMouseInput(int* _this)
    {
        int result; // eax
        int* v3; // edi
        int v4; // ebx
        int v5; // ebx
        int q6; // ebp
        int v7; // [esp+14h] [ebp-18h]
        int v8; // [esp+24h] [ebp-8h]
        int v12;

        int* pMouseDevice = *raw_ptr<int*>(_this, 0xC);
        result = *pMouseDevice;

        if (pMouseDevice)
        {
            v3 = CallAndReturn<int*, 0x624AC0>();
            v4 = CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 3); // pMouseDevice->PeekInput(3)
            v12 = CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 4);
            int* pBroadcast = *raw_ptr<int*>(v3, 0x4);
            int height = CallAndReturn<int, 0x7D0D40>();
            int width = CallAndReturn<int, 0x7D0D10>();
            double fWidth = (double)width;
            double fHeight = (double)height;
            if ((fWidth / fHeight) > 1.3333334f)
            {
                v7 = (int)(fWidth * 0.0011709601f * 107.0f);
                width -= 2 * v7;
                v4 -= v7;
            }
            v5 = 640 * v4 / width;
            v8 = 480 * v12 / height;
            if (CallVirtualMethodAndReturn<int, 14>(pMouseDevice, 1))
            {
                CallVirtualMethod<1>(v3, 130, 0, v5, v8, 0);
            }
            else if (CallVirtualMethodAndReturn<int, 15>(pMouseDevice, 1))
            {
                CallMethod<0x620D10>(v3, 131, 0, v5, v8, 0);
            }
            if (CallVirtualMethodAndReturn<int, 14>(pMouseDevice, 0))
            {
                CallVirtualMethod<1>(v3, 132, 0, v5, v8, 0);
            }
            else if (CallVirtualMethodAndReturn<int, 15>(pMouseDevice, 0))
            {
                CallMethod<0x620D10>(v3, 133, 0, v5, v8, 0);
            }
            q6 = CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 0) != 0;
            if (CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 2))
                q6 |= 4u;
            if (CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 1))
                q6 |= 2u;
            return CallVirtualMethodAndReturn<int, 1>(v3, 128, 0, v5, v8, q6);
        }
        return result;
    }
    // Loadbar
    float loadXPos = ((212.0f / 640.0f) * (RES_Y * 1.33333f)) + ((RES_X - (RES_Y * 1.33333f)) / 2);
    float loadYPos = ((397.0f / 480.0f) * RES_Y);
    float loadWidth = 256.0f * (RES_Y / 480.0f);
    float loadHeight = 64.0f * (RES_Y / 480.0f);
    // Movies
    float CalculateAspectRatio() {

        // Check if aspect ratio is lower than 4:3 (1.333333f)
        if (ASPECT_RATIO < 1.333334f) {
            // If aspect ratio is lower, multiply 1.0 with (x / 640)
            return 1.0f * (RES_X / 640.0f);
        }
        // Check if aspect ratio is higher than 4:3 but lower than 16:9 (1.777777f)
        else if (ASPECT_RATIO > 1.333334f && ASPECT_RATIO <= 1.777777f) {
            // If aspect ratio is higher but lower, multiply 1.0 with (x / 640)
            return 1.0f * (RES_Y / 1088.0f);
        }
        // Check if aspect ratio is higher than 16:9
        else {
            // If aspect ratio is higher, multiply 1.0 with (y / 480)
            return 1.0f * (RES_X / 1920.0f);
        }
    }
    // CreationZone
    int czXPos = (int)(((362.0f / 640.0f) * (RES_Y * 1.33333f)) + ((RES_X - (RES_Y * 1.33333f)) / 2));
    int czYPos = (int)(((74.0f / 480.0f) * RES_Y));
    int czWidth = (int)((225.0f * (RES_Y / 480.0f)));
    int czHeight = (int)((303.0f * (RES_Y / 480.0f)));


    // -------------------------------------------------------------------------
    // NBA Live 06 runtime resolution-menu labels
    //
    // EA's frontend copies the stock labels into transient allocations. The
    // old implementation walked *all* writable private memory and called
    // memcmp() directly on those regions every render frame. VirtualQuery
    // is only a point-in-time observation: allocations can be freed between
    // the query and the compare. This caused the ASI memcmp access violation.
    //
    // This compatibility implementation does not directly dereference any
    // inspected game address. ReadProcessMemory takes a bounded snapshot;
    // WriteProcessMemory performs the conditional replacement. Both can fail
    // without an access violation in our scanner. Each pass checks all ten
    // labels once, at a throttled interval, and remembers individual matches.
    //
    // This remains a heuristic scan, not a targeted frontend string hook.
    // -------------------------------------------------------------------------

    struct ResolutionStringRemap06
    {
        const char* oldText;
        const char* newText;
    };

    static const ResolutionStringRemap06 kResolutionStringRemaps06[] =
    {
        { "640x480x16",   "640x480"   },
        { "640x480x32",   "800x600"   },
        { "800x600x16",   "1024x768"  },
        { "800x600x32",   "1280x720"  },
        { "1024x768x16",  "1280x1024" },
        { "1024x768x32",  "1366x768"  },
        { "1280x1024x16", "1440x900"  },
        { "1280x1024x32", "1600x900"  },
        { "1600x1200x16", "1920x1080" },
        { "1600x1200x32", "2560x1440" },
    };

    static constexpr size_t kResolutionRemapCount06 =
        sizeof(kResolutionStringRemaps06) / sizeof(kResolutionStringRemaps06[0]);
    static constexpr size_t kResolutionScanChunk06 = 64 * 1024;
    static constexpr size_t kResolutionScanOverlap06 = 32;

    static bool IsWritableFrontendMemory06(DWORD protect)
    {
        if ((protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;

        switch (protect & 0xFF)
        {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    }

    static bool TryReplaceResolutionString06(
        HANDLE process, uintptr_t address,
        const ResolutionStringRemap06& remap,
        size_t oldLen, size_t newLen)
    {
        // Re-read immediately before writing. The scanned snapshot may be
        // stale, and the original allocation might no longer exist.
        char current[32] = {};
        SIZE_T bytesRead = 0;
        if (oldLen + 1 > sizeof(current) ||
            !ReadProcessMemory(process,
                reinterpret_cast<LPCVOID>(address),
                current, oldLen + 1, &bytesRead) ||
            bytesRead != oldLen + 1 ||
            memcmp(current, remap.oldText, oldLen) != 0 ||
            current[oldLen] != '\0')
            return false;

        char replacement[32] = {};
        memcpy(replacement, remap.newText, newLen);
        SIZE_T bytesWritten = 0;
        return WriteProcessMemory(process,
            reinterpret_cast<LPVOID>(address),
            replacement, oldLen + 1, &bytesWritten) != FALSE &&
            bytesWritten == oldLen + 1;
    }

    static void ScanResolutionMenuStrings06(unsigned int& foundMask)
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const uintptr_t maximum =
            reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        uintptr_t address =
            reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        HANDLE process = GetCurrentProcess();

        // Static scratch buffer avoids large per-frame stack allocations.
        static unsigned char snapshot[kResolutionScanChunk06];
        size_t oldLengths[kResolutionRemapCount06] = {};
        size_t newLengths[kResolutionRemapCount06] = {};
        for (size_t i = 0; i < kResolutionRemapCount06; ++i)
        {
            oldLengths[i] = strlen(kResolutionStringRemaps06[i].oldText);
            newLengths[i] = strlen(kResolutionStringRemaps06[i].newText);
        }

        while (address < maximum)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(address),
                &mbi, sizeof(mbi)) != sizeof(mbi))
                break;

            const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const SIZE_T regionSize = mbi.RegionSize;
            if (regionSize == 0 || base > UINTPTR_MAX - regionSize)
                break;
            const uintptr_t next = base + regionSize;
            if (next <= address)
                break;

            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                IsWritableFrontendMemory06(mbi.Protect))
            {
                uintptr_t cursor = base;
                while (cursor < next)
                {
                    const size_t bytesToRead = static_cast<size_t>(
                        (next - cursor < kResolutionScanChunk06) ?
                        next - cursor : kResolutionScanChunk06);
                    SIZE_T bytesRead = 0;
                    if (ReadProcessMemory(process,
                        reinterpret_cast<LPCVOID>(cursor),
                        snapshot, bytesToRead, &bytesRead) &&
                        bytesRead == bytesToRead)
                    {
                        for (size_t offset = 0; offset < bytesRead; ++offset)
                        {
                            // Every old string begins with a digit.
                            if (snapshot[offset] < '0' || snapshot[offset] > '9')
                                continue;

                            for (size_t i = 0; i < kResolutionRemapCount06; ++i)
                            {
                                const size_t oldLen = oldLengths[i];
                                if (oldLen == 0 || newLengths[i] > oldLen ||
                                    oldLen + 1 > bytesRead - offset)
                                    continue;
                                const auto& remap = kResolutionStringRemaps06[i];
                                if (snapshot[offset + oldLen] != 0 ||
                                    memcmp(snapshot + offset, remap.oldText, oldLen) != 0)
                                    continue;
                                if (TryReplaceResolutionString06(process,
                                    cursor + offset, remap, oldLen, newLengths[i]))
                                    foundMask |= (1u << i);
                                break;
                            }
                        }
                    }

                    // Overlap catches strings crossing a chunk boundary.
                    if (bytesToRead == next - cursor)
                        break;
                    cursor += bytesToRead - kResolutionScanOverlap06;
                }
            }
            address = next;
        }
    }

    static void PatchResolutionMenuStrings06()
    {
        static unsigned int foundMask = 0;
        static unsigned int frameCounter = 0;
        static bool completed = false;
        if (completed)
            return;

        // Do not scan during the earliest frontend frames. Scanning every
        // render call was costly and increased startup/focus race exposure.
        // Once started, retry approximately every 120 render calls.
        if (++frameCounter < 120)
            return;
        frameCounter = 0;

        ScanResolutionMenuStrings06(foundMask);
        const unsigned int completeMask =
            (1u << kResolutionRemapCount06) - 1u;
        if ((foundMask & completeMask) == completeMask)
            completed = true;
    }

    // Legacy compatibility scanner retained below for reference only.
    // Production resolution labels now use LocalizeStringHook06.

}

void Install_LIVE06() {
    using namespace live06;
    // Change aspect ratio in-game
    patch::RedirectJump(0x70221E, SetPerspectiveProjection06);
    patch::RedirectJump(0x73E3C0, SetTestInConicalFrustum06);
    // High-resolution APT/RealFont rendering
    InstallHighResolutionAptFonts06();

    // Localized resolution labels. Hook the native localization wrapper
    // instead of scanning process memory or patching APT TextFields.
    patch::RedirectJump(0x0041B710, LocalizeStringHook06);

    // Normalize the APT logical timeline when the FE loop is uncapped.
    patch::RedirectJump(0x007BA230, APT_UpdateNormalized06);
    // Sets resolutions
    for (const auto& resolution : ids) {
        patch::SetUInt(0xC4CF38 + 20 * resolution.id + 4, resolution.width);
        patch::SetUInt(0xC4CF38 + 20 * resolution.id + 8, resolution.height);
        patch::SetUInt(0xC4CF38 + 20 * resolution.id + 12, resolution.depth);
    }
    // Changes size of windows
    patch::SetUInt(0x414C6C + 1, RES_Y);
    patch::SetUInt(0x414C71 + 1, RES_X);
    patch::SetUInt(0x41EAB0 + 1, RES_Y);
    patch::SetUInt(0x41EAB5 + 1, RES_X);
    patch::SetUInt(0x600216 + 1, RES_Y);
    patch::SetUInt(0x60021B + 1, RES_X);
    patch::SetUInt(0x606685 + 1, RES_Y);
    patch::SetUInt(0x60668A + 1, RES_X);
    patch::SetUInt(0x6F2EC5 + 1, RES_Y);
    patch::SetUInt(0x6F2ECA + 1, RES_X);
    patch::SetUInt(0x755E48 + 1, RES_Y);
    patch::SetUInt(0x755E4D + 1, RES_X);
    patch::SetUInt(0x7D1D6D + 1, RES_Y);
    patch::SetUInt(0x7D1D72 + 1, RES_X);
    patch::SetUInt(0x7D1DA2 + 1, RES_Y);
    patch::SetUInt(0x7D1DA7 + 1, RES_X);
    patch::SetUInt(0xC4CEDC, RES_X);
    patch::SetUInt(0xC4CEE0, RES_Y);
    // UI adjustment
    patch::RedirectJump(0x4BC4F0, FEAptInterface_Render);
    patch::RedirectJump(0x626600, BroadcastMouseInput);
    // loadbar
    patch::SetFloat(0xB83778 + 6, loadXPos);
    patch::SetFloat(0xB837B8 + 6, loadYPos);
    patch::SetFloat(0xB837F8 + 6, loadWidth);
    patch::SetFloat(0xB83838 + 6, loadHeight);
    // Movies
    if (INTRO == 1) {
        if (ASPECT_RATIO < 1.333334f) {
            patch::SetPointer(0x560A23 + 1, "EABRAND");
            patch::SetPointer(0x560CA1 + 1, "INTRO");
            patch::SetPointer(0x560C96 + 1, "INTRO");
            patch::SetPointer(0x598BDB + 1, "INTRO");
            patch::SetPointer(0x598BE2 + 1, "INTRO");
        }
        else {
            patch::SetPointer(0x560A23 + 1, "EABRANDWS");
            patch::SetPointer(0x560CA1 + 1, "INTROWS");
            patch::SetPointer(0x560C96 + 1, "INTROWS");
            patch::SetPointer(0x598BDB + 1, "INTROWS");
            patch::SetPointer(0x598BE2 + 1, "INTROWS");
        }
    }
    else {
        patch::SetPointer(0x560A23 + 1, "DUMMY");
        patch::SetPointer(0x560CA1 + 1, "DUMMY");
        patch::SetPointer(0x560C96 + 1, "DUMMY");
        patch::SetPointer(0x598BDB + 1, "DUMMY");
        patch::SetPointer(0x598BE2 + 1, "DUMMY");
    }
    patch::SetFloat(0x6364F4 + 1, CalculateAspectRatio());
    // CreateZone
    patch::SetUInt(0xB851C8 + 6, czXPos);
    patch::SetUInt(0xB85208 + 6, czYPos);
    patch::SetUInt(0xB85248 + 6, czWidth);
    patch::SetUInt(0xB85288 + 6, czHeight);

    // Switch UI files
    std::string pathA = ".\\assets\\06WSUI"; // Replace with the actual path 'a'
    std::string pathB = ".\\sgsm"; // Replace with the actual path 'b'
    adjustWSUI::adjustWidescreenUI(ASPECT_RATIO, pathA, pathB);

}
