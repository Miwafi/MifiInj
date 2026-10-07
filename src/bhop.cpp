#include "bhop.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace bhop
{
Settings g_settings;

namespace
{
    // client.dll global rvas, build 14189 / steam buildid 25738536.
    constexpr std::uintptr_t kDwJumpButton      = 0x22324E0; // buttons::jump
    constexpr std::uintptr_t kDwLocalPlayerPawn = 0x2562808;

    // C_BaseEntity::m_fFlags (uint8), bit 0 = FL_ONGROUND.
    constexpr std::ptrdiff_t kFlags     = 0x3F4;
    constexpr std::uint8_t  kFlagGround = 1 << 0;

    // kbutton_t-style jump state consumed by movement code.
    constexpr std::int32_t kJumpPress   = 65537; // 0x10001
    constexpr std::int32_t kJumpRelease = 256;   // 0x100

    std::uint8_t* g_clientBase = nullptr;
    Diag          g_diag;
    bool          g_keyWasDown = false;

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
}

void Poll()
{
    std::uint8_t* base = ResolveClient();
    if (base == nullptr)
        return;

    g_diag.pawnOk = false;
    g_diag.keyHeld = false;
    g_diag.onGround = false;

    if (!g_settings.enabled)
    {
        g_keyWasDown = false;
        return;
    }

    // Physical key state only records the intent ("jump key held"); the
    // actual press/release decision happens below against live ground
    // state, so landing timing can never be missed by a stale event.
    const bool keyDown =
        (GetAsyncKeyState(g_settings.jumpKey) & 0x8000) != 0;
    g_diag.keyHeld = keyDown;

    if (!keyDown)
    {
        // One release edge on key-up so the button can never stick; while
        // the key stays up we leave the global alone and the game's own
        // input sampling is undisturbed.
        if (g_keyWasDown)
            SafeWrite(base + kDwJumpButton, kJumpRelease);
        g_keyWasDown = false;
        return;
    }
    g_keyWasDown = true;

    void* pawn = nullptr;
    if (!SafeRead(base + kDwLocalPlayerPawn, &pawn) || pawn == nullptr)
        return;
    g_diag.pawnOk = true;

    std::uint8_t flags = 0;
    if (!SafeRead(reinterpret_cast<std::uint8_t*>(pawn) + kFlags, &flags))
        return;

    const bool grounded = (flags & kFlagGround) != 0;
    g_diag.onGround = grounded;

    if (SafeWrite(base + kDwJumpButton,
                  grounded ? kJumpPress : kJumpRelease) &&
        grounded)
    {
        ++g_diag.jumpPulses;
    }
}

Diag GetDiag()
{
    return g_diag;
}
}
