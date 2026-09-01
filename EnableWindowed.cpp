#include "plugin-std.h"

using namespace plugin;

namespace nbalive {

    static void METHOD OnSetWindowed05(void* t, DUMMY_ARG, char* a2, char a3, int a4, int a5) {
        CallMethod<0x7415A0>(t, a2, a3, a4, 1);
    }

    static void METHOD OnSetWindowed06(void* t, DUMMY_ARG, char* a2, char a3, int a4, int a5) {
        CallMethod<0x755D70>(t, a2, a3, a4, 1);
    }

    static void METHOD OnSetWindowed07(int a5) {
        CallMethod<0x9D90E0>(1);
    }

    static void METHOD OnSetWindowed08(int a5) {
        CallMethod<0xA12900>(1);
    }
    LPCWSTR section = L"DISPLAY";
    LPCWSTR key = L"WINDOWED";
    LPCWSTR filePath = L".\\main.ini";
    int defaultValue = 0;  // Provide a default value in case the key is not found

    // Call GetPrivateProfileInt to retrieve the integer value
    int attributeValue = GetPrivateProfileIntW(section, key, defaultValue, filePath);
}

void EnableWindowed() {
    using namespace nbalive;
    using namespace nbalive;

    const int attributeValue =
        GetPrivateProfileIntW(
            L"DISPLAY",
            L"WINDOWED",
            0,
            L".\\main.ini"
        );

    if (attributeValue != 1)
        return;

    switch (FM::GetEntryPoint()) {

        case 0xCD8005: // NBA Live 2005 1.0 NOCD
        {
            patch::RedirectCall(0x7B0B00, OnSetWindowed05);
            patch::SetChar(0xC4A8D0, 1);
            break;
        }
        case 0x40109F:
        {
            if (patch::GetFloat(0xBD832C) == 1.3333334f) { // NBA Live 06 1.0 NOCD
                patch::RedirectCall(0x7D0BB0, OnSetWindowed06);
                patch::SetChar(0xCB3748, 1);
                break;
            }
            else if (patch::GetFloat(0xBBBC3C) == 1.3333334f) { // NBA Live 07 1.1 NOCD
                patch::RedirectJump(0x9D9419, OnSetWindowed07);
                patch::SetChar(0xCEBBAC, 1);
                break;
            }
            else if (patch::GetFloat(0xC3DF84) == 1.3333334f) { // NBA Live 08 1.0 NOCD
                patch::RedirectJump(0xA12D8A, OnSetWindowed08);
                patch::SetChar(0xDA7A1C, 1);
                break;
            }
        }
    }
}


