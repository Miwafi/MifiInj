#pragma once

namespace menu
{
    // Applies the custom dark theme to the current ImGui context. Call
    // once right after CreateContext(); the caller may ScaleAllSizes
    // afterwards.
    void ApplyTheme();

    // Draws the overlay window. *pOpen is wired to the window close button
    // and is also toggled globally with the INSERT key.
    void Render(bool* pOpen);
}
