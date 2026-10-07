#include "overrides.h"
#include "thirdperson.h"
#include "esp.h"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstring>

namespace overrides
{
Settings g_settings;

namespace
{
    // client.dll globals / schema offsets. Globals re-derived for build
    // 14189 (client.dll 2026-10-06); schema fields verified unchanged.
    constexpr std::uintptr_t kDwEntityList      = 0x2717828; // build 14189
    constexpr std::uintptr_t kDwLocalPlayerPawn = 0x2562808;
    constexpr std::uintptr_t kDwLocalPlayerCtrl = 0x253A068;

    // CEntityInstance / CEntityIdentity layout (the chunk-table layout
    // itself is discovered at runtime by the esp module).
    constexpr std::ptrdiff_t kEntityIdentity = 0x10; // entity->m_pEntity
    constexpr std::ptrdiff_t kDesignerName   = 0x20; // identity->name ptr
    constexpr int            kMaxSkyScan     = 4095;
    constexpr int            kRescanFrames   = 60;   // ~1s between full scans
    constexpr int            kRevalidateFrames = 30;

    // C_EnvSky
    constexpr std::ptrdiff_t kSkyActive       = 0x600; // bool
    constexpr std::ptrdiff_t kSkyTintColor    = 0x640; // Color RGBA bytes
    constexpr std::ptrdiff_t kSkyTintOverride = 0x644; // bool
    constexpr int            kMaxSkyEntities  = 4;

    // C_EnvCubemapFog: colors the atmospheric sky fog (no override flag).
    constexpr std::ptrdiff_t kFogTintColor    = 0x604; // Color RGBA bytes
    constexpr int            kMaxFogEntities  = 4;

    // C_CSPlayerPawnBase flash block (6 consecutive dwords).
    constexpr std::ptrdiff_t kFlashBlock    = 0x14FC;
    constexpr std::size_t    kFlashBlockLen = 6 * sizeof(std::uint32_t);

    // C_BasePlayerPawn::m_pCameraServices
    constexpr std::ptrdiff_t kCameraServices = 0x1328;
    // CCSPlayer_CameraServices
    constexpr std::ptrdiff_t kFovCurrent    = 0x298; // m_iFOV
    constexpr std::ptrdiff_t kFovStart      = 0x29C; // m_iFOVStart
    // CCSPlayerController::m_iDesiredFOV
    constexpr std::ptrdiff_t kDesiredFov    = 0x794;

    std::uint8_t* g_clientBase = nullptr;
    Diag          g_diag;

    // Maps can carry several env_sky entities (world + 3D skybox); the
    // renderer uses the active one, so tint every env_sky we find.
    struct SkyEntity { void* ptr = nullptr; int index = 0; };
    SkyEntity g_skyEntities[kMaxSkyEntities] = {};
    int       g_skyCount    = 0;

    // Cubemap-fog entities have no override flag, so the original tint is
    // snapshotted before the first write and restored on disable.
    struct FogEntity
    {
        void* ptr = nullptr;
        int   index = 0;
        std::uint32_t orig = 0;
        bool  saved = false;
    };
    FogEntity g_fogEntities[kMaxFogEntities] = {};
    int       g_fogCount = 0;

    bool      g_skyWasOn    = false;
    int       g_rescanTtl   = 0; // frames until next full scan
    int       g_revalidateTtl = 0;

    bool IsReadablePointer(std::uintptr_t p)
    {
        return p > 0x10000ULL && p < 0x7FFFFFFFFFFFULL;
    }

    bool SafeReadString(const void* addr, char* out, std::size_t length)
    {
        out[0] = '\0';
        __try
        {
            for (std::size_t i = 0; i + 1 < length; ++i)
            {
                const char c = static_cast<const char*>(addr)[i];
                if (c == '\0')
                    return true;
                out[i] = c;
            }
            out[length - 1] = '\0';
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

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

    void ApplyAntiFlash(void* pawn)
    {
        static const std::uint32_t kZeros[6] = {};
        if (SafeWriteRaw(reinterpret_cast<std::uint8_t*>(pawn) + kFlashBlock,
                         kZeros, kFlashBlockLen))
        {
            ++g_diag.flashWrites;
        }
    }

    void ApplyFov(void* pawn, void* controller)
    {
        const std::int32_t fov = g_settings.fov;

        if (controller != nullptr)
            SafeWrite(reinterpret_cast<std::uint8_t*>(controller) + kDesiredFov,
                      fov);

        if (pawn != nullptr)
        {
            void* camSvc = nullptr;
            if (SafeRead(reinterpret_cast<std::uint8_t*>(pawn) +
                             kCameraServices,
                         &camSvc) &&
                camSvc != nullptr)
            {
                g_diag.camSvcOk = true;
                SafeWrite(reinterpret_cast<std::uint8_t*>(camSvc) + kFovCurrent,
                          fov);
                SafeWrite(reinterpret_cast<std::uint8_t*>(camSvc) + kFovStart,
                          fov);
                ++g_diag.fovWrites;
            }
            else
            {
                g_diag.camSvcOk = false;
            }
        }
    }

    // ---- env_sky entity resolution --------------------------------------

    bool GetEntity(std::uintptr_t entitySystem, int index, void** out)
    {
        *out = nullptr;
        const esp::EntityLayoutInfo layout = esp::GetEntityLayout();
        if (!layout.discovered)
            return false;

        std::uintptr_t chunk = 0;
        if (!SafeRead(reinterpret_cast<std::uint8_t*>(entitySystem) +
                          layout.chunkTableOffset +
                          8ULL * (static_cast<unsigned>(index) >> 9),
                      &chunk) ||
            !IsReadablePointer(chunk))
        {
            return false;
        }

        std::uintptr_t instance = 0;
        if (!SafeRead(reinterpret_cast<std::uint8_t*>(chunk) +
                          layout.identityStride *
                              (static_cast<unsigned>(index) & 0x1FF) +
                          layout.instanceOffset,
                      &instance) ||
            !IsReadablePointer(instance))
        {
            return false;
        }
        *out = reinterpret_cast<void*>(instance);
        return true;
    }

    bool GetName(void* entity, char* name, std::size_t cap)
    {
        name[0] = '\0';
        if (entity == nullptr)
            return false;
        std::uintptr_t identity = 0;
        if (!SafeRead(reinterpret_cast<std::uint8_t*>(entity) + kEntityIdentity,
                      &identity) ||
            !IsReadablePointer(identity))
        {
            return false;
        }
        std::uintptr_t namePtr = 0;
        if (!SafeRead(reinterpret_cast<std::uint8_t*>(identity) + kDesignerName,
                      &namePtr) ||
            !IsReadablePointer(namePtr))
        {
            return false;
        }
        return SafeReadString(reinterpret_cast<void*>(namePtr), name, cap);
    }

    bool IsEnvSky(void* entity)
    {
        char name[32] = {};
        if (!GetName(entity, name, sizeof(name)))
            return false;
        return std::strncmp(name, "env_sky", 7) == 0;
    }

    bool ContainsI(const char* hay, const char* needle)
    {
        for (const char* h = hay; *h; ++h)
        {
            std::size_t i = 0;
            while (needle[i] &&
                   std::tolower(static_cast<unsigned char>(h[i])) ==
                       std::tolower(static_cast<unsigned char>(needle[i])))
            {
                ++i;
            }
            if (needle[i] == '\0')
                return true;
        }
        return false;
    }

    // Full entity-list scan: collect every env_sky / env_cubemap_fog and
    // record all sky/fog-named entities for diagnostics. Throttled.
    void ScanEntities(std::uint8_t* base)
    {
        g_skyCount = 0;
        g_fogCount = 0;
        g_diag.skyCandCount = 0;
        std::uintptr_t entitySystem = 0;
        if (!SafeRead(base + kDwEntityList, &entitySystem) ||
            !IsReadablePointer(entitySystem))
        {
            return;
        }
        for (int i = 1; i <= kMaxSkyScan; ++i)
        {
            void* entity = nullptr;
            if (!GetEntity(entitySystem, i, &entity))
                continue;
            char name[32] = {};
            if (!GetName(entity, name, sizeof(name)))
                continue;

            const bool isSky = std::strncmp(name, "env_sky", 7) == 0;
            const bool isFog =
                std::strncmp(name, "env_cubemap_fog", 15) == 0;
            if (isSky && g_skyCount < kMaxSkyEntities)
                g_skyEntities[g_skyCount++] = { entity, i };
            if (isFog && g_fogCount < kMaxFogEntities)
                g_fogEntities[g_fogCount++] = { entity, i, 0, false };

            if (g_diag.skyCandCount < 8 &&
                (ContainsI(name, "sky") || ContainsI(name, "fog")))
            {
                Diag::Candidate& c = g_diag.skyCandidates[g_diag.skyCandCount++];
                c.index = i;
                std::strncpy(c.name, name, sizeof(c.name) - 1);
                if (isSky)
                    SafeRead(reinterpret_cast<std::uint8_t*>(entity) + kSkyActive,
                             &c.active);
            }
        }
    }

    bool IsEnvCubemapFog(void* entity)
    {
        char name[32] = {};
        if (!GetName(entity, name, sizeof(name)))
            return false;
        return std::strncmp(name, "env_cubemap_fog", 15) == 0;
    }

    void ApplySky()
    {
        const float* c = g_settings.skyColor;
        const std::uint8_t rgba[4] = {
            static_cast<std::uint8_t>(c[0] * 255.0f + 0.5f),
            static_cast<std::uint8_t>(c[1] * 255.0f + 0.5f),
            static_cast<std::uint8_t>(c[2] * 255.0f + 0.5f),
            static_cast<std::uint8_t>(c[3] * 255.0f + 0.5f),
        };
        for (int i = 0; i < g_skyCount; ++i)
        {
            auto* p = reinterpret_cast<std::uint8_t*>(g_skyEntities[i].ptr);
            if (SafeWriteRaw(p + kSkyTintColor, rgba, sizeof(rgba)) &&
                SafeWrite(p + kSkyTintOverride, std::uint8_t(1)))
            {
                ++g_diag.skyWrites;
            }
        }
        for (int i = 0; i < g_fogCount; ++i)
        {
            FogEntity& f = g_fogEntities[i];
            auto* p = reinterpret_cast<std::uint8_t*>(f.ptr);
            if (!f.saved)
            {
                if (!SafeRead(p + kFogTintColor, &f.orig))
                    continue;
                f.saved = true;
            }
            if (SafeWriteRaw(p + kFogTintColor, rgba, sizeof(rgba)))
                ++g_diag.skyWrites;
        }
    }

    void ClearSkyOverride()
    {
        for (int i = 0; i < g_skyCount; ++i)
            SafeWrite(reinterpret_cast<std::uint8_t*>(g_skyEntities[i].ptr) +
                          kSkyTintOverride,
                      std::uint8_t(0));
        for (int i = 0; i < g_fogCount; ++i)
        {
            FogEntity& f = g_fogEntities[i];
            if (f.saved)
            {
                SafeWrite(reinterpret_cast<std::uint8_t*>(f.ptr) + kFogTintColor,
                          f.orig);
                f.saved = false;
            }
        }
    }

    // Keep the cached env_sky / cubemap-fog sets fresh. Full scans are
    // throttled to ~1/s; caches are cheaply re-validated by designer names.
    void ResolveEnvSky(std::uint8_t* base)
    {
        if (g_skyCount > 0 || g_fogCount > 0)
        {
            if (--g_revalidateTtl <= 0)
            {
                g_revalidateTtl = kRevalidateFrames;
                bool stale = false;
                for (int i = 0; i < g_skyCount && !stale; ++i)
                    stale = !IsEnvSky(g_skyEntities[i].ptr);
                for (int i = 0; i < g_fogCount && !stale; ++i)
                    stale = !IsEnvCubemapFog(g_fogEntities[i].ptr);
                if (stale)
                {
                    g_skyCount = 0; // map change / entities removed
                    g_fogCount = 0;
                }
            }
            if (g_skyCount > 0 || g_fogCount > 0)
                return;
        }

        if (g_rescanTtl > 0)
        {
            --g_rescanTtl;
            return;
        }
        g_rescanTtl = kRescanFrames;

        ScanEntities(base);
        if (g_skyCount > 0 || g_fogCount > 0)
            g_revalidateTtl = kRevalidateFrames;
    }
}

void Poll()
{
    std::uint8_t* base = ResolveClient();
    if (base == nullptr)
        return;

    // Reset per-frame link status so the menu diag reflects the current
    // resolution even while features are disabled.
    g_diag.pawnOk = false;
    g_diag.ctrlOk = false;

    const bool needPawn = g_settings.antiFlash || g_settings.fovEnabled;
    const bool needCtrl = g_settings.fovEnabled;

    void* pawn = nullptr;
    void* controller = nullptr;

    if (needPawn)
    {
        if (SafeRead(base + kDwLocalPlayerPawn, &pawn) && pawn != nullptr)
            g_diag.pawnOk = true;
        else
            pawn = nullptr;
    }

    if (needCtrl)
    {
        if (SafeRead(base + kDwLocalPlayerCtrl, &controller) &&
            controller != nullptr)
        {
            g_diag.ctrlOk = true;
        }
        else
        {
            controller = nullptr;
        }
    }

    if (g_settings.antiFlash && pawn != nullptr)
        ApplyAntiFlash(pawn);

    // First-person FOV only: skip while third person is active (its
    // camera block owns framing).
    if (g_settings.fovEnabled && !thirdperson::Active())
        ApplyFov(pawn, controller);

    // Sky tint. Works regardless of local pawn state (entities exist once
    // the map is loaded).
    if (g_settings.skyEnabled)
    {
        ResolveEnvSky(base);
        g_diag.skyFound = g_skyCount > 0 || g_fogCount > 0;
        if (g_skyCount > 0 || g_fogCount > 0)
            ApplySky();
        g_skyWasOn = true;
    }
    else
    {
        g_diag.skyFound = false;
        if (g_skyWasOn)
        {
            ClearSkyOverride();
            g_skyWasOn = false;
        }
    }
}

Diag GetDiag()
{
    return g_diag;
}
}
