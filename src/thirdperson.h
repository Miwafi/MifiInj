#pragma once

#include <cstddef>
#include <cstdint>

// CS2 third-person camera toggle (works on ANY server, including remote
// HvH servers).
//
// "thirdperson"/"firstperson" are cheat commands forwarded to the server,
// which remote servers reject. The effects are purely client-side, so we
// drive the client camera state directly:
//
//   client.dll global pointer at rva 0x2238450 -> per-slot camera states
//   (stride 0x928); slot 0 third-person flag at +0x229.
//
// Implementation layers:
//
//   1. Replicate the thirdperson command callback (rva 0xB64790) exactly:
//      camera-block init (seed orbit angles from the indexed view entry,
//      distance 30.0f, flag = 1) FOLLOWED BY the authoritative client-side
//      switch it performs on the local pawn:
//          pawn = client!0x96B2A0(0);
//          pawn->vtable[0x9D8/8](pawn, 1);   // 0 on disable
//      Without this call the per-tick camera update (rva 0xB63030) rejects
//      the flag every tick via its service gate (global client+0x2559D60,
//      bool +0x58, read by getter rva 0xACA450): the third-person camera
//      still renders, but the orbit placement/smoothing func (rva 0xB62890)
//      never runs, so orbit angles freeze and don't follow player yaw.
//
//   2. Inline hook on the camera-state getter rva 0xB62DB0 as belt-and-
//      braces for render decisions during edge-state clears (death/respawn).
//      Re-armed (including the pawn call) whenever the gate reads inactive.
namespace thirdperson
{
    struct Settings
    {
        bool  enabled = false;  // desired camera state
        int   toggleKey = 0x74; // VK_F5
        float distance = 120.0f; // orbit distance, units (P+0x238)
    };

    struct Diag
    {
        bool clientFound   = false;
        bool stateResolved = false; // global camera-state pointer non-null
        bool hookInstalled = false;
        bool pawnFound     = false; // last local-pawn lookup succeeded
        int  gateActive    = -1;    // camera service +0x58: -1 unknown
        unsigned vfuncRva  = 0;     // resolved pawn vfunc 0x9D8 target
        int  resolveAttempts = 0;
    };

    extern Settings g_settings;

    void Poll();

    bool Ready();  // camera state resolved and getter hooked
    bool Active(); // camera is currently third person
    Diag GetDiag();
}
