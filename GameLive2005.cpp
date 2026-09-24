#include "plugin-std.h"
#include "Resolutions.h"
#include "AdjustWSUI.h"

using namespace plugin;

// Call GetPrivateProfileInt to retrieve the integer value
const unsigned int RES_X = GetPrivateProfileIntW(L"DISPLAY", L"RES_X", 640, L".\\main.ini");
const unsigned int RES_Y = GetPrivateProfileIntW(L"DISPLAY", L"RES_Y", 480, L".\\main.ini");
const unsigned int INTRO = GetPrivateProfileIntW(L"BOOTUP", L"INTRO", 1, L".\\main.ini");
const float ASPECT_RATIO = static_cast<float>(RES_X) / static_cast<float>(RES_Y);

namespace live2005 {

    ResolutionID ids[] = {
    { 640,  480, 32, 0 }, // 640x480x16
    { 800,  600, 32, 1 }, // 640x480x32
    { 1024,  768, 32, 2 }, // 800x600x16
    { 1280,  720, 32, 3 }, // 800x600x32
    { 1280, 1024, 32, 4 }, // 1024x768x16
    { 1366,  768, 32, 5 }, // 1024x768x32
    { 1440,  900, 32, 6 }, // 1280x1024x16
    { 1600,  900, 32, 7 }, // 1280x1024x32
    { 1920, 1080, 32, 8 }, // 1600x1200x16
    { 2560, 1440, 32, 9 }, // 1600x1200x32
    };

    DWORD* METHOD SetPerspectiveProjection05(
        float* _this, DUMMY_ARG,
        DWORD* a2,
        float fovY,
        float aspectRatio,
        float nearClip,
        float farClip)
    {
        const float width = static_cast<float>(*((unsigned int*)_this + 2));
        const float height = static_cast<float>(*((unsigned int*)_this + 3));

        if (height > 0.0f)
            aspectRatio = width / height;

        _this[15] = 0.0f;        // mGuardBandScale
        _this[6] = fovY;        // mFrustum.mFov
        _this[7] = aspectRatio; // mFrustum.mAspect
        _this[8] = nearClip;    // mFrustum.mNearPlane
        _this[9] = farClip;     // mFrustum.mFarPlane

        memcpy(_this + 36, (void*)0xB837F0, 0x40u);

        const double fovR =
            1.0 / tan(static_cast<double>(fovY) * 0.0087266462);

        _this[47] = -1.0f;

        _this[36] = static_cast<float>(
            0.5 * width / aspectRatio * fovR);

        _this[41] = static_cast<float>(
            -(height * 0.5 * fovR));

        _this[44] = static_cast<float>(
            width * -0.5 - static_cast<double>(*(int*)_this));

        _this[45] = static_cast<float>(
            height * -0.5 - static_cast<double>(*(int*)_this + 1));

        const double dist =
            static_cast<double>(nearClip) - static_cast<double>(farClip);

        _this[46] = static_cast<float>(
            static_cast<double>(nearClip) / dist - 1.0);

        _this[50] = static_cast<float>(
            static_cast<double>(nearClip) * static_cast<double>(farClip) / dist);

        CallMethod<0x6ED4F4>(_this, a2);

        return a2;
    }

    bool METHOD SetTestInConicalFrustum(
        float* _this, DUMMY_ARG,
        float* a2,
        float radius,
        bool cameraclip)
    {
        float dx = a2[0] - _this[4];
        float dy = a2[1] - _this[5];
        float dz = a2[2] - _this[6];

        float testZ = dz;

        if (cameraclip && radius > 2.0f && radius < 10.0f)
            testZ += radius;

        const float distanceSq =
            dx * dx +
            dy * dy +
            testZ * testZ;

        const float distance = sqrtf(distanceSq);

        if (distance <= 0.000001f)
            return true;

        /*
            Projection aspect actually being used by the game.
        */
        const float width =
            static_cast<float>(patch::GetUInt(0xC56BC8));

        const float height =
            static_cast<float>(patch::GetUInt(0xC56BCC));

        float aspectScale = 1.0f;

        if (height > 0.0f)
        {
            const float aspect = width / height;

            /*
                Original game's intended aspect = 4:3.
            */
            aspectScale = aspect / (4.0f / 3.0f);

            /*
                Never make the original frustum narrower.
            */
            if (aspectScale < 1.0f)
                aspectScale = 1.0f;
        }

        /*
            Projection of object center onto cone axis.

            Original algebra:

            -((dz / distance * axisZ +
               dy / distance * axisY +
               dx / distance * axisX) * distance)

            simplifies exactly to:

            -(dx*axisX + dy*axisY + dz*axisZ)
        */
        const float depth = -(
            testZ * _this[9] +
            dy * _this[8] +
            dx * _this[7]
            );

        /*
            Near/far clipping has nothing to do with aspect ratio.
        */
        if (_this[0] - radius <= depth &&
            depth <= _this[1] + radius)
        {
            /*
                Original cone:

                    sin(theta) = mConeSine
                    1/cos(theta) = mConeInvCosine

                Therefore:

                    tan(theta) = sin(theta) / cos(theta)
                               = mConeSine * mConeInvCosine

                Widescreen increases the horizontal tangent.
            */
            const float originalTan =
                _this[2] * _this[3];

            const float widescreenTan =
                originalTan * aspectScale;

            /*
                Recover sin(theta) and 1/cos(theta)
                from the widened tangent.

                    sec(theta) = sqrt(1 + tan²(theta))
                    sin(theta) = tan(theta) / sec(theta)
            */
            const float coneInvCos =
                sqrtf(1.0f + widescreenTan * widescreenTan);

            const float coneSin =
                widescreenTan / coneInvCos;

            const float coneRadius =
                (depth * coneSin + radius) * coneInvCos;

            /*
                Squared perpendicular distance from the cone axis.
            */
            const float radialDistanceSq =
                distanceSq - depth * depth;

            if (coneRadius * coneRadius > radialDistanceSq)
            {
                if (!cameraclip)
                    return true;

                if (radius >= 10.0f)
                {
                    radius += 5.0f;
                }
                else
                {
                    /*
                        Preserve the game's special camera clipping
                        path here.

                        See note below concerning flt_BF45DC.
                    */
                    const float cameraClipScale =
                        patch::GetFloat(0xBF45DC);

                    if (cameraClipScale * cameraClipScale *
                        coneRadius * coneRadius >
                        radialDistanceSq)
                    {
                        return true;
                    }
                }

                if (distance >= radius)
                    return true;
            }
        }

        return false;
    }

	static float gRealFontRasterScale05 = 1.0f;
	static DWORD gRealFontBoundsReturn05 = 0x0079E2B1;


	__declspec(naked) void RealFontLogicalAptBounds05()
	{
		__asm
		{
			// Width returned by the high-resolution font measurement.
			// Convert it back to logical APT units.
			fild dword ptr [esp + 0x10]
			fdiv dword ptr [gRealFontRasterScale05]
			fadd dword ptr [edi + 4]
			fstp dword ptr [edi + 0x0C]


			// Height.
			//
			// Preserve the physical height underneath the logical copy,
			// because stock code after 79E2B1 consumes it.
			fild dword ptr [esp + 0x2C]
			fld st(0)

			fdiv dword ptr [gRealFontRasterScale05]
			fadd dword ptr [edi + 8]
			fstp dword ptr [edi + 0x10]

			jmp dword ptr [gRealFontBoundsReturn05]
		}
	}

	static DWORD gRealFontSecondaryBoundsReturn05 = 0x0079E45A;

	__declspec(naked) void RealFontSecondaryLogicalBounds05()
	{
		__asm
		{
			// Physical raster width -> logical APT width.
			fld dword ptr [ebx + 0x18]
			fdiv dword ptr [gRealFontRasterScale05]
			fstp dword ptr [edi + 0x40]

			// Physical raster height -> logical APT height.
			fld dword ptr [ebx + 0x1C]
			fdiv dword ptr [gRealFontRasterScale05]
			fstp dword ptr [edi + 0x44]

			// Original stack cleanup.
			add esp, 4

			mov dword ptr [edi + 0x20], 0x447A0000

			jmp dword ptr [gRealFontSecondaryBoundsReturn05]
		}
	}
	void InstallHighResolutionAptFonts05()
	{
		gRealFontRasterScale05 =
			(RES_Y > 0)
			? static_cast<float>(RES_Y) / 480.0f
			: 1.0f;

		if (gRealFontRasterScale05 <= 0.0f)
			gRealFontRasterScale05 = 1.0f;

		const float fontTextureScale =
			1.0f / gRealFontRasterScale05;


		// Enable scalable RealFont rasterization.
		patch::SetUInt(0xC14A1C, 0);

		patch::SetFloat(
			0xC14A10,
			gRealFontRasterScale05
		);

		patch::SetFloat(
			0xC14A14,
			gRealFontRasterScale05
		);


		// Convert measured bounds back to logical APT coordinates.
		patch::RedirectJump(
			0x79E29B,
			RealFontLogicalAptBounds05
		);

		patch::Nop(
			0x79E2A0,
			0x11
		);

		patch::RedirectJump(
			0x79E444,
			RealFontSecondaryLogicalBounds05
		);

		patch::Nop(
			0x79E449,
			0x11
		);
		// IMPORTANT 2005 difference:
		//
		// var_258 / var_254 have already been measured using
		// the supersampled font. Do not multiply them by the
		// scale again.
		patch::Nop(0x79E2B5, 4);
		patch::Nop(0x79E2BE, 4);


		// Visible quad stays at logical size.
		patch::Nop(0x79E483, 4);
		patch::Nop(0x79E4A0, 4);


		// Sample the larger generated texture correctly.
		patch::SetFloat(
			0x79EA55,
			fontTextureScale
		);

		patch::SetFloat(
			0x79EA5C,
			fontTextureScale
		);


		// FFN isolation.
		patch::SetUChar(0x79DA02, 0xB8);
		patch::SetUInt(0x79DA03, 0x3F800000);

		patch::SetUChar(0x79DA1F, 0xBA);
		patch::SetUInt(0x79DA20, 0x3F800000);
		patch::Nop(0x79DA24, 1);
	}
    // Changes resolution after exiting game but remains as sample ASM injection
    void __declspec(naked) OnSetArrangeWindow3() {
        __asm {
            push RES_Y
            push RES_X
            mov  ecx, 0x5F85DF
            jmp  ecx
        }
    }

    static void METHOD SetViewPortMovie1(float* _t, DUMMY_ARG, DWORD* a2, int xOffset, int yOffset, int w, int h, int nearP, int farP) {
        CallMethod<0x6ED274>(_t, a2, xOffset, yOffset, RES_X, RES_Y, nearP, farP);
    }

    // IDirect3DDevice9 *device = *(IDirect3DDevice9 **)GetPointer(0xC56CD0);
    DWORD METHOD FEAptInterface_Render(DWORD* t, DUMMY_ARG, char a1)
    {
        int v2; // [esp+Ch] [ebp-94h] BYREF
        float v3[17]; // [esp+10h] [ebp-90h] BYREF
        float v4; // [esp+54h] [ebp-4Ch]
        char v5[64]; // [esp+60h] [ebp-40h] BYREF
        float invX;
        float invY;
        float v6;
        float v9;
        float v10;

        v2 = CallAndReturn<int, 0x7B0C70>(); // 480
        v4 = CallAndReturn<int, 0x7B0CA0>(); // 640
        invX = (double)v2 * 0.0015625f; // 640
        invY = (double)v4 * 0.0020833334f; // 480
        v10 = -0.5f;
        if (v2 / v4 > 1.3333334)
        {
            v9 = invY;
            invX = (double)v2 * 0.0011709601f;
            v10 = 107.0 * invX - 0.5;
        }
        v3[0] = invX;
        v3[5] = invY;
        memset(&v3[1], 0, 16);
        memset(&v3[6], 0, 16);
        v3[10] = 1.0f;
        v3[11] = 0.0f;
        v3[12] = v10;
        v3[13] = -0.5f;
        v3[14] = -100.0f;
        v3[15] = 1.0f;
        //DWORD* address = (DWORD*)patch::GetPointer(0xC49D70);
        void* camera = *raw_ptr<void*>(*(void**)0xC49D70, 28);
        CallMethod<0x6E536A>(camera, &v2, v5);
        CallMethod<0x6ECE34>(camera, &v2, v3);
        if (a1)
        {
            CallMethod<0x6ECD87>(camera, (DWORD*)&v2, 0xFFFFFFFF, 5);
        }
        Call<0x7665D0>();
        return CallMethodAndReturn<DWORD, 0x6ECE34>(camera, &v2, v5);
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

        int* pMouseDevice = *raw_ptr<int*>(_this, 0xC);
        result = *pMouseDevice;

        if (pMouseDevice)
        {
            v3 = CallAndReturn<int*, 0x6160E0>();
            v4 = CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 3); // pMouseDevice->PeekInput(3)
            v12 = CallVirtualMethodAndReturn<int, 2>(pMouseDevice, 4);
            int* pBroadcast = *raw_ptr<int*>(v3, 0x4);
            int height = CallAndReturn<int, 0x7B0CA0>();
            int width = CallAndReturn<int, 0x7B0C70>();
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
                CallMethod<0x611E00>(v3, 131, 0, v5, v8, 0);
            }
            if (CallVirtualMethodAndReturn<int, 14>(pMouseDevice, 0))
            {
                CallVirtualMethod<1>(v3, 132, 0, v5, v8, 0);
            }
            else if (CallVirtualMethodAndReturn<int, 15>(pMouseDevice, 0))
            {
                CallMethod<0x611E00>(v3, 133, 0, v5, v8, 0);
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
    float loadXPos = ((200.0f / 640.0f) * (RES_Y * 1.33333f)) + ((RES_X - (RES_Y * 1.33333f)) / 2);
    float loadYPos = ((447.5f / 480.0f) * RES_Y);
    int loadWidth = 256 * (RES_Y / 480.0f);
    int loadHeight = 32 * (RES_Y / 480.0f);

    static void METHOD ProgressBarCreate(float* _t, DUMMY_ARG, DWORD* a2, float xLocation, float yLocation, int width, int height, float a7, int a8) {
        CallMethod<0x5E84C0>(_t, a2, loadXPos, loadYPos, loadWidth, loadHeight, a7, a8);
    }

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

void Install_LIVE2005() {
    using namespace live2005;
    // Change aspect ratio in-game
    patch::RedirectJump(0x6EC921, SetPerspectiveProjection05);
    patch::RedirectJump(0x728050, SetTestInConicalFrustum);
    InstallHighResolutionAptFonts05();
    // Changes size of buffer.
    patch::SetUInt(0x41E8A0 + 1, RES_Y);
    patch::SetUInt(0x41E865 + 1, RES_X);
    // Changes size of menu, intro.
    patch::SetUInt(0x741678 + 1, RES_Y);
    patch::SetUInt(0x74167D + 1, RES_X);
    // Changes size of external window.
    patch::SetUInt(0x4150EC + 1, RES_Y);
    patch::SetUInt(0x4150F1 + 1, RES_X);
    // Changes resolution after exiting game
    patch::SetUInt(0x5F85D5 + 1, RES_Y);
    patch::SetUInt(0x5F85DA + 1, RES_X);
    patch::SetUInt(0x5F2BA6 + 1, RES_Y);
    patch::SetUInt(0x5F2BAB + 1, RES_X);
    // modify resolution addresses
    for (const auto& resolution : ids) {
        patch::SetUInt(0xBDF758 + 20 * resolution.id + 4, resolution.width);
        patch::SetUInt(0xBDF758 + 20 * resolution.id + 8, resolution.height);
        patch::SetUInt(0xBDF758 + 20 * resolution.id + 12, resolution.depth);
    };
    // UI adjustment
    patch::RedirectJump(0x4C5560, FEAptInterface_Render);
    patch::RedirectJump(0x617660, BroadcastMouseInput);
    // Loadbar
    patch::RedirectCall(0x5CC5FD, ProgressBarCreate);
    patch::RedirectCall(0x5DB5C8, ProgressBarCreate);
    patch::RedirectCall(0x608C1D, ProgressBarCreate);
    patch::RedirectCall(0x60E82F, ProgressBarCreate);
    // Movies
    if (INTRO == 1) {
        if (ASPECT_RATIO < 1.333334f) {
            patch::SetPointer(0x560AA8 + 1, "EABRAND");
            patch::SetPointer(0x560BF6 + 1, "INTRO");
            patch::SetPointer(0x560DBF + 1, "INTRO");        
        }
        else {
            patch::SetPointer(0x560AA8 + 1, "EABRANDWS");
            patch::SetPointer(0x560BF6 + 1, "INTROWS");
            patch::SetPointer(0x560DBF + 1, "INTROWS");
            patch::SetPointer(0x5925A4 + 1, "INTROWS");
        }
    }
    else {
        patch::SetPointer(0x560AA8 + 1, "DUMMY");
        patch::SetPointer(0x560BF6 + 1, "DUMMY");
        patch::SetPointer(0x560DBF + 1, "DUMMY");
        patch::SetPointer(0x5925A4 + 1, "DUMMY");
    }
    patch::SetFloat(0x627544 + 1, CalculateAspectRatio());
    // CreateZone
    patch::SetUInt(0xB216F8 + 6, czXPos);
    patch::SetUInt(0xB21738 + 6, czYPos);
    patch::SetUInt(0xB21778 + 6, czWidth);
    patch::SetUInt(0xB217B8 + 6, czHeight);
    // Switch UI files
    std::string pathA = ".\\assets\\05WSUI"; // Replace with the actual path 'a'
    std::string pathB = ".\\sgsm"; // Replace with the actual path 'b'
    adjustWSUI::adjustWidescreenUI(ASPECT_RATIO, pathA, pathB);

    // Movies
}
