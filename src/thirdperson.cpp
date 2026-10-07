#include "thirdperson.h"
#include "pro/license.h"
#include "antiaim.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace thirdperson
{
Settings g_settings;

namespace
{
    // client.dll offsets, build 14189 / steam buildid 25738536 (2026-10-06).
    // Re-derived against the script-binding thirdperson/firstperson
    // callbacks and the camera-state getter; getter moved -0xF0 from
    // build 14188, TraceShape/GetLocalPawn/gate getter unchanged.
    constexpr std::uintptr_t kCameraStatePtrRva = 0x223A320; // build 14189
    constexpr std::uintptr_t kGetterRva         = 0xB624F0;
    constexpr std::uintptr_t kGetLocalPawnRva   = 0x96AAD0; // unchanged in 14189
    constexpr std::uintptr_t kGateGetterRva     = 0xAC9C80; // unchanged in 14189
    constexpr std::size_t    kPawnSetThirdVfunc = 0x9D8 / sizeof(void*); // 0x13B

    constexpr std::size_t    kSlotStride     = 0x928;
    constexpr std::ptrdiff_t kThirdFlag      = 0x229; // uint8
    constexpr std::ptrdiff_t kAnglesDest     = 0x230; // 2x float
    constexpr std::ptrdiff_t kDistance       = 0x238; // float 30.0
    constexpr std::ptrdiff_t kResetDword     = 0x6A8; // int32 = 0
    constexpr std::ptrdiff_t kAngleIndex     = 0xB50; // int32
    constexpr std::ptrdiff_t kAngleArrayPtr  = 0xB58; // void*
    constexpr std::ptrdiff_t kArrayBase      = 0x430;
    constexpr std::size_t    kArrayStride    = 0x440;
    constexpr std::ptrdiff_t kFallbackAngles = 0x688;

    constexpr unsigned kResolveInterval    = 120u;
    constexpr int      kMaxResolveAttempts = 60;

    std::uint8_t* g_clientBase = nullptr;
    void*         g_state      = nullptr; // P (slot 0 camera state)
    bool          g_hooked     = false;
    std::uint8_t  g_origBytes[14] = {};

    bool  g_active         = false;
    bool  g_keyWasDown     = false;
    unsigned g_pollCounter = 0;

    Diag g_diag;

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

    bool SafeWriteRaw(void* addr, const void* src, std::size_t n)
    {
        __try
        {
            std::memcpy(addr, src, n);
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
        return SafeWriteRaw(addr, &value, sizeof(T));
    }

    using GetLocalPawnFn = void* (__fastcall*)(int slot);
    using GateGetterFn   = std::uint8_t (__fastcall*)();
    using PawnSetThirdFn = void (__fastcall*)(void* pawn, std::uint8_t on);

    void* GetLocalPawn()
    {
        auto fn = reinterpret_cast<GetLocalPawnFn>(
            g_clientBase + kGetLocalPawnRva);
        __try
        {
            return fn(0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    // Returns the camera service "third person authorized" bool
    // (global client+0x255A2C8, +0x58); -1 on failure.
    int ReadGate()
    {
        auto fn = reinterpret_cast<GateGetterFn>(
            g_clientBase + kGateGetterRva);
        __try
        {
            return fn() ? 1 : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // Replicates the pawn->vfunc[0x9D8](bool) tail of both command
    // callbacks. Without it the per-tick update clears the camera flag and
    // the orbit placement function never runs.
    bool CallPawnSetThirdPerson(void* pawn, bool on)
    {
        if (pawn == nullptr)
            return false;

        void** vtable = nullptr;
        if (!SafeRead(pawn, &vtable) || vtable == nullptr)
            return false;

        void* fnp = nullptr;
        if (!SafeRead(vtable + kPawnSetThirdVfunc, &fnp) || fnp == nullptr)
            return false;

        if (g_diag.vfuncRva == 0)
        {
            const auto delta = reinterpret_cast<std::uint8_t*>(fnp) -
                               g_clientBase;
            if (delta > 0 && delta < 0x3000000)
                g_diag.vfuncRva = static_cast<unsigned>(delta);
        }

        auto fn = reinterpret_cast<PawnSetThirdFn>(fnp);
        __try
        {
            fn(pawn, on ? 1 : 0);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Full replacement for the getter at client.dll+0xB624F0:
    //   movsxd rax, edx; imul rdx, rax, 0x928;
    //   movzx eax, byte ptr [rdx + rcx + 0x229]
    // Called by the "should render third person" predicate (rva 0xB1BBF0)
    // and the camera builder (rva 0xB99050). Returning 1 here forces the
    // camera decision at the single read point, so the game's own per-tick
    // flag clears can no longer win the race against frame-rate writes.
    std::uint8_t __fastcall HookedGetter(void* state, int slot)
    {
        if (state == nullptr)
            return 0;

        auto* p = reinterpret_cast<std::uint8_t*>(state) +
                  static_cast<std::int64_t>(slot) *
                      static_cast<std::int64_t>(kSlotStride);

        if (g_settings.enabled &&
            pro::FeatureEnabled(pro::kFeatThirdperson))
        {
            // Re-assert the flag byte inside the camera path itself so any
            // other code reading the memory directly this tick agrees.
            SafeWrite(p + kThirdFlag, std::uint8_t(1));
            return 1;
        }

        std::uint8_t v = 0;
        SafeRead(p + kThirdFlag, &v);
        return v;
    }

    bool InstallHook()
    {
        if (g_hooked || g_clientBase == nullptr)
            return g_hooked;

        void* target = g_clientBase + kGetterRva;
        std::memcpy(g_origBytes, target, sizeof(g_origBytes));

        // FF 25 00000000  <abs qword>  = jmp [rip+0]; .qword target
        std::uint8_t patch[14];
        patch[0] = 0xFF;
        patch[1] = 0x25;
        patch[2] = patch[3] = patch[4] = patch[5] = 0;
        *reinterpret_cast<std::uint64_t*>(patch + 6) =
            reinterpret_cast<std::uint64_t>(&HookedGetter);

        DWORD oldProt = 0;
        if (!VirtualProtect(target, sizeof(patch),
                            PAGE_EXECUTE_READWRITE, &oldProt))
        {
            return false;
        }
        std::memcpy(target, patch, sizeof(patch));
        FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
        DWORD dummy = 0;
        VirtualProtect(target, sizeof(patch), oldProt, &dummy);

        g_hooked = true;
        g_diag.hookInstalled = true;
        return true;
    }

    void Resolve()
    {
        if (g_diag.resolveAttempts >= kMaxResolveAttempts)
            return;

        if (g_clientBase == nullptr)
        {
            HMODULE mod = GetModuleHandleA("client.dll");
            g_diag.clientFound = mod != nullptr;
            if (mod == nullptr)
            {
                ++g_diag.resolveAttempts;
                return;
            }
            g_clientBase = reinterpret_cast<std::uint8_t*>(mod);
        }

        InstallHook();

        if (g_state != nullptr)
            return;

        ++g_diag.resolveAttempts;

        LONG peOff = 0;
        if (!SafeRead(g_clientBase + 0x3C, &peOff) || peOff <= 0)
            return;
        std::size_t imageSize = 0;
        if (!SafeRead(g_clientBase + peOff + 0x50, &imageSize) ||
            imageSize == 0)
        {
            return;
        }
        if (kCameraStatePtrRva + sizeof(void*) > imageSize)
            return;

        void* p = nullptr;
        if (!SafeRead(g_clientBase + kCameraStatePtrRva, &p))
            return;
        g_state = p;
        g_diag.stateResolved = p != nullptr;
    }

    // Resolves the live eye-angle source used by both the thirdperson
    // command callback (rva 0xB6401B) and the game's own orbit smoothing
    // func (rva 0xB6214D): indexed view entry, fallback embedded angles.
    const void* ResolveViewAngleSource(std::uint8_t* p, bool* ok)
    {
        *ok = false;

        // While anti-aim fakes the outgoing usercmd, the indexed history
        // entries carry the fake (face-camera/down) angles. The camera
        // must keep using the real view angles, which stay untouched at
        // the embedded fallback slot.
        if (antiaim::Faking())
        {
            *ok = true;
            return p + kFallbackAngles;
        }

        std::int32_t idx = 0;
        void* angleArray = nullptr;
        if (!SafeRead(p + kAngleIndex, &idx) ||
            !SafeRead(p + kAngleArrayPtr, &angleArray))
        {
            return nullptr;
        }

        if (idx != 0 && angleArray != nullptr)
        {
            auto* s = reinterpret_cast<const std::uint8_t*>(angleArray) +
                      kArrayBase +
                      static_cast<std::size_t>(idx - 1) * kArrayStride;
            *ok = true;
            return s;
        }

        *ok = true;
        return p + kFallbackAngles;
    }

    // Every frame while enabled: force the orbit angles to the live eye
    // angles. The game's own smoothing/placement path (rva 0xB620C0) only
    // runs when the per-slot camera service pointer (+0x6A0) is populated,
    // which does not happen on our command-free path; the fallback state
    // machine only refreshes orbit angles during specific movement events,
    // hence the frozen/only-on-jump camera. The camera builder (rva
    // 0xB99050) consumes +0x230/+0x234 directly, so syncing them per frame
    // gives real-time yaw/pitch following.
    void SetThirdPerson()
    {
        auto* p = static_cast<std::uint8_t*>(g_state);

        bool srcOk = false;
        const void* src = ResolveViewAngleSource(p, &srcOk);
        if (srcOk)
        {
            float angles[2] = {0.0f, 0.0f};
            if (SafeRead(src, &angles[0]) &&
                SafeRead(static_cast<const std::uint8_t*>(src) + 4,
                         &angles[1]))
            {
                SafeWriteRaw(p + kAnglesDest, angles, sizeof(angles));
            }
        }

        SafeWrite(p + kDistance, g_settings.distance);
        SafeWrite(p + kResetDword, std::int32_t(0));
        SafeWrite(p + kThirdFlag, std::uint8_t(1));
    }

    void SetFirstPerson()
    {
        auto* p = static_cast<std::uint8_t*>(g_state);
        SafeWrite(p + kResetDword, std::int32_t(0));
        SafeWrite(p + kThirdFlag, std::uint8_t(0));
    }

    // Full enable sequence, identical to the thirdperson command callback
    // (rva 0xB63FC0): camera block first, pawn authorization after.
    void ArmThirdPerson()
    {
        if (g_state != nullptr)
            SetThirdPerson();

        void* pawn = GetLocalPawn();
        g_diag.pawnFound = pawn != nullptr;
        CallPawnSetThirdPerson(pawn, true);
    }

    void ArmFirstPerson()
    {
        if (g_state != nullptr)
            SetFirstPerson();

        void* pawn = GetLocalPawn();
        g_diag.pawnFound = pawn != nullptr;
        CallPawnSetThirdPerson(pawn, false);
    }
}

void Poll()
{
    if (g_clientBase == nullptr || g_state == nullptr || !g_hooked)
    {
        if ((g_pollCounter % kResolveInterval) == 0)
            Resolve();
    }
    ++g_pollCounter;

    if (!g_hooked)
        return;

    const bool down =
        (GetAsyncKeyState(g_settings.toggleKey) & 0x8000) != 0;
    if (down && !g_keyWasDown &&
        pro::FeatureEnabled(pro::kFeatThirdperson))
        g_settings.enabled = !g_settings.enabled;
    g_keyWasDown = down;

    if (g_settings.enabled &&
        pro::FeatureEnabled(pro::kFeatThirdperson))
    {
        // Re-seed angles/distance if the game dropped the camera flag
        // (cheap no-op while it persists).
        if (g_state != nullptr)
            SetThirdPerson();

        // Arm immediately on the enable edge; afterwards poll the camera
        // service gate on spaced frames and re-arm if it dropped
        // (respawn/team switch resets the pawn-side authorization).
        bool needArm = !g_active;
        if (g_active && (g_pollCounter % 15u) == 0u)
        {
            const int gate = ReadGate();
            g_diag.gateActive = gate;
            if (gate == 0)
                needArm = true;
        }
        if (needArm)
            ArmThirdPerson();

        g_active = true;
    }
    else if (g_active)
    {
        ArmFirstPerson();
        g_active = false;
    }
}

bool Ready()
{
    return g_hooked;
}

bool Active()
{
    return g_active;
}

Diag GetDiag()
{
    return g_diag;
}
}
