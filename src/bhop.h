#pragma once

#include <cstdint>

// Auto bunny hop, pure client-side (works on any server including HvH).
//
// The game's input layer stores the sampled jump button state in a
// client.dll global ("jump" in cs2-dumper's buttons dump, rva 0x22324E0):
//   65537 (0x10001) = button pressed edge / held impulse
//   256   (0x100)   = button released
// The movement code consumes it per tick. Every rendered frame while the
// chosen jump key is physically held we re-decide from the pawn's actual
// ground state (C_BaseEntity::m_fFlags, pawn+0x3F4, bit 0 = FL_ONGROUND):
// force a press only when grounded, otherwise force release so the next
// landing can re-trigger. The jump decision therefore always runs against
// the current physics state, never against a stale key event.
//
// Globals/offsets from the local cs2-dumper output, build 14189 / steam
// buildid 25738536 (client.dll 2026-10-06); they move with game updates.
namespace bhop
{
    struct Settings
    {
        bool enabled = false;
        int  jumpKey = 0x20; // VK_SPACE
    };

    struct Diag
    {
        bool clientFound = false;
        bool pawnOk      = false; // last local-pawn dereference succeeded
        bool keyHeld     = false; // jump key physically held this frame
        bool onGround    = false; // FL_ONGROUND in m_fFlags
        int  jumpPulses  = 0;     // total grounded press impulses written
    };

    extern Settings g_settings;

    void Poll();
    Diag GetDiag();
}
