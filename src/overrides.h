#pragma once

#include <cstdint>

// Client-side state overrides using the same per-frame memory-write
// strategy as the third-person module: no commands, no network traffic,
// server-independent (works on remote HvH servers).
//
//   Anti-flash: zero the local pawn's entire flash state block every
//       frame. The fields are client-predicted/network-replicated floats
//       consumed by FlashbangOverlay / build-up rendering:
//         pawn+0x14FC m_flFlashBangTime
//         pawn+0x1500 m_flFlashScreenshotAlpha
//         pawn+0x1504 m_flFlashOverlayAlpha
//         pawn+0x1508 m_bFlashBuildUp
//         pawn+0x150C m_flFlashMaxAlpha
//         pawn+0x1510 m_flFlashDuration
//
//   First-person FOV: write the requested FOV to both the controller's
//       m_iDesiredFOV (0x794) and the pawn camera-service m_iFOV /
//       m_iFOVStart (0x298/0x29C via pawn+0x1328 m_pCameraServices) every
//       frame; writing start==target makes the game's own FOV lerp settle
//       immediately. Applied only while NOT in third person.
//
//   Sky color: find the per-map env_sky entity (C_EnvSky) via the entity
//       list and force its sky tint: m_TintColor (+0x640, RGBA bytes) plus
//       m_bOverrideTintColor (+0x644)=1 every frame. On disable the
//       override flag is cleared so the map's original sky returns.
//
// All offsets verified in client.dll schema field tables, patch
// 1.41.8.8 (2026-09-30 game files). They move with game updates.
namespace overrides
{
    struct Settings
    {
        bool  antiFlash = false;
        bool  fovEnabled = false;
        int   fov = 110;
        bool  skyEnabled = false;
        float skyColor[4] = { 0.45f, 0.70f, 1.00f, 1.00f }; // RGBA 0..1
    };

    struct Diag
    {
        bool clientFound = false;
        bool pawnOk      = false; // last local-pawn dereference succeeded
        bool ctrlOk      = false; // local-controller dereference succeeded
        bool camSvcOk    = false; // pawn->m_pCameraServices non-null
        bool skyFound    = false; // cached env_sky entity valid
        int  flashWrites = 0;     // frames anti-flash was applied
        int  fovWrites   = 0;     // frames FOV was applied
        int  skyWrites   = 0;     // frames sky tint was applied

        // Diagnostics from the latest throttled entity scan: sky/fog-named
        // entities present on the current map (helps target selection).
        struct Candidate
        {
            char name[24] = {};
            int  index = 0;
            unsigned char active = 0; // env_sky m_bActive (0/1)
        };
        Candidate skyCandidates[8] = {};
        int       skyCandCount = 0;
    };

    extern Settings g_settings;

    void Poll();
    Diag GetDiag();
}
