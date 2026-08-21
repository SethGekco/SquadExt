#include "SquadExt.h"

#include <Phobos.h>
#include <Syringe.h>
#include <Utilities/Patch.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

HANDLE SquadExtDLL::hInstance = nullptr;

char SquadExtDLL::readBuffer[SquadExtDLL::readLength];
wchar_t SquadExtDLL::wideBuffer[SquadExtDLL::readLength];

void SquadExtDLL::ExeRun()
{
    Patch::ApplyStatic();
}

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        SquadExtDLL::hInstance = hInstance;
        Phobos::hInstance = hInstance; // needed by Patch::ApplyStatic
    }
    return true;
}

SYRINGE_HANDSHAKE(pInfo)
{
    pInfo->Message = const_cast<char*>("SquadExt");
    return S_OK;
}

// Hook into the game's main loop start so our patches apply at the right time.
DEFINE_HOOK(0x7CD810, SquadExt_ExeRun, 0x9)
{
    SquadExtDLL::ExeRun();
    return 0;
}

// Trigger deferred debug log flush after command line parse.
DEFINE_HOOK(0x52F639, SquadExt_CmdLineParse, 0x5)
{
    Debug::LogDeferredFinalize();
    return 0;
}
