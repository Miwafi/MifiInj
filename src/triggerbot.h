#pragma once

#include <cstdint>

// Trigger bot with a visibility guarantee.
//
// Every frame a ray is posted to the vis service from the eye along the
// REAL camera angles (the crosshair never moves for this). When the trace
// result comes back blocked, the impact point is matched against the enemy
// players' bone positions: a hit within the body/head radius means a living
// enemy -- not a wall -- is under the crosshair. Firing is then delayed by
// a random human-like interval and the attack button global is driven the
// same way bhop drives jump (65537 press / 256 release).
//
// It physically cannot shoot through geometry: without a fresh trace
// result nothing happens.
namespace triggerbot
{
    constexpr int kKeyAlways = -1;

    struct Settings
    {
        bool enabled  = false;
        int  holdKey  = kKeyAlways; // -1 = always on while enabled
        int  delayMin = 20;         // ms before the first shot
        int  delayMax = 80;
        bool teammates = false;    // also fire at teammates
    };

    struct Diag
    {
        bool keyHeld    = false;
        bool pawnOk     = false;
        bool traceFresh = false;
        bool traceBlocked = false;
        bool enemyUnderCrosshair = false;
        int  state     = 0; // 0 idle, 1 waiting delay, 2 holding fire
        int  presses   = 0; // total press impulses written
        int  releases  = 0;
        float nearest  = -1.0f; // degrees of the acquired target off the
                                // crosshair (-1 = no visible target)
    };

    extern Settings g_settings;

    void Poll();
    Diag GetDiag();
}
