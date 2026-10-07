#include "hooks.h"
#include "menu.h"
#include "esp.h"
#include "aimbot.h"
#include "vis.h"
#include "triggerbot.h"
#include "thirdperson.h"
#include "overrides.h"
#include "bhop.h"
#include "antiaim.h"
#include "pro/license.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// Forward declaration copied from imgui_impl_win32.h (it is inside an #if 0 block there).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace hooks
{
namespace
{

// IDXGISwapChain vtable slots
constexpr UINT kPresentIndex       = 8;
constexpr UINT kResizeBuffersIndex = 13;

using PresentFn       = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn       g_originalPresent       = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;

ID3D11Device*           g_device      = nullptr;
ID3D11DeviceContext*    g_context     = nullptr;
ID3D11RenderTargetView* g_renderView  = nullptr;
HWND                    g_gameWindow  = nullptr;
WNDPROC                 g_originalWndProc = nullptr;

bool g_initAttempted = false;
bool g_imguiReady    = false;
bool g_menuOpen      = true;
bool g_insertWasDown = false;

// ---------------------------------------------------------------------------
// VTable patching
// ---------------------------------------------------------------------------
bool PatchVTableEntry(void** vtable, UINT index, void* hookFunction, void** originalFunction)
{
    void* original = vtable[index];

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    vtable[index] = hookFunction;

    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored);

    *originalFunction = original;
    return true;
}

// Create a temporary D3D11 device + swapchain purely to read the vtable.
// The vtable itself lives in dxgi.dll and stays valid after everything is released.
bool GetSwapChainVTable(void*** outVTable)
{
    *outVTable = nullptr;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize        = sizeof(windowClass);
    windowClass.lpfnWndProc   = DefWindowProcW;
    windowClass.hInstance     = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"MifiInjDummyWindow";
    RegisterClassExW(&windowClass);

    HWND dummyWindow = CreateWindowExW(
        0, windowClass.lpszClassName, L"MifiInjDummy",
        WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
        nullptr, nullptr, windowClass.hInstance, nullptr);

    if (!dummyWindow)
    {
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferCount       = 1;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow      = dummyWindow;
    swapDesc.SampleDesc.Count  = 1;
    swapDesc.Windowed          = TRUE;
    swapDesc.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device*        device      = nullptr;
    ID3D11DeviceContext* context     = nullptr;
    IDXGISwapChain*      swapChain   = nullptr;
    D3D_FEATURE_LEVEL    featureLevel = D3D_FEATURE_LEVEL_11_0;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &swapDesc, &swapChain, &device, &featureLevel, &context);

    // Fallback for headless / virtual machines without a hardware D3D11 device.
    if (FAILED(hr))
    {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &swapDesc, &swapChain, &device, &featureLevel, &context);
    }

    if (SUCCEEDED(hr) && swapChain != nullptr)
        *outVTable = *reinterpret_cast<void***>(swapChain);

    if (swapChain) swapChain->Release();
    if (context)   context->Release();
    if (device)    device->Release();

    DestroyWindow(dummyWindow);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    return *outVTable != nullptr;
}

// ---------------------------------------------------------------------------
// Back-buffer render target (ImGui 1.92 DX11 backend does not bind one itself)
// ---------------------------------------------------------------------------
bool CreateRenderTarget(IDXGISwapChain* swapChain)
{
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || backBuffer == nullptr)
        return false;

    HRESULT hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderView);
    backBuffer->Release();
    return SUCCEEDED(hr) && g_renderView != nullptr;
}

void ReleaseRenderTarget()
{
    if (g_renderView != nullptr)
    {
        g_renderView->Release();
        g_renderView = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_imguiReady && g_menuOpen && hWnd == g_gameWindow)
    {
        // Feed every message to ImGui first.
        if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
            return TRUE;

        // While the cursor/keyboard is over the menu, swallow raw mouse
        // (WM_INPUT drives CS2's camera) and keyboard messages so the game
        // does not react to menu interaction.
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse &&
            (message == WM_INPUT ||
             (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)))
        {
            return TRUE;
        }
        if (io.WantCaptureKeyboard &&
            message >= WM_KEYFIRST && message <= WM_KEYLAST)
        {
            return TRUE;
        }
    }

    // g_originalWndProc is only valid between install and shutdown.
    if (g_originalWndProc != nullptr)
        return CallWindowProcW(g_originalWndProc, hWnd, message, wParam, lParam);

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

// ---------------------------------------------------------------------------
// ImGui bootstrap (runs once, on the game's render thread, inside Present)
// ---------------------------------------------------------------------------
bool InitializeImGui(IDXGISwapChain* swapChain)
{
    if (g_initAttempted)
        return g_imguiReady;
    g_initAttempted = true;

    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device),
                                   reinterpret_cast<void**>(&g_device))) ||
        g_device == nullptr)
    {
        return false;
    }
    g_device->GetImmediateContext(&g_context);
    if (g_context == nullptr)
        return false;

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(swapChain->GetDesc(&swapDesc)) || swapDesc.OutputWindow == nullptr)
        return false;
    g_gameWindow = swapDesc.OutputWindow;

    if (!CreateRenderTarget(swapChain))
        return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // do not dump imgui.ini into the game directory

    // Global UI scale: rasterize the UI font at a larger pixel size
    // (crisp, unlike io.FontGlobalScale which stretches the atlas bitmap),
    // then scale every style metric so controls grow with the text.
    constexpr float kUiScale = 1.5f;
    constexpr float kFontPixels = 13.0f * kUiScale;

    // Prefer Segoe UI (ships with Windows) over the bitmap-style default
    // font; fall back if the TTF is unavailable.
    bool fontLoaded = false;
    const char* kFontCandidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\msyh.ttc",
    };
    for (const char* path : kFontCandidates)
    {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        {
            ImFontConfig cfg;
            cfg.OversampleH = 3;
            cfg.OversampleV = 2;
            cfg.PixelSnapH = false;
            if (io.Fonts->AddFontFromFileTTF(path, kFontPixels,
                                             &cfg) != nullptr)
            {
                fontLoaded = true;
                break;
            }
        }
    }
    if (!fontLoaded)
    {
        ImFontConfig fontConfig;
        fontConfig.SizePixels = kFontPixels; // 13px is ImGui's default
        io.Fonts->AddFontDefault(&fontConfig);
    }

    menu::ApplyTheme();
    ImGui::GetStyle().ScaleAllSizes(kUiScale);

    if (!ImGui_ImplWin32_Init(g_gameWindow))
        return false;
    if (!ImGui_ImplDX11_Init(g_device, g_context))
        return false;

    g_originalWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(&HookedWndProc)));
    if (g_originalWndProc == nullptr)
        return false;

    g_imguiReady = true;
    return true;
}

void ShutdownImGui()
{
    if (!g_imguiReady)
        return;
    g_imguiReady = false;

    if (g_gameWindow != nullptr && g_originalWndProc != nullptr)
    {
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_originalWndProc));
        g_originalWndProc = nullptr;
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    ReleaseRenderTarget();
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device)  { g_device->Release();  g_device = nullptr; }
    g_gameWindow = nullptr;
}

void HandleToggle()
{
    const bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (insertDown && !g_insertWasDown)
    {
        g_menuOpen = !g_menuOpen;
        if (!g_menuOpen && g_imguiReady)
            ImGui::GetIO().ClearEventsQueue(); // avoid stuck keys after closing
    }
    g_insertWasDown = insertDown;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain,
                                        UINT syncInterval, UINT flags)
{
    if (!g_initAttempted)
        InitializeImGui(swapChain);

    if (g_imguiReady)
    {
        HandleToggle();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Pro license: one-shot verification before any gated module runs.
        pro::Init();

        // Wall/line-of-sight service: install the TraceShape hook before
        // any module posts rays this frame.
        vis::Poll();

        // Aimbot adjusts the game's global view angles every frame.
        aimbot::Run();

        // Third-person camera toggle (hotkey edge + desired-state apply).
        thirdperson::Poll();

        // Anti-flash and first-person FOV overrides (same per-frame
        // client-state overwrite strategy).
        overrides::Poll();

        // Auto bunny hop: per-frame jump-button state from live ground flag.
        bhop::Poll();

        // Trigger bot: consumes the crosshair LOS result each frame.
        triggerbot::Poll();

        // Visual anti-aim: pose local model toward camera + look down.
        antiaim::Poll();

        // ESP draws to the background draw list, so it stays visible
        // even while the menu window is hidden with INSERT.
        esp::Render();

        if (g_menuOpen)
            menu::Render(&g_menuOpen); // pressing the window's X also closes it

        ImGui::Render();

        g_context->OMSetRenderTargets(1, &g_renderView, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swapChain,
                                              UINT bufferCount, UINT width,
                                              UINT height, DXGI_FORMAT newFormat,
                                              UINT swapChainFlags)
{
    ReleaseRenderTarget();

    const HRESULT result = g_originalResizeBuffers(
        swapChain, bufferCount, width, height, newFormat, swapChainFlags);

    if (g_imguiReady && SUCCEEDED(result))
        CreateRenderTarget(swapChain);

    return result;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
bool Install()
{
    void** vtable = nullptr;
    if (!GetSwapChainVTable(&vtable))
        return false;

    if (!PatchVTableEntry(vtable, kPresentIndex,
                          reinterpret_cast<void*>(&HookedPresent),
                          reinterpret_cast<void**>(&g_originalPresent)))
        return false;

    if (!PatchVTableEntry(vtable, kResizeBuffersIndex,
                          reinterpret_cast<void*>(&HookedResizeBuffers),
                          reinterpret_cast<void**>(&g_originalResizeBuffers)))
        return false;

    return true;
}

} // namespace hooks
