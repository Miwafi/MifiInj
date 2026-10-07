#pragma once

#include <cstdint>

// Server-visible anti-aim ("face camera + look down" for HvH).
//
// Client-side-only writes to the model eye-angle field cannot turn the
// body (the animation graph clamps eye-vs-feet yaw) and are never seen by
// other players. The angles other players' clients animate from are the
// ones carried by the outgoing input history entries (CUserCmd samples):
// every tick client.dll builds a new 0x440-byte entry whose QAngle at
// +0x430 is copied from the live camera angles (slot state +0x688, i.e.
// the dwViewAngles global) and then immediately used to fill the network
// user command.
//
// We trampoline-hook that entry builder (client rva 0xB5F770, build
// 14189), call the original, then overwrite the newest
// entry's QAngle with:
//     pitch = +89 (look down), yaw = camera yaw + 180 (face the camera).
// The camera angle global (+0x688) is never modified.
//
// WriteUserCmd then applies the command QAngle to the LOCAL pawn twice per
// tick via the shared leaf SetLocalViewAngles(pawn, ang) at client rva
// 0xB1A4A0 (build 14189; writes pawn+0x13A8); without
// intervention that drags the third-person orbit camera down/around with
// the fake. A second hook substitutes the live angles there whenever the
// angles being applied match our fake. The network send (vfunc+0xF8) reads
// the entry memory directly, so the server (and every other client) still
// receives the faked angles; only the local pawn pose stays real.
//
// Third-person orbit sync in thirdperson.cpp reads the same history ring;
// while faking it must use the embedded fallback angles (+0x688) instead.
namespace antiaim
{
    struct Settings
    {
        bool enabled         = false;
        bool realWhileAttack = false; // send real angles while firing
    };

    struct Diag
    {
        bool clientFound  = false;
        bool hookInstalled = false;
        bool setHookInstalled = false; // local SetEyeAngles redirect hook
        bool faking       = false; // fake angles applied on last tick
        bool firing       = false; // attack held (real angles forced)
        int  ticks        = 0;     // entries faked
        int  silentWrites = 0;     // entries rewritten by silent aim
        int  rcsApplied   = 0;     // ticks aim punch was subtracted
        int  localFixes   = 0;     // local SetEyeAngles calls redirected to real
        float realYaw     = 0.0f;
        float fakeYaw     = 0.0f;
    };

    extern Settings g_settings;

    void Poll();
    Diag GetDiag();

    // True while the entry hook is live and faking is requested. Used by
    // the third-person orbit sync to avoid consuming the faked entries.
    bool Faking();
}
