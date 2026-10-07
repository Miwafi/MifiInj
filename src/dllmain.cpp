#include <windows.h>

#include "hooks.h"

static DWORD WINAPI MainThread(LPVOID /*parameter*/)
{
    // In case the DLL is injected before the renderer is ready, retry a few times.
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        if (hooks::Install())
            break;
        Sleep(1000);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE moduleHandle, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(moduleHandle);

        HANDLE threadHandle = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (threadHandle != nullptr)
            CloseHandle(threadHandle);
    }
    return TRUE;
}
