#include "hudfix.h"
#include <MinHook.h>
#include <Windows.h>
#include <cstring>

#pragma comment(linker, "/export:getBuildInfo=Engine_BuildInfo_orig.getBuildInfo")
namespace
{
    // UIMovieInstance::onViewResized sub esp, 48h / push ebx
    constexpr unsigned char readyBytes[] = { 0x83, 0xEC, 0x48, 0x53 };

    DWORD WINAPI initThread(LPVOID)
    {
        const auto* probe = reinterpret_cast<const unsigned char*>(OFF_UIMovieInstance_onViewResized);
        while (std::memcmp(probe, readyBytes, sizeof(readyBytes)) != 0)
            Sleep(1);

        if (MH_Initialize() != MH_OK)
        {
            hudfix::log("MinHook init failed");
            return 0;
        }

        hudfix::installScaleHooks();
        hudfix::installWidgetHooks();
        hudfix::installMenuHooks();
        hudfix::installSettingsHooks();

        const MH_STATUS status = MH_EnableHook(MH_ALL_HOOKS);
        hudfix::log("hooks: {}", MH_StatusToString(status));
        hudfix::loadSettings();

        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, &initThread, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}
