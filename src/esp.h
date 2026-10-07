#pragma once

#include <cstddef>

#include "imgui.h"

// Player ESP drawn onto ImGui's background draw list every Present frame.
// Reads game memory directly (the DLL lives inside cs2.exe).
namespace esp
{
    // Box outline style.
    enum BoxStyle
    {
        kBoxOff     = 0,
        kBoxFull    = 1, // classic 1px rectangle
        kBoxCorners = 2, // Neverlose-style corner brackets
        kBoxRounded = 3, // rounded rectangle
    };

    // Accent color source.
    enum ColorMode
    {
        kColorStatic  = 0, // user picked color
        kColorHealth  = 1, // green -> red with remaining health
        kColorRainbow = 2, // animated hue cycle
    };

    // Where the snapline attaches to the screen edge.
    enum SnapOrigin
    {
        kSnapBottom = 0,
        kSnapCenter = 1,
        kSnapTop    = 2,
    };

    struct Settings
    {
        bool  enabled      = true;
        int   boxStyle     = kBoxRounded;
        bool  boxGlow      = true;  // soft outer glow around the box
        bool  boxFill      = true;  // dark translucent vertical fill
        int   colorMode    = kColorStatic;
        bool  name         = true;
        bool  healthBar    = true;
        bool  hpNumber     = true;  // numeric HP next to the bar
        bool  distanceText = true;
        bool  skeleton     = false;
        bool  skeletonGlow = true;  // soft glow pass behind bone lines
        bool  skeletonJoints = true; // dots on every joint
        bool  headRing     = false; // ring projected around the head
        bool  snapline     = false;
        int   snapOrigin   = kSnapBottom;
        bool  teammates    = false; // draw teammates as well as enemies
        bool  hideDormant  = true;
        bool  debug        = true;  // on-screen data-pipeline diagnostics
        float maxDistance  = 200.0f; // meters

        ImVec4 boxColor      = ImVec4(0.35f, 0.55f, 1.00f, 1.00f);
        ImVec4 skeletonColor = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        ImVec4 snapColor     = ImVec4(0.35f, 0.55f, 1.00f, 0.55f);
    };

    extern Settings g_settings;

    // CGameEntitySystem layout reverse-engineered at runtime (gold-anchor
    // scan). Other modules must use this instead of hardcoded offsets.
    struct EntityLayoutInfo
    {
        std::size_t chunkTableOffset = 0x10;
        std::size_t identityStride   = 0x78;
        std::size_t instanceOffset   = 0x00;
        bool        discovered       = false;
    };
    EntityLayoutInfo GetEntityLayout();

    // Must be called between ImGui::NewFrame() and ImGui::Render().
    void Render();
}
