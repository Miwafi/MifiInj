#pragma once

#include <cstdint>

// Wall / line-of-sight service.
//
// The game resolves all world queries (movement sweeps, bullet traces, ...)
// through one client.dll export: TraceShape at rva 0xA19EF0
//   bool __fastcall TraceShape(void* physicsIface, void* queryParams,
//                              Vec3* start, Vec3* end,
//                              void* filter, void* result);
// It is thread-affine: the function immediately compares the calling
// thread's index against a global and skips the whole query on threads
// that are not allowed to run physics. Calling it from the render thread
// therefore silently does nothing.
//
// Instead we trampoline-hook it. Most traces are movement/weapon traces
// originating at the local pawn; after letting the original call run we
// piggyback on such a call (same, physics-allowed thread, live query
// objects) and fire our own rays REENTRANTLY through the trampoline:
// the game's own params block is temporarily flattened to a zero-extent
// ray and the filter is switched to ray mode, then everything is restored
// before the game's caller regains control.
//
// The result block receives the ray end points at +0x84 (input end when
// the ray is clear, impact point when blocked) and the wrapper itself
// returns true when the ray hit something.
//
// Producers (render thread) post rays into tagged channels; the hook
// (simulation thread) drains them whenever a local-pawn trace passes by.
// Results are a few milliseconds stale, which is fine for aim assistance.
namespace vis
{
    struct Vec
    {
        float x, y, z;
    };

    struct Result
    {
        bool  fresh   = false; // a serviced result younger than maxAge exists
        bool  blocked = false; // ray hit something before its end
        Vec   hit{};           // impact point when blocked
    };

    struct Diag
    {
        bool     clientFound  = false;
        bool     hookInstalled = false;
        uint64_t calls        = 0; // TraceShape invocations observed
        uint64_t nearPawn     = 0; // calls whose ray started at local eye/feet
        uint64_t probesRun    = 0; // own rays fired
        uint64_t probesBlocked = 0;
        uint32_t lastProbeMs  = 0; // GetTickCount64() of last serviced probe
    };

    extern Diag g_diag;

    // Installs the hook once client.dll is loaded (call per frame).
    void Poll();

    // Entity-tagged channels for target selection (aimbot). Posts with the
    // same entIndex reuse one channel; tags recycle LRU-ish.
    void PostEntity(int entIndex, const Vec& start, const Vec& end);
    Result GetEntity(int entIndex, uint32_t maxAgeMs = 300);

    // Dedicated channel for the crosshair ray (triggerbot).
    void PostCrosshair(const Vec& start, const Vec& end);
    Result GetCrosshair(uint32_t maxAgeMs = 90);
}
