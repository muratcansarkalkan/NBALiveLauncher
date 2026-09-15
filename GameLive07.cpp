#include "plugin-std.h"
#include "Resolutions.h"

using namespace plugin;

const unsigned int RES_X = GetPrivateProfileIntW(L"DISPLAY", L"RES_X", 640, L".\\main.ini");
const unsigned int RES_Y = GetPrivateProfileIntW(L"DISPLAY", L"RES_Y", 480, L".\\main.ini");
const unsigned int INTRO = GetPrivateProfileIntW(L"BOOTUP", L"INTRO", 1, L".\\main.ini");
const float ASPECT_RATIO = static_cast<float>(RES_X) / static_cast<float>(RES_Y);
const float ASPECT_DIFF = ASPECT_RATIO / 1.33333f;
static float gAspectScale07 = 1.0f;

namespace live07 {

    ResolutionID ids[] = {
        { 640,  480, 32, 0 }, // 640x480x16
        { RES_X,  RES_Y, 32, 1 }, // 640x480x32
        { 1024,  768, 32, 2 }, // 800x600x16
        { 1280,  720, 32, 3 }, // 800x600x32
        { 1280, 1024, 32, 4 },
        { 1366,  768, 32, 5 },
        { 1440,  900, 32, 6 },
        { 1600,  900, 32, 7 },
        { 1920, 1080, 32, 8 },
        { 2560, 1440, 32, 9 },
    };

    DWORD* METHOD SetPerspectiveProjection07(
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

            gAspectScale07 =
                aspectRatio / (4.0f / 3.0f);

            if (gAspectScale07 < 1.0f)
                gAspectScale07 = 1.0f;
        }
        else
        {
            gAspectScale07 = 1.0f;
        }

        _this[15] = 0.0f;
        _this[6] = fovY;
        _this[7] = aspectRatio;
        _this[8] = nearClip;
        _this[9] = farClip;

        memcpy(_this + 36, (void*)0xBBC7B0, 0x40u);

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

        CallMethod<0x439FEA>(_this, a2);

        return a2;
    }

    // NBA Live 07
    int SetTestInConicalFrustum07(
        float* a1,
        DUMMY_ARG,
        float radius,
        bool cameraclip)
    {
        const float dx =
            a1[0] - patch::GetFloat(0xCF0614);

        const float dy =
            a1[1] - patch::GetFloat(0xCF0618);

        const float dz =
            a1[2] - patch::GetFloat(0xCF061C);

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
            dx * patch::GetFloat(0xCF0620) +
            dy * patch::GetFloat(0xCF0624) +
            testZ * patch::GetFloat(0xCF0628)
            );

        if (patch::GetFloat(0xCF0604) - radius <= depth &&
            depth <= patch::GetFloat(0xCF0608) + radius)
        {
            const float originalConeSine =
                patch::GetFloat(0xCF060C);

            const float originalConeInvCosine =
                patch::GetFloat(0xCF0610);

            const float originalTan =
                originalConeSine *
                originalConeInvCosine;

            /*
                07's previous culling test used:

                    v5² > v12 * 0.33333

                Equivalent expansion:

                    1 / sqrt(0.33333)
                    ~= 1.73206

                Preserve that baseline and then add the
                widescreen aspect expansion.
            */
            constexpr float BASE_CULL_SCALE_07 =
                1.73205948f;

            const float widenedTan =
                originalTan *
                BASE_CULL_SCALE_07 *
                gAspectScale07;

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

    DWORD METHOD FEAptInterface_Render(DWORD* t, DUMMY_ARG, char a1, int a2)
    {
        int v3;
        float v4[17];
        float v5;
        char v6[64];
        float invX;
        float invY;
        float v7;
        float v9;
        float v10;

        DWORD* getResolution = (DWORD*)patch::GetPointer(0xD42568);
        DWORD* address = (DWORD*)patch::GetPointer(0xD42584);
        v3 = CallMethodAndReturn<int, 0x435953>((void*)getResolution);
        v5 = CallMethodAndReturn<int, 0x435956>(getResolution);
        invX = (double)v3 * 0.0015625f;
        invY = (double)v5 * 0.0020833334f;
        v10 = -0.5f;
        if (v3 / v5 > 1.3333334f)
        {
            v9 = invY;
            invX = (double)v3 * 0.0011709601f;
            v10 = 107.0f * invX - 0.5f;
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
        v4[15] = 1.0f;
        memcpy(v6, (const void*)(address + 80), sizeof(v6));;
        CallMethod<0x43A472>(address, &v3, v6);
        CallMethod<0x439024>(address, &v3, v4);
        if (a1)
        {
            v3 = -1;
            CallMethod<0x438F77>(address, &v3, 0xFFFFFFFF, 1);
        }
        Call<0x88B440>();
        return CallMethodAndReturn<DWORD, 0x439024>(address, &v3, v6);
    }

    __declspec(naked) void Live07MovieFilter()
    {
        __asm
        {
            // Original instruction
            mov ecx, [esp + 8]

            // EAX = movie ID returned by sub_501110.
            cmp eax, 1
            je skipMovie

            // Original movie playback.
            push eax
            mov edx, 0x0055C430
            call edx

            skipMovie :
            ret
        }
    }

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

        DWORD* getResolution = (DWORD*)patch::GetPointer(0xD42568);

        int* pMouseDevice = *raw_ptr<int*>(_this, 0xC);
        result = *pMouseDevice;

        if (pMouseDevice)
        {
            v3 = CallAndReturn<int*, 0x65C090>();
            v4 = CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 3);
            v12 = CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 4);
            int* pBroadcast = *raw_ptr<int*>(v3, 0x4);
            int height = CallMethodAndReturn<int, 0x435956>(getResolution);
            int width = CallMethodAndReturn<int, 0x435953>(getResolution);
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
                CallVirtualMethod<1>(v3, 133, 0, v5, v8, 0);
            }
            else if (CallVirtualMethodAndReturn<int, 15>(pMouseDevice, 1))
            {
                CallMethod<0x657D30>(v3, 134, 0, v5, v8, 0);
            }
            if (CallVirtualMethodAndReturn<int, 14>(pMouseDevice, 0))
            {
                CallVirtualMethod<1>(v3, 135, 0, v5, v8, 0);
            }
            else if (CallVirtualMethodAndReturn<int, 15>(pMouseDevice, 0))
            {
                CallMethod<0x657D30>(v3, 136, 0, v5, v8, 0);
            }
            q6 = CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 0) != 0;
            if (CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 2))
                q6 |= 4u;
            if (CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 1))
                q6 |= 2u;
            return CallVirtualMethodAndReturn<int, 1>(v3, 131, 0, v5, v8, q6);
        }
        return result;
    }
    // loadbar
    float loadXStart = ((66.0f / 640.0f) * (RES_Y * 1.33333f)) + ((RES_X - (RES_Y * 1.33333f)) / 2);
    float loadYStart = ((340.0f / 480.0f) * RES_Y);
    float loadXEnd = ((130.0f / 640.0f) * (RES_Y * 1.33333f)) + ((RES_X - (RES_Y * 1.33333f)) / 2);
    float loadYEnd = ((404.0f / 480.0f) * RES_Y);
    // CreationZone
    int czXPos = (int)(((362.0f / 640.0f) * (RES_Y * 1.33333f)) + ((RES_X - (RES_Y * 1.33333f)) / 2));
    int czYPos = (int)(((74.0f / 480.0f) * RES_Y));
    int czWidth = (int)((225.0f * (RES_Y / 480.0f)));
    int czHeight = (int)((303.0f * (RES_Y / 480.0f)));

}

void Install_LIVE07() {
    using namespace live07;
    patch::RedirectJump(0x438B11, SetPerspectiveProjection07);
    patch::RedirectJump(0x67BE80, SetTestInConicalFrustum07);
    for (const auto& resolution : ids) {
        patch::SetUInt(0xC65CA0 + 20 * resolution.id + 4, resolution.width);
        patch::SetUInt(0xC65CA0 + 20 * resolution.id + 8, resolution.height);
        patch::SetUInt(0xC65CA0 + 20 * resolution.id + 12, resolution.depth);
    }
    patch::RedirectJump(0x4EAFA0, FEAptInterface_Render);
    patch::RedirectJump(0x65D820, BroadcastMouseInput);
    patch::SetUInt(0x414CFC + 1, RES_Y);
    patch::SetUInt(0x414D01 + 1, RES_X);
    patch::SetUInt(0x41E310 + 1, RES_Y);
    patch::SetUInt(0x41E315 + 1, RES_X);
    patch::SetUInt(0x42C47D + 1, RES_Y);
    patch::SetUInt(0x42C482 + 1, RES_X);
    patch::SetUInt(0x64AB40 + 1, RES_Y);
    patch::SetUInt(0x64AB45 + 1, RES_X);
    patch::SetUInt(0x9D91C2 + 1, RES_Y);
    patch::SetUInt(0x9D91C7 + 1, RES_X);
    // loadbar
    patch::SetFloat(0x63F932 + 4, loadXStart);
    patch::SetFloat(0x63F962 + 4, loadXStart);
    patch::SetFloat(0x63F93A + 4, loadYStart);
    patch::SetFloat(0x63F952 + 4, loadYStart);
    patch::SetFloat(0x63F94A + 4, loadXEnd);
    patch::SetFloat(0x63F97A + 4, loadXEnd);
    patch::SetFloat(0x63F96A + 4, loadYEnd);
    patch::SetFloat(0x63F982 + 4, loadYEnd);
    // enable/disable intro
    if (INTRO != 1) {
        patch::RedirectJump(0x005650AB, Live07MovieFilter);
        patch::SetPointer(0x560A23 + 1, "DUMMY");
        //patch::SetPointer(0x560C96 + 1, "DUMMY");
    }
    // CreateZone
    patch::SetUInt(0xC65D68, czXPos);
    patch::SetUInt(0xC65D6C, czYPos);
    patch::SetUInt(0xC65D70, czWidth);
    patch::SetUInt(0xC65D74, czHeight);

}
