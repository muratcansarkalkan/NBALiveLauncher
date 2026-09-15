#include "plugin-std.h"
#include "Resolutions.h"
#include "AdjustWSUI.h"

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
                sec(theta) = sqrt(1 + tan²(theta))
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
                        0.5² = 0.25
                    */
                    return 1;
                }

                if (a2a >= radius)
                    return 1;
            }
        }

        return 0;
    }

    // Function that scales UI components properly for widescreen
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

}

void Install_LIVE06() {
    using namespace live06;
    // Change aspect ratio in-game
    patch::RedirectJump(0x70221E, SetPerspectiveProjection06);
    patch::RedirectJump(0x73E3C0, SetTestInConicalFrustum06);
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
