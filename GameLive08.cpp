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

    ResolutionID ids[] = {
        { 640,  480, 32, 0 }, // 640x480x16
        { RES_X,  RES_Y, 32, 1 }, // 640x480x32
        { 1024,  768, 32, 2 }, // 800x600x16
        { 1280,  720, 32, 3 }, // 800x600x32
        { 1280, 1024, 32, 4 },
        { 1366, 768, 32, 5 },
        { 1440,  900, 32, 6 },
        { 1600,  900, 32, 7 },
        { 1600, 1200, 32, 8 },
        { 1680, 1050, 32, 9 },
        { 1920, 1080, 32, 10 },
        { 2560, 1440, 32, 11 },
        { 3440, 1440, 32, 12 }, // 640x480x16
        { 3840, 1080, 32, 13 }, // 640x480x32
        { 3840, 1200, 32, 14 }, // 800x600x16
        { 3840, 1600, 32, 15 }, // 800x600x32
    };

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

                    coneRadius² > radialDistance² * 0.3

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
