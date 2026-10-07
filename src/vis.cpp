#include "vis.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vis
{
Diag g_diag;

namespace
{
    // client.dll rvas, build 14189 / steam buildid 25738536.
    constexpr std::uintptr_t kTraceShapeRva    = 0xA19EF0; // unchanged in 14189
    constexpr std::uintptr_t kDwLocalPlayerPawn = 0x2562808;

    constexpr std::ptrdiff_t kGameSceneNode = 0x330;
    constexpr std::ptrdiff_t kVecAbsOrigin  = 0xC8;
    constexpr std::ptrdiff_t kVecViewOffset = 0xF60;

    // Prologue = 48 89 54 24 10 / 48 89 4C 24 08 / 55 / 53 / 56 / 57
    // (two 5-byte stack-home stores + four pushes), exactly 14 bytes,
    // ending on a complete instruction boundary before push r12.
    constexpr int kRelocBytes = 14;

    constexpr int            kEntityChannels    = 16;
    constexpr int            kMaxProbesPerCall  = 6;
    constexpr float          kEyeMatchDist      = 9.0f;
    constexpr float          kFeetMatchHorzDist = 9.0f;
    constexpr float          kFeetMatchDz       = 10.0f;
    constexpr std::size_t    kResultBytes       = 0x200;
    constexpr std::ptrdiff_t kResultEnd         = 0x84;
    constexpr std::ptrdiff_t kQueryShapeMin     = 0x00;
    constexpr std::ptrdiff_t kQueryShapeMax     = 0x1C;
    constexpr std::ptrdiff_t kQueryRayByte      = 0x28;
    constexpr std::ptrdiff_t kFilterHullByte    = 0x40;

    using TraceFn =
        bool (__fastcall*)(void* physicsIface, void* queryParams,
                           Vec* start, Vec* end, void* filter, void* result);

    struct Channel
    {
        // Seqlock: producer bumps seqIn to odd while writing, back to even
        // afterwards; consumer stamps seqOut when a result is ready.
        volatile std::int64_t seqIn  = 0;
        int                   tag    = -1;
        float                 s[3]   = {};
        float                 e[3]   = {};
        volatile std::int64_t seqOut = 0;
        int                   blocked = 0;
        float                 hit[3]  = {};
        std::uint32_t         stampMs = 0;
    };

    std::uint8_t* g_clientBase  = nullptr;
    volatile long g_installed   = 0;
    // Fast reject for the hot path: TraceShape fires constantly; the hook
    // only touches entity memory while a producer has work queued.
    volatile long g_anyPending  = 0;

    TraceFn g_original = nullptr;

    Channel g_ent[kEntityChannels];
    Channel g_cross;
    volatile long g_cursor = 0;

    // Adaptive simulation origin. The scene-node abs origin used as the
    // initial seed is the RENDER-interpolated pawn position; while moving
    // it lags the simulation origin that the game's own movement/ground
    // traces start from, so at speed a render-relative fixed-radius gate
    // rejects every trace and probes go stale. Accepted movement traces
    // start exactly at the simulated feet, so track that point directly.
    Vec  g_refFeet{};
    bool g_refValid = false;

    constexpr float kEyeMatchDist2     = 12.0f; // sim eye, 3D
    constexpr float kFeetMatchHorz2    = 10.0f; // sim feet, horizontal
    constexpr float kFeetMatchDz2      = 12.0f;
    constexpr float kRefTrackHorz      = 48.0f; // trace may update ref
    constexpr float kRefTrackDz        = 36.0f;
    constexpr float kRefResyncDist     = 128.0f; // teleport/respawn snap

    template <typename T>
    bool SafeRead(const void* addr, T* out)
    {
        __try
        {
            *out = *reinterpret_cast<const T*>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool PatchTarget(std::uint8_t* target, const std::uint8_t* patch, int n)
    {
        DWORD oldProt = 0;
        if (!VirtualProtect(target, n, PAGE_EXECUTE_READWRITE, &oldProt))
            return false;
        std::memcpy(target, patch, n);
        FlushInstructionCache(GetCurrentProcess(), target, n);
        DWORD dummy = 0;
        VirtualProtect(target, n, oldProt, &dummy);
        return true;
    }

    bool __fastcall RunProbe(void* physicsIface, void* params, void* filter,
                             const Vec* s, const Vec* e,
                             int* outBlocked, float outHit[3])
    {
        __declspec(align(16)) std::uint8_t result[kResultBytes];
        std::uint8_t savedParams[kQueryRayByte + 1];
        std::uint8_t savedFilter = 0;
        bool ok = false;

        std::memset(result, 0, sizeof(result));

        // POD only inside the SEH frame (/EHsc). Restore unconditionally:
        // the game reuses its query object right after we return.
        __try
        {
            std::memcpy(savedParams, params, sizeof(savedParams));
            savedFilter =
                *(reinterpret_cast<std::uint8_t*>(filter) + kFilterHullByte);

            // Flatten the shape: zero mins/maxs and force ray mode.
            std::memset(params, 0, kQueryShapeMax);
            *(reinterpret_cast<std::uint8_t*>(params) + kQueryRayByte) = 0;
            *(reinterpret_cast<std::uint8_t*>(filter) + kFilterHullByte) = 0;

            const bool blocked = g_original(
                physicsIface, params,
                const_cast<Vec*>(s), const_cast<Vec*>(e),
                filter, result);

            *outBlocked = blocked ? 1 : 0;
            std::memcpy(outHit, result + kResultEnd, sizeof(float) * 3);
            ok = true;
        }
        __finally
        {
            std::memcpy(params, savedParams, sizeof(savedParams));
            *(reinterpret_cast<std::uint8_t*>(filter) + kFilterHullByte) =
                savedFilter;
        }
        return ok;
    }

    bool RecomputePending()
    {
        if (g_cross.seqIn != g_cross.seqOut)
            return true;
        for (int i = 0; i < kEntityChannels; ++i)
        {
            if (g_ent[i].seqIn != g_ent[i].seqOut)
                return true;
        }
        return false;
    }

    // Runs on the physics-allowed thread, inside the game's own (already
    // completed) local-pawn trace, so the TLS physics context is live.
    void ServiceProbes(void* physicsIface, void* params,
                       const Vec* start, void* filter)
    {
        if (physicsIface == nullptr || params == nullptr ||
            filter == nullptr || start == nullptr || g_clientBase == nullptr)
        {
            InterlockedExchange(&g_anyPending, RecomputePending() ? 1 : 0);
            return;
        }

        bool servicedAny = false;

        __try
        {
            std::uint8_t* pawn = nullptr;
            if (!SafeRead(g_clientBase + kDwLocalPlayerPawn, &pawn) ||
                pawn == nullptr)
            {
                return;
            }
            std::uint8_t* scene = nullptr;
            if (!SafeRead(pawn + kGameSceneNode, &scene) || scene == nullptr)
                return;

            Vec origin{};
            Vec viewOffset{};
            if (!SafeRead(scene + kVecAbsOrigin, &origin) ||
                !SafeRead(pawn + kVecViewOffset, &viewOffset))
            {
                return;
            }

            // Seed/resync the simulation-feet reference from the render
            // origin, then let accepted traces pull it to the exact sim
            // position below.
            if (!g_refValid)
            {
                g_refFeet = origin;
                g_refValid = true;
            }
            else
            {
                const float rx = origin.x - g_refFeet.x;
                const float ry = origin.y - g_refFeet.y;
                if (rx * rx + ry * ry >
                    kRefResyncDist * kRefResyncDist)
                {
                    // Teleport / respawn / long stall: resync.
                    g_refFeet = origin;
                }
            }

            const Vec eye{
                origin.x + viewOffset.x,
                origin.y + viewOffset.y,
                origin.z + viewOffset.z,
            };
            const Vec refEye{
                g_refFeet.x + viewOffset.x,
                g_refFeet.y + viewOffset.y,
                g_refFeet.z + viewOffset.z,
            };

            // Only borrow traces that originate at the local pawn (movement
            // sweeps/ground probes at the feet, weapon traces at the eye).
            // Match against BOTH the render origin (standing still) and the
            // trace-tracked simulation origin (moving), so high-speed
            // strafes and bhops do not starve the probe service.
            float ex = start->x - eye.x;
            float ey = start->y - eye.y;
            float ez = start->z - eye.z;
            bool nearLocal =
                (ex * ex + ey * ey + ez * ez) <
                (kEyeMatchDist * kEyeMatchDist);
            if (!nearLocal)
            {
                ex = start->x - refEye.x;
                ey = start->y - refEye.y;
                ez = start->z - refEye.z;
                nearLocal =
                    (ex * ex + ey * ey + ez * ez) <
                    (kEyeMatchDist2 * kEyeMatchDist2);
            }
            if (!nearLocal)
            {
                float fx = start->x - origin.x;
                float fy = start->y - origin.y;
                nearLocal =
                    (fx * fx + fy * fy) <
                        (kFeetMatchHorzDist * kFeetMatchHorzDist) &&
                    std::fabs(start->z - origin.z) < kFeetMatchDz;
            }
            if (!nearLocal)
            {
                const float fx = start->x - g_refFeet.x;
                const float fy = start->y - g_refFeet.y;
                nearLocal =
                    (fx * fx + fy * fy) <
                        (kFeetMatchHorz2 * kFeetMatchHorz2) &&
                    std::fabs(start->z - g_refFeet.z) < kFeetMatchDz2;
            }
            if (!nearLocal)
                return;

            InterlockedIncrement64(
                reinterpret_cast<volatile long long*>(&g_diag.nearPawn));

            // Movement/ground traces begin exactly at the simulated feet:
            // adopt their start so the reference tracks gameplay movement
            // frame-independently of render interpolation. Eye-level
            // weapon traces (dz large) never move the feet reference.
            {
                const float tx = start->x - g_refFeet.x;
                const float ty = start->y - g_refFeet.y;
                const float tz = start->z - g_refFeet.z;
                if (tx * tx + ty * ty <
                        kRefTrackHorz * kRefTrackHorz &&
                    std::fabs(tz) < kRefTrackDz)
                {
                    g_refFeet = *start;
                }
            }

            Channel* pending[kEntityChannels + 1];
            int count = 0;
            if (g_cross.seqIn != g_cross.seqOut)
                pending[count++] = &g_cross;
            for (int i = 0;
                 i < kEntityChannels && count < kMaxProbesPerCall; ++i)
            {
                if (g_ent[i].seqIn != g_ent[i].seqOut)
                    pending[count++] = &g_ent[i];
            }

            for (int i = 0; i < count; ++i)
            {
                Channel* ch = pending[i];

                const std::int64_t s1 = ch->seqIn;
                if ((s1 & 1LL) != 0 || s1 == ch->seqOut)
                    continue;

                // Fire from the CURRENT simulated eye, not the render-eye
                // snapshot the producer posted: while moving the latter
                // lags several units and could still sit behind a wall
                // edge during a peek. The target endpoint is unchanged.
                const Vec rs = refEye;
                const Vec re{ ch->e[0], ch->e[1], ch->e[2] };

                // Producer mid-write?
                if (ch->seqIn != s1 || (s1 & 1LL) != 0)
                    continue;
                if (!std::isfinite(rs.x) || !std::isfinite(re.z))
                    continue;

                int   blocked = 0;
                float hit[3] = {};
                if (!RunProbe(physicsIface, params, filter, &rs, &re,
                              &blocked, hit))
                {
                    continue;
                }

                ch->blocked = blocked;
                ch->hit[0]  = hit[0];
                ch->hit[1]  = hit[1];
                ch->hit[2]  = hit[2];
                ch->stampMs =
                    static_cast<std::uint32_t>(GetTickCount64());
                InterlockedExchange64(
                    reinterpret_cast<volatile long long*>(&ch->seqOut), s1);

                servicedAny = true;
                InterlockedIncrement64(
                    reinterpret_cast<volatile long long*>(&g_diag.probesRun));
                if (blocked)
                    InterlockedIncrement64(
                        reinterpret_cast<volatile long long*>(
                            &g_diag.probesBlocked));
                g_diag.lastProbeMs = ch->stampMs;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        if (servicedAny || g_anyPending)
            InterlockedExchange(&g_anyPending, RecomputePending() ? 1 : 0);
    }

    bool __fastcall HookedTraceShape(void* physicsIface, void* params,
                                     Vec* start, Vec* end,
                                     void* filter, void* result)
    {
        // g_original is published before the code patching; it can never be
        // null here in practice, but keep the defensive branch cheap.
        const bool blocked =
            g_original != nullptr
                ? g_original(physicsIface, params, start, end, filter, result)
                : false;

        InterlockedIncrement64(
            reinterpret_cast<volatile long long*>(&g_diag.calls));

        if (g_anyPending)
            ServiceProbes(physicsIface, params, start, filter);

        return blocked;
    }

    void Publish(Channel* ch, const Vec& s, const Vec& e)
    {
        InterlockedIncrement64(
            reinterpret_cast<volatile long long*>(&ch->seqIn)); // odd
        ch->s[0] = s.x; ch->s[1] = s.y; ch->s[2] = s.z;
        ch->e[0] = e.x; ch->e[1] = e.y; ch->e[2] = e.z;
        InterlockedIncrement64(
            reinterpret_cast<volatile long long*>(&ch->seqIn)); // even
        InterlockedExchange(&g_anyPending, 1);
    }

    Result ReadChannel(const Channel& ch, std::uint32_t maxAgeMs)
    {
        Result r;
        const std::int64_t out = ch.seqOut;
        if (out == 0 || ch.stampMs == 0)
            return r;
        const std::uint32_t now =
            static_cast<std::uint32_t>(GetTickCount64());
        if (now - ch.stampMs > maxAgeMs)
            return r;
        r.fresh   = true;
        r.blocked = ch.blocked != 0;
        r.hit     = { ch.hit[0], ch.hit[1], ch.hit[2] };
        return r;
    }

    bool InstallHook()
    {
        if (InterlockedCompareExchange(&g_installed, 1, 0) != 0)
            return g_original != nullptr;
        if (g_clientBase == nullptr)
        {
            g_installed = 0;
            return false;
        }

        static const std::uint8_t kPrologue[kRelocBytes] = {
            0x48, 0x89, 0x54, 0x24, 0x10,
            0x48, 0x89, 0x4C, 0x24, 0x08,
            0x55, 0x53, 0x56, 0x57,
        };

        auto* target = g_clientBase + kTraceShapeRva;
        if (std::memcmp(target, kPrologue, kRelocBytes) != 0)
        {
            g_installed = 0;
            return false;
        }

        auto* stub = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                         PAGE_EXECUTE_READWRITE));
        if (stub == nullptr)
        {
            g_installed = 0;
            return false;
        }

        std::memcpy(stub, target, kRelocBytes);
        std::uint8_t* sp = stub + kRelocBytes;
        sp[0] = 0xFF; sp[1] = 0x25;
        sp[2] = sp[3] = sp[4] = sp[5] = 0;
        *reinterpret_cast<std::uint64_t*>(sp + 6) =
            reinterpret_cast<std::uint64_t>(target) + kRelocBytes;

        // Publish the trampoline BEFORE patching the game bytes. Calling the
        // stub before the patch lands is harmless: it executes the complete
        // entry sequence (stack-home stores + pushes) and jumps mid-function,
        // which is exactly what the patched code would have done.
        g_original = reinterpret_cast<TraceFn>(stub);

        std::uint8_t patch[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
        *reinterpret_cast<std::uint64_t*>(patch + 6) =
            reinterpret_cast<std::uint64_t>(&HookedTraceShape);

        if (!PatchTarget(target, patch, sizeof(patch)))
        {
            g_original = nullptr;
            g_installed = 0;
            VirtualFree(stub, 0, MEM_RELEASE);
            return false;
        }

        g_diag.hookInstalled = true;
        return true;
    }
}

void Poll()
{
    if (g_clientBase == nullptr)
    {
        HMODULE mod = GetModuleHandleA("client.dll");
        g_diag.clientFound = mod != nullptr;
        if (mod != nullptr)
            g_clientBase = reinterpret_cast<std::uint8_t*>(mod);
    }
    if (g_original == nullptr)
        InstallHook();
}

void PostEntity(int entIndex, const Vec& start, const Vec& end)
{
    if (entIndex <= 0)
        return;

    Channel* ch = nullptr;
    for (int i = 0; i < kEntityChannels; ++i)
    {
        if (g_ent[i].tag == entIndex)
        {
            ch = &g_ent[i];
            break;
        }
    }
    if (ch == nullptr)
    {
        const long slot =
            InterlockedIncrement(&g_cursor) % kEntityChannels;
        ch = &g_ent[slot < 0 ? slot + kEntityChannels : slot];
        ch->tag = entIndex;
    }
    Publish(ch, start, end);
}

Result GetEntity(int entIndex, std::uint32_t maxAgeMs)
{
    for (int i = 0; i < kEntityChannels; ++i)
    {
        if (g_ent[i].tag == entIndex)
            return ReadChannel(g_ent[i], maxAgeMs);
    }
    return Result{};
}

void PostCrosshair(const Vec& start, const Vec& end)
{
    Publish(&g_cross, start, end);
}

Result GetCrosshair(std::uint32_t maxAgeMs)
{
    return ReadChannel(g_cross, maxAgeMs);
}
} // namespace vis
