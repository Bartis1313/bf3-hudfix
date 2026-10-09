#include "hudfix.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <atomic>
#include <cstring>
#include <unordered_map>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

namespace hudfix
{
    // we could use input manager, but the game uses DirectInput directly, so w/e
    namespace
    {
        using GetDeviceStateFn = HRESULT(__stdcall*)(IDirectInputDevice8A* device, DWORD size, void* data);
        using GetDeviceDataFn = HRESULT(__stdcall*)(IDirectInputDevice8A* device, DWORD objectSize, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags);

        GetDeviceStateFn oGetDeviceState = nullptr;
        GetDeviceDataFn oGetDeviceData = nullptr;
        WNDPROC g_gameProc = nullptr;
        HWND g_window = nullptr;

        std::atomic<ULONGLONG> g_blockUntil{ 0 };
        constexpr ULONGLONG BLOCK_FOR = 250;

        bool blocked()
        {
            return GetTickCount64() < g_blockUntil.load();
        }

        bool isKeyboard(IDirectInputDevice8A* device)
        {
            static std::unordered_map<IDirectInputDevice8A*, bool> known;
            const auto found = known.find(device);
            if (found != known.end())
                return found->second;

            DIDEVCAPS caps{ };
            caps.dwSize = sizeof(caps);
            const bool keyboard = SUCCEEDED(device->GetCapabilities(&caps)) && GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_KEYBOARD;
            known.emplace(device, keyboard);
            return keyboard;
        }

        HRESULT __stdcall hkGetDeviceState(IDirectInputDevice8A* device, DWORD size, void* data)
        {
            const HRESULT result = oGetDeviceState(device, size, data);
            if (SUCCEEDED(result) && blocked() && data && isKeyboard(device))
                std::memset(data, 0, size);
            return result;
        }

        HRESULT __stdcall hkGetDeviceData(IDirectInputDevice8A* device, DWORD objectSize, DIDEVICEOBJECTDATA* data, DWORD* count, DWORD flags)
        {
            const HRESULT result = oGetDeviceData(device, objectSize, data, count, flags);
            if (SUCCEEDED(result) && blocked() && count && isKeyboard(device))
                *count = 0; // the buffered presses are taken, just not given
            return result;
        }

        bool isKeyboardInput(LPARAM input)
        {
            RAWINPUTHEADER header{ };
            UINT size = sizeof(header);
            return GetRawInputData(reinterpret_cast<HRAWINPUT>(input), RID_HEADER, &header, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)
                && header.dwType == RIM_TYPEKEYBOARD;
        }

        LRESULT CALLBACK hkWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
        {
            if (blocked())
            {
                switch (message)
                {
                case WM_KEYDOWN:
                case WM_KEYUP:
                case WM_SYSKEYDOWN:
                case WM_SYSKEYUP:
                case WM_CHAR:
                case WM_SYSCHAR:
                    return 0;
                case WM_INPUT:
                    if (isKeyboardInput(lparam))
                        return DefWindowProcW(window, message, wparam, lparam); // the input's cleanup, without the game
                    break;
                }
            }
            return CallWindowProcW(g_gameProc, window, message, wparam, lparam);
        }

        BOOL CALLBACK findGameWindow(HWND window, LPARAM out)
        {
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
                return TRUE;
            *reinterpret_cast<HWND*>(out) = window;
            return FALSE;
        }

        // the window exists only once the game runs, so it is subclassed when first needed
        void subclassWindow()
        {
            if (g_window)
                return;
            HWND window = nullptr;
            EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&window));
            if (!window)
                return;
            g_window = window;
            g_gameProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&hkWndProc)));
            log("keyboard block: window {} subclassed", static_cast<const void*>(window));
        }
    }

    void holdKeyboard()
    {
        subclassWindow();
        g_blockUntil = GetTickCount64() + BLOCK_FOR;
    }

    void releaseKeyboard()
    {
        g_blockUntil = 0;
    }

    void installInputHooks()
    {
        IDirectInput8A* input = nullptr;
        IDirectInputDevice8A* device = nullptr;
        if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&input), nullptr)) || !input)
        {
            log("keyboard block: no DirectInput");
            return;
        }
        if (SUCCEEDED(input->CreateDevice(GUID_SysKeyboard, &device, nullptr)) && device)
        {
            void** vtable = *reinterpret_cast<void***>(device);
            hook(reinterpret_cast<uintptr_t>(vtable[9]), hkGetDeviceState, &oGetDeviceState);
            hook(reinterpret_cast<uintptr_t>(vtable[10]), hkGetDeviceData, &oGetDeviceData);
            device->Release();
        }
        input->Release();
    }
}
