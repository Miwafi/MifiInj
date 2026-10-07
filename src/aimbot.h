#pragma once

#include "imgui.h"

#include <cstdint>

// Aim assistance.
//
// Two output modes share the same target enumeration (FOV cone, closest
// enemy bone):
//   - classic mode: writes dwViewAngles, i.e. moves the local camera;
//   - silent mode:  the desired angles are only published to the usercmd
//                    entry hook (antiaim.cpp), which overwrites the outgoing
//                    command's QAngle. The local camera never moves, so the
//                    snap is invisible locally and absent from demos.
//
// Target visibility can be gated through the vis service (a real game
// physics ray trace from the eye to the bone); stale/unknown results are
// treated as visible so a non-running trace service never bricks the aim.
namespace aimbot
{
    struct Settings
    {
        bool  enabled   = false;
        int   holdKey   = 0x12; // VK_MENU (left ALT)
        float fov       = 4.0f; // degrees, target selection radius
        float smoothing = 5.0f; // 1 = snap, higher = slower glide (classic)
        int   targetBone = 0;   // 0 head, 1 neck, 2 pelvis/chest
        bool  teammates = false;
        bool  drawFov   = true; // FOV circle around the crosshair
        ImVec4 fovColor = ImVec4(1.00f, 1.00f, 1.00f, 0.35f);

        bool  silentAim    = false; // write outgoing command angles only
        bool  visibleCheck = true;  // skip occluded targets (trace LOS)

        bool  rcs       = false; // recoil control: subtract aim punch
        float rcsScale  = 1.0f;  // punch multiplier (1.0 = full comp)
    };

    extern Settings g_settings;

    struct SilentCommand
    {
        bool               active   = false;
        float              pitch    = 0.0f;
        float              yaw      = 0.0f;
        int                target   = 0;
        std::uint64_t      stampMs  = 0;
    };

    // Runs every Present frame regardless of menu visibility.
    // Must be called between ImGui::NewFrame() and ImGui::Render().
    void Run();

    // Seqlocked read of the latest silent command, consumed by the usercmd
    // entry builder on the input thread. Returns false on seqlock retry
    // exhaustion / torn read.
    bool GetSilentCommand(SilentCommand* out);
}
