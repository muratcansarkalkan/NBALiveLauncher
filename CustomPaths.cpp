#include "plugin-std.h"

using namespace plugin;

namespace custompaths
{
    static DWORD* AddCustomPaths05()
    {
        DWORD* fsm = CallAndReturn<DWORD*, 0x429E10>();
        CallMethod<0x429E90>(fsm, "faces\\");
        return fsm;
    }

    static DWORD* AddCustomPaths06()
    {
        DWORD* fsm = CallAndReturn<DWORD*, 0x429F50>();
        CallMethod<0x429FD0>(fsm, "faces\\");
        return fsm;
    }

    static DWORD* AddCustomPaths07()
    {
        DWORD* fsm = CallAndReturn<DWORD*, 0x44DCA0>();
        CallMethod<0x44DD20>(fsm, "faces\\");
        return fsm;
    }

    static DWORD* AddCustomPaths08()
    {
        DWORD* fsm = CallAndReturn<DWORD*, 0x450500>();
        CallMethod<0x450580>(fsm, "faces\\");
        return fsm;
    }

    void Initialize()
    {
        switch (FM::GetEntryPoint())
        {
        case 0xCD8005: // NBA Live 2005 1.0 NOCD
            patch::RedirectCall(0x005D8AF1, AddCustomPaths05);
            break;

        case 0x40109F:
            if (patch::GetFloat(0xBD832C) == 1.3333334f)
            {
                // NBA Live 06 1.0 NOCD
                patch::RedirectCall(0x005E756C, AddCustomPaths06);
            }
            else if (patch::GetFloat(0xBBBC3C) == 1.3333334f)
            {
                // NBA Live 07 1.1 NOCD
                patch::RedirectCall(0x0063A19E, AddCustomPaths07);
            }
            else if (patch::GetFloat(0xC3DF84) == 1.3333334f)
            {
                // NBA Live 08 1.0 NOCD
                patch::RedirectCall(0x00659E0E, AddCustomPaths08);
            }

            break;
        }
    }
}

void InitializeCustomPaths()
{
    custompaths::Initialize();
}
