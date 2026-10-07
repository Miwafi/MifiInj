#include "antiaim.h"
#include "thirdperson.h"
#include "pro/license.h"
#include "aimbot.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace antiaim
{
Settings g_settings;

namespace
{
    // client.dll rvas, build 14189 / steam buildid 25738536 (2026-10-06).
    constexpr std::uintptr_t kDwAttackButton = 0x2231FD0; // buttons::attack, build 14189
    constexpr std::uintptr_t kDwLocalPlayerPawn = 0x2562808;

    // Recoil control: CCSPlayerPawn::m_pAimPunchServices ->
    // CCSPlayer_AimPunchServices::m_aimPunchAngle (+0x50, QAngle);
    // m_iShotsFired gates compensation to active bursts.
    constexpr std::ptrdiff_t kAimPunchServices = 0x1598;
    constexpr std::ptrdiff_t kAimPunchAngle    = 0x50;
    constexpr std::ptrdiff_t kShotsFired       = 0x1EB4;

    // Silent command older than this is ignored (render thread stall etc.).
    constexpr std::uint64_t kSilentMaxAgeMs = 150;

    // CCSGOInput per-slot history ring (same block the third-person code
    // resolves as the camera state P):
    //   ring base   = P + 0xB50 + slot*0x18
    //   count dword = ring+0x00, element array ptr = ring+0x08
    //   entry stride 0x440, entry view QAngle at +0x430, roll dword +0x438
    // Live camera angles: P + slot*0x928 + 0x688 (QAngle).
    constexpr std::ptrdiff_t kRingBaseOff   = 0xB50;
    constexpr std::ptrdiff_t kSlotStride    = 0x928;
    constexpr std::size_t    kRingSlotStride = 0x18;
    constexpr std::size_t    kEntryStride   = 0x440;
    constexpr std::ptrdiff_t kEntryAngles   = 0x430;
    constexpr std::ptrdiff_t kLiveAngles    = 0x688;

    constexpr std::uintptr_t kEntryBuildRva = 0xB5F770; // build 14189
    // Prologue = THREE 5-byte RSP-relative mov stores (15 bytes total);
    // must relocate all 15 or the stub lands mid-instruction.
    constexpr int            kRelocBytes    = 15;

    // Build 14189: the old (global*, slot, ang) wrapper at 0xBF00E0 was
    // inlined into WriteUserCmd; the shared leaf below now carries the
    // write. It backs the old QAngle (pawn+0x13A8) up to +0x13B4 and
    // stores *ang into pawn+0x13A8. WriteUserCmd calls it TWICE per tick
    // with the (faked) command angles; ~10 call sites total.
    constexpr std::uintptr_t kSetAnglesRva  = 0xB1A4A0;
    // movsd xmm0,[rcx+13A8h] (8B) + mov eax,[rcx+13B0h] (6B) = 14 bytes;
    // an RIP-relative byte comes right after, so both must be relocated.
    constexpr int            kSetRelocBytes = 14;

    constexpr float kDownPitch  = 89.0f;
    constexpr float kYawReverse  = 180.0f;

    struct QAngle
    {
        float pitch;
        float yaw;
        float roll;
    };

    std::uint8_t* g_clientBase = nullptr;
    std::uint8_t* g_inputBase  = nullptr; // CCSGOInput instance P (rcx of the entry builder)
    Diag          g_diag;

    using EntryBuildFn = void (__fastcall*)(void* slotBase, int slot);
    EntryBuildFn   g_original = nullptr;
    volatile long  g_installed = 0;

    using SetAnglesFn = void (__fastcall*)(void* pawn, QAngle* ang);
    SetAnglesFn    g_originalSet = nullptr;
    volatile long  g_setInstalled = 0;

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

    template <typename T>
    bool SafeWrite(void* addr, T value)
    {
        __try
        {
            *reinterpret_cast<T*>(addr) = value;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::uint8_t* ResolveClient()
    {
        if (g_clientBase != nullptr)
            return g_clientBase;
        HMODULE mod = GetModuleHandleA("client.dll");
        g_diag.clientFound = mod != nullptr;
        if (mod != nullptr)
            g_clientBase = reinterpret_cast<std::uint8_t*>(mod);
        return g_clientBase;
    }

    float NormalizeYaw(float yaw)
    {
        while (yaw > 180.0f)  yaw -= 360.0f;
        while (yaw < -180.0f) yaw += 360.0f;
        return yaw;
    }

    bool ReadablePointer(std::uintptr_t p)
    {
        return p > 0x10000ULL && p < 0x7FFFFFFFFFFFULL;
    }

    // Resolve the newest history entry and the live camera angles. Returns
    // false when the ring cannot be read.
    bool ResolveEntry(void* slotBase, int slot,
                      std::uint8_t** outEntry, QAngle* outLive)
    {
        auto* ring = reinterpret_cast<std::uint8_t*>(slotBase) +
                     kRingBaseOff +
                     static_cast<std::size_t>(slot) * kRingSlotStride;

        std::int32_t count = 0;
        void*        array = nullptr;
        if (!SafeRead(ring, &count) || !SafeRead(ring + 8, &array) ||
            count <= 0 || count > 200000 || !ReadablePointer(
                reinterpret_cast<std::uintptr_t>(array)))
        {
            return false;
        }

        *outEntry = reinterpret_cast<std::uint8_t*>(array) +
                    static_cast<std::size_t>(count - 1) * kEntryStride;

        auto* liveAddr = reinterpret_cast<std::uint8_t*>(slotBase) +
                         static_cast<std::size_t>(slot) * kSlotStride +
                         kLiveAngles;
        if (!SafeRead(liveAddr, outLive) ||
            !std::isfinite(outLive->pitch) || !std::isfinite(outLive->yaw))
        {
            return false;
        }
        return true;
    }

    // Read the current aim punch QAngle. Returns false (no punch data)
    // outside an active burst.
    bool ReadAimPunch(QAngle* outPunch, std::int32_t* outShots)
    {
        *outShots = 0;
        if (g_clientBase == nullptr)
            return false;

        std::uint8_t* pawn = nullptr;
        if (!SafeRead(g_clientBase + kDwLocalPlayerPawn, &pawn) ||
            pawn == nullptr)
            return false;

        std::int32_t shots = 0;
        if (!SafeRead(pawn + kShotsFired, &shots))
            return false;
        *outShots = shots;
        if (shots <= 0)
            return false;

        std::uint8_t* svc = nullptr;
        if (!SafeRead(pawn + kAimPunchServices, &svc) || svc == nullptr)
            return false;

        QAngle punch{};
        if (!SafeRead(svc + kAimPunchAngle, &punch) ||
            !std::isfinite(punch.pitch) || !std::isfinite(punch.yaw))
            return false;
        *outPunch = punch;
        return true;
    }

    // CS2 applies recoil by pushing the LIVE camera angles: by the time we
    // read them, the accumulated punch is already there. Subtracting the
    // full punch therefore over-compensates ~2x. Only the per-tick DELTA
    // must be cancelled (classic Osiris-style RCS): each new kick pushes
    // the view up by (punchNow - punchLast), we pull the command back by
    // the same amount; punch recovery (delta flips sign) pulls back with
    // it. The entry builder fires several times per tick (subtick samples),
    // so the correction is memoised on (shotsFired, punch value) and every
    // sample of the same tick receives the same correction.
    struct RcsState
    {
        bool        armed = false; // a burst baseline exists
        std::int32_t shots = 0;
        QAngle      last{};        // punch the delta was measured from
        QAngle      delta{};       // correction valid for this tick
    };
    RcsState g_rcs;

    void RcsReset()
    {
        g_rcs.armed = false;
        g_rcs.shots = 0;
        g_rcs.last  = QAngle{ 0.0f, 0.0f, 0.0f };
        g_rcs.delta = QAngle{ 0.0f, 0.0f, 0.0f };
    }

    QAngle RcsCorrection(const QAngle& punch, std::int32_t shots)
    {
        if (shots <= 0)
        {
            RcsReset();
            return QAngle{ 0.0f, 0.0f, 0.0f };
        }

        // Same tick (identical punch snapshot): reuse the memoised value.
        if (g_rcs.armed &&
            g_rcs.shots == shots &&
            g_rcs.last.pitch == punch.pitch &&
            g_rcs.last.yaw == punch.yaw)
        {
            return g_rcs.delta;
        }

        if (!g_rcs.armed)
        {
            // First sample of a burst: establish the baseline, do not
            // compensate the first shot.
            g_rcs.armed = true;
            g_rcs.shots = shots;
            g_rcs.last  = punch;
            g_rcs.delta = QAngle{ 0.0f, 0.0f, 0.0f };
            return g_rcs.delta;
        }

        g_rcs.delta = QAngle{
            punch.pitch - g_rcs.last.pitch,
            NormalizeYaw(punch.yaw - g_rcs.last.yaw),
            0.0f,
        };
        g_rcs.last  = punch;
        g_rcs.shots = shots;
        return g_rcs.delta;
    }

    // Runs on the game's input thread, once per input history entry (once
    // per tick, plus subtick samples). The original has just appended the
    // newest entry. Exactly one command-angle rewrite may win per tick:
    //   1. anti-aim fake (when enabled in third person),
    //   2. silent aim snap (+ optional RCS),
    //   3. RCS over the live angles.
    // Pro-gated AA activation: the feature bit must be covered by a valid
    // license (community build locks the feature off entirely).
    bool AAActive()
    {
        return g_settings.enabled && thirdperson::Ready() &&
               thirdperson::Active() &&
               pro::FeatureEnabled(pro::kFeatAntiaim);
    }

    void __fastcall HookedEntryBuild(void* slotBase, int slot)
    {
        g_original(slotBase, slot);

        g_diag.faking = false;

        if (slotBase == nullptr || slot < 0 || slot > 3)
            return;

        g_inputBase = reinterpret_cast<std::uint8_t*>(slotBase);

        __try
        {
            std::uint8_t* entry = nullptr;
            QAngle live{};
            if (!ResolveEntry(slotBase, slot, &entry, &live))
                return;

            // ---- Mode 1: anti-aim (takes priority over everything) ----
            if (AAActive())
            {
                // AA owns the command angles here; do not let a frozen RCS
                // baseline unload in one big correction when it ends.
                RcsReset();

                std::uint32_t attack = 0;
                if (g_settings.realWhileAttack &&
                    SafeRead(g_clientBase + kDwAttackButton, &attack) &&
                    (attack & 1u))
                {
                    g_diag.firing = true;
                    return;
                }
                g_diag.firing = false;

                const QAngle fake{
                    kDownPitch,
                    NormalizeYaw(live.yaw + kYawReverse),
                    0.0f,
                };

                if (SafeWrite(entry + kEntryAngles, fake))
                {
                    g_diag.faking = true;
                    g_diag.realYaw = live.yaw;
                    g_diag.fakeYaw = fake.yaw;
                    ++g_diag.ticks;
                }
                return;
            }

            // ---- Modes 2/3: silent aim and RCS ----
            aimbot::SilentCommand sc;
            const std::uint64_t now = GetTickCount64();
            const bool silent =
                aimbot::GetSilentCommand(&sc) &&
                sc.active != 0 &&
                (now - sc.stampMs) < kSilentMaxAgeMs &&
                std::isfinite(sc.pitch) && std::isfinite(sc.yaw);

            const bool rcsWanted = aimbot::g_settings.rcs;
            if (!silent && !rcsWanted)
                return;

            QAngle out = silent ? QAngle{ sc.pitch, sc.yaw, 0.0f } : live;
            bool changed = silent;

            if (rcsWanted)
            {
                QAngle punch{};
                std::int32_t shots = 0;
                if (ReadAimPunch(&punch, &shots))
                {
                    // Advance the state every tick so the delta stays
                    // continuous, but a silent snap already points at the
                    // target in world space -- do not add RCS on top of it.
                    const QAngle corr = RcsCorrection(punch, shots);
                    if (!silent &&
                        (corr.pitch != 0.0f || corr.yaw != 0.0f))
                    {
                        const float scale = aimbot::g_settings.rcsScale;
                        out.pitch = out.pitch - corr.pitch * scale;
                        out.yaw   = NormalizeYaw(out.yaw - corr.yaw * scale);
                        changed = true;
                        ++g_diag.rcsApplied;
                    }
                }
                else
                {
                    RcsReset();
                }
            }

            if (!changed)
                return;

            out.pitch = (std::max)(-89.0f, (std::min)(89.0f, out.pitch));
            out.yaw   = NormalizeYaw(out.yaw);
            out.roll  = 0.0f;

            if (SafeWrite(entry + kEntryAngles, out))
            {
                if (silent)
                    ++g_diag.silentWrites;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // WriteUserCmd applies the command QAngle to the local pawn twice per
    // tick through this wrapper; with faked command angles that drags the
    // third-person camera down/around. Whenever the angles being applied
    // match our fake (yaw == live+180), pass the live slot angles instead.
    // The outgoing entry memory is untouched, so vfunc+0xF8 still sends the
    // fake to the server.
    // Build 14189 leaf: SetLocalViewAngles(pawn, ang). When the caller
    // pushes the reversed (fake) yaw into the LOCAL pawn, pass the real
    // live input angles instead so the local camera does not spin.
    void __fastcall HookedSetAngles(void* pawn, QAngle* ang)
    {
        QAngle* pass = ang;

        __try
        {
            if (AAActive() && g_inputBase != nullptr &&
                ang != nullptr && pawn != nullptr)
            {
                void* localPawn = nullptr;
                SafeRead(g_clientBase + kDwLocalPlayerPawn, &localPawn);
                if (localPawn != nullptr && pawn == localPawn)
                {
                    QAngle applied{};
                    QAngle live{};
                    // The local input slot is 0 (thirdperson resolves the
                    // pawn through GetLocalPawn(0)).
                    auto* liveAddr = g_inputBase + kLiveAngles;
                    if (SafeRead(ang, &applied) &&
                        SafeRead(liveAddr, &live) &&
                        std::isfinite(applied.yaw) && std::isfinite(live.yaw))
                    {
                        float dy = NormalizeYaw(
                            applied.yaw - NormalizeYaw(live.yaw + kYawReverse));
                        if (std::fabs(dy) < 3.0f)
                        {
                            pass = reinterpret_cast<QAngle*>(liveAddr);
                            ++g_diag.localFixes;
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            pass = ang;
        }

        g_originalSet(pawn, pass);
    }

    bool PrologueMatches(void* target, const std::uint8_t* expected, int n)
    {
        return expected != nullptr && std::memcmp(target, expected, n) == 0;
    }

    bool PatchTarget(std::uint8_t* target, int patchBytes,
                     const std::uint8_t* patch)
    {
        DWORD oldProt = 0;
        if (!VirtualProtect(target, patchBytes,
                            PAGE_EXECUTE_READWRITE, &oldProt))
            return false;
        std::memcpy(target, patch, patchBytes);
        FlushInstructionCache(GetCurrentProcess(), target, patchBytes);
        DWORD dummy = 0;
        VirtualProtect(target, patchBytes, oldProt, &dummy);
        return true;
    }

    // 14-byte absolute JMP detour + executable trampoline stub. Only valid
    // when relocBytes >= 14 AND ends on a complete instruction boundary so
    // that target+relocBytes (the jump-back point) is outside the patched
    // region. The entry builder relocates 15 such bytes.
    void* InstallAbsJmp(void* target, void* detour, int relocBytes,
                        const std::uint8_t* expected)
    {
        if (!PrologueMatches(target, expected, relocBytes))
            return nullptr;

        auto* stub = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                         PAGE_EXECUTE_READWRITE));
        if (stub == nullptr)
            return nullptr;

        std::memcpy(stub, target, relocBytes);
        std::uint8_t* sp = stub + relocBytes;
        sp[0] = 0xFF; sp[1] = 0x25;
        sp[2] = sp[3] = sp[4] = sp[5] = 0;
        *reinterpret_cast<std::uint64_t*>(sp + 6) =
            reinterpret_cast<std::uint64_t>(target) + relocBytes;

        std::uint8_t patch[14] = { 0xFF, 0x25, 0, 0, 0, 0 };
        *reinterpret_cast<std::uint64_t*>(patch + 6) =
            reinterpret_cast<std::uint64_t>(detour);

        if (!PatchTarget(static_cast<std::uint8_t*>(target), sizeof(patch),
                         patch))
        {
            VirtualFree(stub, 0, MEM_RELEASE);
            return nullptr;
        }
        return stub;
    }

    bool InstallEntryHook()
    {
        if (InterlockedCompareExchange(&g_installed, 1, 0) != 0)
            return g_original != nullptr;
        if (g_clientBase == nullptr)
        {
            g_installed = 0;
            return false;
        }

        // 48 89 5C 24 08 / 48 89 6C 24 10 / 48 89 74 24 18
        static const std::uint8_t kPrologue[kRelocBytes] = {
            0x48, 0x89, 0x5C, 0x24, 0x08,
            0x48, 0x89, 0x6C, 0x24, 0x10,
            0x48, 0x89, 0x74, 0x24, 0x18,
        };

        void* stub = InstallAbsJmp(g_clientBase + kEntryBuildRva,
                                   reinterpret_cast<void*>(&HookedEntryBuild),
                                   kRelocBytes, kPrologue);
        if (stub == nullptr)
        {
            g_installed = 0;
            return false;
        }
        g_original = reinterpret_cast<EntryBuildFn>(stub);
        g_diag.hookInstalled = true;
        return true;
    }

    bool InstallSetAnglesHook()
    {
        if (InterlockedCompareExchange(&g_setInstalled, 1, 0) != 0)
            return g_originalSet != nullptr;
        if (g_clientBase == nullptr)
        {
            g_setInstalled = 0;
            return false;
        }

        // F2 0F 10 81 A8 13 00 00  movsd xmm0,[rcx+13A8h]
        // 8B 81 B0 13 00 00        mov  eax,[rcx+13B0h]
        static const std::uint8_t kPrologue[kSetRelocBytes] = {
            0xF2, 0x0F, 0x10, 0x81, 0xA8, 0x13, 0x00, 0x00,
            0x8B, 0x81, 0xB0, 0x13, 0x00, 0x00,
        };

        void* trampoline = InstallAbsJmp(
            g_clientBase + kSetAnglesRva,
            reinterpret_cast<void*>(&HookedSetAngles),
            kSetRelocBytes, kPrologue);
        if (trampoline == nullptr)
        {
            g_setInstalled = 0;
            return false;
        }
        g_originalSet = reinterpret_cast<SetAnglesFn>(trampoline);
        g_diag.setHookInstalled = true;
        return true;
    }
}

void Poll()
{
    if (ResolveClient() == nullptr)
        return;
    if (g_original == nullptr)
        InstallEntryHook();
    if (g_originalSet == nullptr)
        InstallSetAnglesHook();
}

bool Faking()
{
    return AAActive() && g_original != nullptr;
}

Diag GetDiag()
{
    return g_diag;
}
}
