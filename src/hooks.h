#pragma once

// DXGI SwapChain VTable hooks + ImGui DX11/Win32 rendering bootstrap.
namespace hooks
{
    // Creates a throwaway D3D11 device to grab the IDXGISwapChain vtable,
    // then patches Present (8) and ResizeBuffers (13).
    // Returns false if the D3D11 device/swapchain could not be created.
    bool Install();
}
