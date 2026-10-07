#include "esp.h"
#include "aimbot.h"
#include "vis.h"
#include "triggerbot.h"

#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "imgui.h"

// ---------------------------------------------------------------------------
// Offsets - schema fields verified in client.dll patch 1.41.8.8
// (2026-09-30); globals re-derived from cs2-dumper code patterns.
// Only kModelBoneArray is NOT a schema field: it is a reversed offset of
// CModelState::m_pBoneArray (CSkeletonInstance::m_modelState == 0x140).
// ---------------------------------------------------------------------------
namespace
{
    // client.dll global offsets. Build 14189 (client.dll 2026-10-06),
    // re-derived locally from cs2-dumper code patterns in the new binary.
    constexpr uintptr_t kDwEntityList          = 0x2717828; // build 14189
    constexpr uintptr_t kDwLocalPlayerPawn     = 0x2562808;
    constexpr uintptr_t kDwLocalPlayerCtrl     = 0x253A068;
    constexpr uintptr_t kDwViewMatrix          = 0x2567FA0;
    constexpr uintptr_t kDwViewAngles          = 0x25787E8;

    // C_BaseModelEntity / C_BasePlayerPawn
    constexpr uintptr_t kVecViewOffset         = 0xF60; // CNetworkViewOffsetVector

    // CEntityInstance (build 14186: m_pEntity moved to 0x10)
    constexpr uintptr_t kEntityIdentity        = 0x10;
    // CEntityIdentity
    constexpr uintptr_t kDesignerName          = 0x20;

    // C_BaseEntity
    constexpr uintptr_t kGameSceneNode      = 0x330;
    constexpr uintptr_t kHealth             = 0x34C;
    constexpr uintptr_t kLifeState          = 0x354;
    constexpr uintptr_t kTeamNum            = 0x3E7;

    // CCSPlayerController
    constexpr uintptr_t kHPlayerPawn        = 0x92C;
    // CBasePlayerController
    constexpr uintptr_t kPlayerName         = 0x6FC;

    // CGameSceneNode
    constexpr uintptr_t kVecAbsOrigin       = 0xC8;
    constexpr uintptr_t kDormant            = 0x103;

    // CSkeletonInstance / CModelState
    constexpr uintptr_t kModelState         = 0x140;
    constexpr uintptr_t kModelBoneArray     = 0x80;  // reversed, non-schema
    constexpr uintptr_t kBoneStride         = 32;

    constexpr int       kMaxControllers     = 512; // whole first identity chunk
    constexpr float     kUnitsPerMeter      = 39.3701f;
    constexpr float     kStandingHeight     = 72.0f; // units, head-bone fallback
    constexpr int       kMaxBone            = 28;

    struct Vector3
    {
        float x, y, z;
    };

    struct ViewMatrix
    {
        float m[4][4];
    };

    struct PlayerData
    {
        uintptr_t   boneArray = 0;
        Vector3     origin{};
        int         health = 0;
        float       distance = 0.0f;
        char        name[128] = {};
        const ViewMatrix* vm = nullptr;
        ImVec2      screenSize{};
        ImDrawList* drawList = nullptr;
    };

    // -----------------------------------------------------------------------
    // Safe memory access (we are in-process, but entity data can move while
    // the render thread walks the list - SEH turns a bad pointer into skip).
    // POD-only so /EHsc unwinding rules are not violated inside __try.
    // -----------------------------------------------------------------------
    template <typename T>
    bool SafeRead(const void* address, T* out)
    {
        __try
        {
            *out = *static_cast<const T*>(address);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    template <typename T>
    bool SafeWrite(void* address, const T& value)
    {
        __try
        {
            *static_cast<T*>(address) = value;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    bool SafeReadString(const void* address, char* buffer, size_t length)
    {
        __try
        {
            for (size_t i = 0; i < length; ++i)
            {
                buffer[i] = static_cast<const char*>(address)[i];
                if (buffer[i] == '\0')
                    return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            buffer[0] = '\0';
            return false;
        }
        buffer[length - 1] = '\0';
        return true;
    }

    bool IsReadablePointer(uintptr_t p)
    {
        return p > 0x10000ULL && p < 0x7FFFFFFFFFFFULL;
    }

    // Discovered entity-list layout. Defaults are the classic CS2 values;
    // the anchor scanner below overwrites them if this build differs.
    struct EntityLayout
    {
        size_t    chunkTableOffset = 0x10; // CGameEntitySystem -> chunk ptr table
        size_t    identityStride   = 0x78; // sizeof(CEntityIdentity)
        size_t    instanceOffset   = 0x00; // m_pInstance inside an identity
        bool      discovered       = false;
        int       controllerIndex  = -1;
        int       pawnIndex        = -1;
        uintptr_t chunk0           = 0;
        size_t    ctrlHitOffset    = 0;
        size_t    pawnHitOffset    = 0;
        int       scanTtl          = 0;    // retries while undiscovered
    };
    EntityLayout g_layout;

    // CGameEntitySystem entity list: chunk pointer table, chunks hold 512
    // CEntityIdentity slots; m_pInstance sits inside each slot.
    uintptr_t GetEntity(uintptr_t entitySystem, int index)
    {
        uintptr_t chunk = 0;
        if (!SafeRead(reinterpret_cast<void*>(
                          entitySystem + g_layout.chunkTableOffset +
                          8ULL * (static_cast<unsigned>(index) >> 9)),
                      &chunk) ||
            !IsReadablePointer(chunk))
        {
            return 0;
        }

        uintptr_t instance = 0;
        if (!SafeRead(reinterpret_cast<void*>(
                          chunk + g_layout.identityStride *
                                      (static_cast<unsigned>(index) & 0x1FF) +
                          g_layout.instanceOffset),
                      &instance) ||
            !IsReadablePointer(instance))
        {
            return 0;
        }
        return instance;
    }

    // entity (m_pEntity 0x10) -> CEntityIdentity -> m_designerName (0x20).
    bool GetDesignerName(uintptr_t entity, char* out, size_t length)
    {
        out[0] = '\0';
        uintptr_t identity = 0;
        if (!SafeRead(reinterpret_cast<void*>(entity + kEntityIdentity),
                      &identity) ||
            !IsReadablePointer(identity))
        {
            return false;
        }
        uintptr_t namePointer = 0;
        if (!SafeRead(reinterpret_cast<void*>(identity + kDesignerName),
                      &namePointer) ||
            !IsReadablePointer(namePointer))
        {
            return false;
        }
        return SafeReadString(reinterpret_cast<void*>(namePointer),
                              out, length);
    }

    // Scan [start, start+range) for 8-byte pointer values equal to target.
    // Page-walks with VirtualQuery and guards every readable page.
    int FindPointerReferences(uintptr_t start, size_t range, uintptr_t target,
                              size_t* outOffsets, int maxOut)
    {
        int hits = 0;
        uintptr_t addr = start;
        const uintptr_t end = start + range;

        while (addr < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi,
                             sizeof(mbi)) == 0)
            {
                addr += 0x1000;
                continue;
            }
            if (mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            {
                addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) +
                       mbi.RegionSize;
                continue;
            }

            const uintptr_t pageEnd =
                (std::min)(end,
                           reinterpret_cast<uintptr_t>(mbi.BaseAddress) +
                               mbi.RegionSize);

            bool faulted = false;
            __try
            {
                for (; addr < pageEnd; addr += 8)
                {
                    if (*reinterpret_cast<const uintptr_t*>(addr) == target)
                    {
                        outOffsets[hits++] = addr - start;
                        if (hits >= maxOut)
                            return hits;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                faulted = true;
            }
            if (faulted)
                addr += 8;
        }
        return hits;
    }

    // Try to derive (identityStride, m_pInstance offset) from the chunk-local
    // offsets of two known instances (local controller & local pawn).
    bool DeriveLayout(size_t ctrlOff, size_t pawnOff,
                      size_t* outStride, size_t* outInstOff)
    {
        static const size_t kStrides[] = {
            0x78, 0x80, 0x88, 0x90, 0x70, 0x98, 0xA0
        };
        for (size_t stride : kStrides)
        {
            const size_t xc = ctrlOff % stride;
            const size_t xp = pawnOff % stride;
            if (xc != xp)
                continue;
            if (xc > 0x40 || (xc & 0x7) != 0)
                continue;
            // Sanity: the two entities must occupy different slots, and
            // their indices should look plausible.
            const long long ci = static_cast<long long>(ctrlOff - xc) /
                                 static_cast<long long>(stride);
            const long long pi = static_cast<long long>(pawnOff - xp) /
                                 static_cast<long long>(stride);
            if (ci <= 0 || pi <= 0 || ci == pi || ci > 0x200 || pi > 0x400)
                continue;
            *outStride = stride;
            *outInstOff = xc;
            return true;
        }
        return false;
    }

    // Gold-anchor discovery: locate localController/localPawn pointers inside
    // the identity chunks and reverse-engineer the real list layout.
    void DiscoverLayout(uintptr_t entitySystem, uintptr_t localController,
                        uintptr_t localPawn)
    {
        if (g_layout.discovered || g_layout.scanTtl > 0)
        {
            if (g_layout.scanTtl > 0)
                --g_layout.scanTtl;
            return;
        }
        g_layout.scanTtl = 120; // retry ~twice a second until anchors exist

        if (!IsReadablePointer(localController) || !IsReadablePointer(localPawn))
            return;

        constexpr size_t kChunkScanSize = 0x40000; // 256 KB

        // Candidate chunk pointers from the entity system header.
        for (size_t tableOff = 0x10; tableOff <= 0x80; tableOff += 8)
        {
            uintptr_t chunk = 0;
            if (!SafeRead(reinterpret_cast<void*>(entitySystem + tableOff),
                          &chunk) ||
                !IsReadablePointer(chunk))
            {
                continue;
            }

            size_t ctrlHits[4] = {};
            const int ctrlCount = FindPointerReferences(
                chunk, kChunkScanSize, localController, ctrlHits, 4);
            if (ctrlCount == 0)
                continue;

            size_t pawnHits[4] = {};
            const int pawnCount = FindPointerReferences(
                chunk, kChunkScanSize, localPawn, pawnHits, 4);
            if (pawnCount == 0)
                continue;

            // Pick the pair that yields a consistent layout.
            for (int c = 0; c < ctrlCount; ++c)
            {
                for (int p = 0; p < pawnCount; ++p)
                {
                    size_t stride = 0, instOff = 0;
                    if (!DeriveLayout(ctrlHits[c], pawnHits[p],
                                      &stride, &instOff))
                    {
                        continue;
                    }

                    g_layout.chunkTableOffset = tableOff;
                    g_layout.identityStride   = stride;
                    g_layout.instanceOffset   = instOff;
                    g_layout.chunk0           = chunk;
                    g_layout.ctrlHitOffset    = ctrlHits[c];
                    g_layout.pawnHitOffset    = pawnHits[p];
                    g_layout.controllerIndex =
                        static_cast<int>((ctrlHits[c] - instOff) / stride);
                    g_layout.pawnIndex =
                        static_cast<int>((pawnHits[p] - instOff) / stride);
                    g_layout.discovered = true;
                    g_layout.scanTtl    = 0;
                    return;
                }
            }
        }
    }

    bool WorldToScreen(const ViewMatrix& vm, const Vector3& world,
                       const ImVec2& screenSize, ImVec2* out)
    {
        const float w = vm.m[3][0] * world.x + vm.m[3][1] * world.y +
                        vm.m[3][2] * world.z + vm.m[3][3];
        if (w < 0.01f)
            return false;

        const float nx = (vm.m[0][0] * world.x + vm.m[0][1] * world.y +
                          vm.m[0][2] * world.z + vm.m[0][3]) / w;
        const float ny = (vm.m[1][0] * world.x + vm.m[1][1] * world.y +
                          vm.m[1][2] * world.z + vm.m[1][3]) / w;

        out->x = (screenSize.x * 0.5f) * (nx + 1.0f);
        out->y = (screenSize.y * 0.5f) * (1.0f - ny);
        return true;
    }

    // CS2 skeleton bone connection table (reverse-engineered from the local
    // player's bone slots, build 14186):
    //   pelvis 2, spine 3-5, neck 6, head 7
    //   L arm (rifle support, forward): 8 shoulder, 9 upper, 10 elbow, 11 hand
    //   R arm (trigger, near chest):   14 shoulder, 15 elbow, 16 hand
    //   L leg: 17 hip, 18 knee, 19 foot ; R leg: 20 hip, 21 knee, 22 foot
    struct BonePair { int a; int b; };
    const BonePair kBonePairs[] = {
        { 7,  6},  // head -> neck
        { 6,  5},  { 5,  4},  { 4,  3},  { 3,  2},  // spine -> pelvis
        { 5,  8},  { 8,  9},  { 9, 10},  {10, 11},  // left arm
        { 5, 14},  {14, 15},  {15, 16},              // right arm
        { 2, 17},  {17, 18},  {18, 19},              // left leg
        { 2, 20},  {20, 21},  {21, 22},              // right leg
    };

    bool GetBone(const PlayerData& player, int index, Vector3* out)
    {
        if (player.boneArray == 0 || index < 0 || index >= kMaxBone)
            return false;
        if (!SafeRead(reinterpret_cast<const void*>(
                          player.boneArray + static_cast<uintptr_t>(index) * kBoneStride),
                      out))
        {
            return false;
        }

        // Geometric validity: a real bone lies inside a human-sized box
        // above the feet. The bone array is allocated for all slots, so a
        // plain memory read cannot tell a used slot from a stale/unused one.
        const float dx = out->x - player.origin.x;
        const float dy = out->y - player.origin.y;
        const float dz = out->z - player.origin.z;
        if (dz < -10.0f || dz > 85.0f)
            return false;
        if (std::fabs(dx) > 40.0f || std::fabs(dy) > 40.0f)
            return false;
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz))
            return false;
        return true;
    }

    float Distance3D(const Vector3& a, const Vector3& b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // -------------------------------------------------------------------
    // Premium (Neverlose-style) draw helpers
    // -------------------------------------------------------------------
    constexpr ImU32 kShadowCol = IM_COL32(0, 0, 0, 200);

    ImU32 ScaleAlpha(ImU32 c, float mul)
    {
        int a = static_cast<int>(((c >> IM_COL32_A_SHIFT) & 0xFF) * mul);
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        return (c & ~(0xFFu << IM_COL32_A_SHIFT)) |
               (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
    }

    // Remaining-health color: green (full) -> amber (half) -> red (empty).
    ImU32 HealthColorU32(float frac, float alpha = 1.0f)
    {
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        float r, g, b;
        if (frac >= 0.5f)
        {
            const float t = (frac - 0.5f) * 2.0f;
            r = 255.0f + (90.0f  - 255.0f) * t;
            g = 210.0f + (230.0f - 210.0f) * t;
            b = 70.0f  + (120.0f - 70.0f)  * t;
        }
        else
        {
            const float t = frac * 2.0f;
            r = 255.0f;
            g = 70.0f + (210.0f - 70.0f) * t;
            b = 70.0f;
        }
        return IM_COL32(static_cast<int>(r), static_cast<int>(g),
                        static_cast<int>(b),
                        static_cast<int>(255.0f * alpha));
    }

    ImU32 RainbowColorU32(float alpha = 1.0f)
    {
        const float hue =
            fmodf(static_cast<float>(ImGui::GetTime()) * 0.06f, 1.0f);
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(hue, 0.72f, 1.0f, r, g, b);
        return IM_COL32(static_cast<int>(r * 255),
                        static_cast<int>(g * 255),
                        static_cast<int>(b * 255),
                        static_cast<int>(255 * alpha));
    }

    // Box accent according to the selected color mode.
    ImU32 AccentColorU32(float healthFrac, float alpha = 1.0f)
    {
        switch (esp::g_settings.colorMode)
        {
        case esp::kColorHealth:
            return HealthColorU32(healthFrac, alpha);
        case esp::kColorRainbow:
            return RainbowColorU32(alpha);
        default:
            return ImGui::GetColorU32(ImVec4(
                esp::g_settings.boxColor.x,
                esp::g_settings.boxColor.y,
                esp::g_settings.boxColor.z,
                esp::g_settings.boxColor.w * alpha));
        }
    }

    // One stroke of the selected box style; expand pulls the edges outward.
    void StrokeBox(ImDrawList* dl, int style,
                   ImVec2 mn, ImVec2 mx, ImU32 col,
                   float thick, float expand)
    {
        mn.x -= expand; mn.y -= expand;
        mx.x += expand; mx.y += expand;

        if (style == esp::kBoxCorners)
        {
            const float lx = (std::max)(4.0f,
                (std::min)(16.0f, (mx.x - mn.x) * 0.28f));
            const float ly = (std::max)(3.0f,
                (std::min)(12.0f, (mx.y - mn.y) * 0.16f));

            const ImVec2 c[4] = { mn, {mx.x, mn.y}, {mn.x, mx.y}, mx };
            const float sx[4] = { 1.0f, -1.0f,  1.0f, -1.0f };
            const float sy[4] = { 1.0f,  1.0f, -1.0f, -1.0f };
            for (int i = 0; i < 4; ++i)
            {
                dl->AddLine(c[i],
                    { c[i].x + sx[i] * lx, c[i].y }, col, thick);
                dl->AddLine(c[i],
                    { c[i].x, c[i].y + sy[i] * ly }, col, thick);
            }
            return;
        }

        const float rounding = style == esp::kBoxRounded ? 5.0f : 0.0f;
        dl->AddRect(mn, mx, col, rounding, 0, thick);
    }

    // Layered translucent strokes read as a soft outer glow.
    void GlowBox(ImDrawList* dl, int style,
                 ImVec2 mn, ImVec2 mx, ImU32 col)
    {
        static const float kExpand[3] = { 1.6f, 3.2f, 5.0f };
        static const float kAlpha[3]  = { 0.18f, 0.10f, 0.055f };
        static const float kThick[3]  = { 3.6f, 4.6f, 5.6f };
        for (int i = 2; i >= 0; --i)
            StrokeBox(dl, style, mn, mx, ScaleAlpha(col, kAlpha[i]),
                      kThick[i], kExpand[i]);
    }

    void GlowLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col)
    {
        static const float kAlpha[3] = { 0.22f, 0.12f, 0.06f };
        static const float kThick[3] = { 3.2f, 4.6f, 6.0f };
        for (int i = 2; i >= 0; --i)
            dl->AddLine(a, b, ScaleAlpha(col, kAlpha[i]), kThick[i]);
    }

    void DrawSkeleton(const PlayerData& p)
    {
        Vector3 bones[kMaxBone] = {};
        bool valid[kMaxBone] = {};

        const ImU32 boneCol =
            ImGui::GetColorU32(esp::g_settings.skeletonColor);

        for (const BonePair& pair : kBonePairs)
        {
            if (!valid[pair.a]) valid[pair.a] = GetBone(p, pair.a, &bones[pair.a]);
            if (!valid[pair.b]) valid[pair.b] = GetBone(p, pair.b, &bones[pair.b]);
            if (!valid[pair.a] || !valid[pair.b])
                continue;

            // Single-bone length sanity: the longest human bone (femur) is
            // ~18 units; reject cross-body garbage from stale index tables.
            const float boneLength = Distance3D(bones[pair.a], bones[pair.b]);
            if (boneLength < 1.5f || boneLength > 26.0f)
                continue;

            ImVec2 a, b;
            if (WorldToScreen(*p.vm, bones[pair.a], p.screenSize, &a) &&
                WorldToScreen(*p.vm, bones[pair.b], p.screenSize, &b))
            {
                if (esp::g_settings.skeletonGlow)
                {
                    GlowLine(p.drawList, a, b, boneCol);
                }
                p.drawList->AddLine(a, b, kShadowCol, 2.6f);
                p.drawList->AddLine(a, b, boneCol, 1.5f);
            }
        }

        if (esp::g_settings.skeletonJoints)
        {
            for (int i = 0; i < kMaxBone; ++i)
            {
                if (!valid[i])
                    continue;
                ImVec2 pt;
                if (!WorldToScreen(*p.vm, bones[i], p.screenSize, &pt))
                    continue;
                p.drawList->AddCircleFilled(pt, 2.4f, kShadowCol);
                p.drawList->AddCircleFilled(pt, 1.7f,
                    ScaleAlpha(boneCol, 0.95f));
            }
        }
    }

    bool DrawPlayer(const PlayerData& player)
    {
        // Feet = absolute origin; head = head bone (7), fallback origin+72u.
        Vector3 headWorld{};
        if (!GetBone(player, 7, &headWorld))
        {
            headWorld = player.origin;
            headWorld.z += kStandingHeight;
        }
        else
        {
            headWorld.z += 6.0f; // head joint -> crown of the head
        }

        ImVec2 feet2D, head2D;
        if (!WorldToScreen(*player.vm, player.origin, player.screenSize, &feet2D) ||
            !WorldToScreen(*player.vm, headWorld, player.screenSize, &head2D))
        {
            return false;
        }

        const float boxHeight = feet2D.y - head2D.y;
        if (boxHeight < 4.0f || boxHeight > player.screenSize.y * 1.5f)
            return false;
        const float boxWidth = boxHeight * 0.40f;

        const ImVec2 boxMin(head2D.x - boxWidth * 0.5f, head2D.y);
        const ImVec2 boxMax(head2D.x + boxWidth * 0.5f, feet2D.y);

        const float clampedHealth =
            player.health < 0 ? 0.0f :
            player.health > 100 ? 100.0f : static_cast<float>(player.health);
        const float fraction = clampedHealth / 100.0f;

        const ImU32 accent  = AccentColorU32(fraction, 1.0f);
        const ImU32 hltCol  = HealthColorU32(fraction, 1.0f);

        // ---- Layer order: fill, skeleton, glow, box outline, bars, text.

        if (esp::g_settings.boxStyle != esp::kBoxOff &&
            esp::g_settings.boxFill)
        {
            // Dark vertical gradient body (slightly inset so glow owns the
            // outside and the outline stays crisp).
            const ImU32 fillTop = IM_COL32(8, 10, 16, 46);
            const ImU32 fillBot = IM_COL32(8, 10, 16, 100);
            player.drawList->AddRectFilledMultiColor(
                boxMin, boxMax, fillTop, fillTop, fillBot, fillBot);
        }

        if (esp::g_settings.skeleton)
            DrawSkeleton(player);

        if (esp::g_settings.boxStyle != esp::kBoxOff)
        {
            if (esp::g_settings.boxGlow)
                GlowBox(player.drawList, esp::g_settings.boxStyle,
                        boxMin, boxMax, accent);

            // Dark contrast stroke just outside, then the accent outline.
            StrokeBox(player.drawList, esp::g_settings.boxStyle,
                      boxMin, boxMax, IM_COL32(0, 0, 0, 210), 2.4f, 0.9f);
            StrokeBox(player.drawList, esp::g_settings.boxStyle,
                      boxMin, boxMax, accent, 1.4f, 0.0f);
        }

        if (esp::g_settings.healthBar)
        {
            const ImVec2 barMin(boxMin.x - 6.0f, boxMin.y);
            const ImVec2 barMax(boxMin.x - 2.0f, boxMax.y);

            player.drawList->AddRectFilled(
                ImVec2(barMin.x - 1.0f, barMin.y - 1.0f),
                ImVec2(barMax.x + 1.0f, barMax.y + 1.0f),
                IM_COL32(0, 0, 0, 220), 2.0f);

            const float fillHeight = (barMax.y - barMin.y) * fraction;
            if (fillHeight > 0.5f)
            {
                // Vertical gradient: brighter health color at the top edge.
                const ImU32 top = ScaleAlpha(hltCol, 1.0f);
                const ImU32 bot = ScaleAlpha(hltCol, 0.62f);
                player.drawList->AddRectFilledMultiColor(
                    ImVec2(barMin.x, barMax.y - fillHeight),
                    ImVec2(barMax.x, barMax.y),
                    top, top, bot, bot);
            }

            if (esp::g_settings.hpNumber)
            {
                char hpBuffer[8];
                sprintf_s(hpBuffer, "%d", static_cast<int>(clampedHealth));
                const ImVec2 ts = ImGui::CalcTextSize(hpBuffer);
                const ImVec2 tp(barMin.x - ts.x - 3.0f,
                                barMin.y - ts.y - 1.0f);
                player.drawList->AddText(
                    ImVec2(tp.x + 1.0f, tp.y + 1.0f),
                    IM_COL32(0, 0, 0, 220), hpBuffer);
                player.drawList->AddText(tp, hltCol, hpBuffer);
            }
        }

        if (esp::g_settings.headRing)
        {
            float radius = boxHeight * 0.115f;
            if (radius < 3.0f) radius = 3.0f;
            if (radius > 14.0f) radius = 14.0f;
            if (esp::g_settings.boxGlow)
                player.drawList->AddCircle(head2D, radius + 1.6f,
                    ScaleAlpha(accent, 0.22f), 20, 3.2f);
            player.drawList->AddCircle(head2D, radius + 0.8f,
                IM_COL32(0, 0, 0, 210), 20, 2.4f);
            player.drawList->AddCircle(head2D, radius, accent, 20, 1.3f);
        }

        if (esp::g_settings.snapline)
        {
            const float anchorX = head2D.x;
            float anchorY = boxMax.y;
            if (esp::g_settings.snapOrigin == esp::kSnapCenter)
                anchorY = (boxMin.y + boxMax.y) * 0.5f;
            else if (esp::g_settings.snapOrigin == esp::kSnapTop)
                anchorY = boxMin.y;

            float edgeX = player.screenSize.x * 0.5f;
            float edgeY = player.screenSize.y;
            if (esp::g_settings.snapOrigin == esp::kSnapTop)
                edgeY = 0.0f;
            else if (esp::g_settings.snapOrigin == esp::kSnapCenter)
                edgeY = player.screenSize.y * 0.5f;
            const ImVec2 edge(edgeX, edgeY);
            const ImVec2 anchor(anchorX, anchorY);
            const ImU32 snapCol = ImGui::GetColorU32(
                esp::g_settings.snapColor);
            player.drawList->AddLine(edge, anchor,
                                     IM_COL32(0, 0, 0, 150), 2.0f);
            player.drawList->AddLine(edge, anchor, snapCol, 1.0f);
        }

        if (esp::g_settings.name && player.name[0] != '\0')
        {
            const ImVec2 textSize = ImGui::CalcTextSize(player.name);
            const ImVec2 textPos(head2D.x - textSize.x * 0.5f,
                                 boxMin.y - textSize.y - 2.0f);
            player.drawList->AddText(
                ImVec2(textPos.x + 1.0f, textPos.y + 1.0f),
                IM_COL32(0, 0, 0, 220), player.name);
            player.drawList->AddText(textPos, IM_COL32(255, 255, 255, 255),
                                     player.name);
        }

        if (esp::g_settings.distanceText)
        {
            char distanceBuffer[24];
            sprintf_s(distanceBuffer, "%.0fm", player.distance);
            const ImVec2 textSize = ImGui::CalcTextSize(distanceBuffer);
            const ImVec2 textPos(head2D.x - textSize.x * 0.5f,
                                 boxMax.y + 2.0f);
            player.drawList->AddText(
                ImVec2(textPos.x + 1.0f, textPos.y + 1.0f),
                IM_COL32(0, 0, 0, 220), distanceBuffer);
            player.drawList->AddText(textPos, IM_COL32(220, 225, 235, 255),
                                     distanceBuffer);
        }

        return true;
    }
}

namespace
{
    // Per-frame funnel counters so the on-screen debug panel can show
    // exactly which filter stage drops the entities.
    struct SlotDebug
    {
        int       slot = 0;
        char      className[24] = {};
        uint32_t  handle = 0;
        char      mask = '-'; // which handle mask first resolved: '7','F','3'
        uintptr_t pawn = 0;
    };

    struct DebugStats
    {
        uintptr_t clientBase      = 0;
        uintptr_t entitySystem    = 0;
        uintptr_t localPawn       = 0;
        uintptr_t localController = 0;
        int       goldSlot        = -1; // slot containing localController
        uint32_t  goldHandle      = 0;  // local ctrl m_hPlayerPawn raw value
        uintptr_t goldPawn        = 0;  // resolved pawn of local controller
        int       localTeam       = 0;
        float     m00 = 0.0f, m03 = 0.0f, m13 = 0.0f, m33 = 0.0f;
        bool      matrixValid     = false;
        int       entitiesFound   = 0; // non-null entity slots in 1..64
        int       controllers     = 0; // slots whose designerName matches
        int       pawnResolved    = 0; // controller pawn handle resolved to entity
        int       alive           = 0; // lifeState alive + health > 0
        int       enemy           = 0; // passed team filter
        int       nonDormant      = 0; // passed dormant filter
        int       inRange         = 0; // passed distance filter
        int       drawn           = 0; // actually produced draw commands
        SlotDebug slots[14] = {};

        // Local-player bone diagnostics: height (z) and horizontal radius
        // relative to the feet origin, for every slot in 0..kMaxBone-1.
        float     boneDz[kMaxBone] = {};
        float     boneR[kMaxBone]  = {};
        bool      boneOk[kMaxBone] = {};
    };

    // CHandle index width varies across Source 2 builds; try the standard
    // 15-bit mask first, then wider fallbacks. Returns first readable entity.
    uintptr_t ResolvePawnHandle(uintptr_t entitySystem, uint32_t handle,
                                char* maskUsed)
    {
        static const uint32_t kMasks[] = { 0x7FFFu, 0xFFFFu, 0x3FFFFu };
        static const char     kNames[] = { '7', 'F', '3' };

        for (int m = 0; m < 3; ++m)
        {
            const int index = static_cast<int>(handle & kMasks[m]);
            if (index == 0)
                continue;
            const uintptr_t entity = GetEntity(entitySystem, index);
            if (entity != 0)
            {
                if (maskUsed != nullptr)
                    *maskUsed = kNames[m];
                return entity;
            }
        }
        if (maskUsed != nullptr)
            *maskUsed = '-';
        return 0;
    }

    void DrawDebugOverlay(const DebugStats& stats, ImDrawList* drawList)
    {
        char lines[48][176];
        int lineCount = 0;

        sprintf_s(lines[lineCount++], "MifiInj ESP debug");
        sprintf_s(lines[lineCount++], "client.dll    : %p", reinterpret_cast<void*>(stats.clientBase));
        sprintf_s(lines[lineCount++], "entity system : %p %s",
                  reinterpret_cast<void*>(stats.entitySystem),
                  stats.entitySystem ? "" : "(NULL - dwEntityList deref failed)");
        sprintf_s(lines[lineCount++],
                  "view matrix   : %s [m00=%.2f m03=%.2f m13=%.2f m33=%.2f]",
                  stats.matrixValid ? "ok" : "INVALID",
                  stats.m00, stats.m03, stats.m13, stats.m33);
        sprintf_s(lines[lineCount++], "local pawn    : %p (team %d)",
                  reinterpret_cast<void*>(stats.localPawn), stats.localTeam);
        const int anchorLine = lineCount;
        sprintf_s(lines[lineCount++],
                  "local ctrl    : %p @ slot %d h=%08X -> %p %s",
                  reinterpret_cast<void*>(stats.localController),
                  stats.goldSlot, stats.goldHandle,
                  reinterpret_cast<void*>(stats.goldPawn),
                  stats.goldPawn == stats.localPawn ? "MATCH" : "(no match)");
        const int layoutLine = lineCount;
        if (g_layout.discovered)
        {
            sprintf_s(lines[lineCount++],
                      "layout        : table=+0x%zX stride=0x%zX inst=+0x%zX"
                      " (ctrl idx %d, pawn idx %d)",
                      g_layout.chunkTableOffset, g_layout.identityStride,
                      g_layout.instanceOffset,
                      g_layout.controllerIndex, g_layout.pawnIndex);
        }
        else
        {
            sprintf_s(lines[lineCount++],
                      "layout        : NOT discovered (defaults"
                      " table=+0x%zX stride=0x%zX)",
                      g_layout.chunkTableOffset, g_layout.identityStride);
        }
        sprintf_s(lines[lineCount++],
                  "scan hits     : ctrl@0x%zX pawn@0x%zX chunk=%p",
                  g_layout.ctrlHitOffset, g_layout.pawnHitOffset,
                  reinterpret_cast<void*>(g_layout.chunk0));
        sprintf_s(lines[lineCount++], "entity slots  : %d", stats.entitiesFound);
        sprintf_s(lines[lineCount++], "controllers   : %d", stats.controllers);
        sprintf_s(lines[lineCount++], "pawns resolved: %d", stats.pawnResolved);
        sprintf_s(lines[lineCount++], "alive         : %d", stats.alive);
        sprintf_s(lines[lineCount++], "enemies       : %d", stats.enemy);
        sprintf_s(lines[lineCount++], "non-dormant   : %d", stats.nonDormant);
        sprintf_s(lines[lineCount++], "in range      : %d", stats.inRange);
        const int drawnLineActual = lineCount;
        sprintf_s(lines[lineCount++], "DRAWN         : %d", stats.drawn);

        const int slotsHeaderLine = lineCount;
        sprintf_s(lines[lineCount++], "-- non-null slots: class [idx] handle --");
        for (const SlotDebug& slot : stats.slots)
        {
            if (slot.slot == 0)
                break;
            sprintf_s(lines[lineCount++],
                      "%-22s [%2d] h=%08X m=%c p=%p",
                      slot.className[0] ? slot.className : "?",
                      slot.slot, slot.handle, slot.mask,
                      reinterpret_cast<void*>(slot.pawn));
        }

        // Bone index discovery table: z = height above feet, r = horizontal
        // distance from spine axis (units). 4 slots per line.
        const int boneHeaderLine = lineCount;
        sprintf_s(lines[lineCount++],
                  "-- local bones [idx] z=height r=radius --");
        for (int base = 0; base < kMaxBone; base += 4)
        {
            char text[176] = "";
            for (int b = base; b < base + 4 && b < kMaxBone; ++b)
            {
                char cell[40];
                if (stats.boneOk[b])
                    sprintf_s(cell, "[%02d] z%02.0f r%02.0f  ",
                              b, stats.boneDz[b], stats.boneR[b]);
                else
                    sprintf_s(cell, "[%02d] ----       ", b);
                strcat_s(text, sizeof(text), cell);
            }
            sprintf_s(lines[lineCount++], "%s", text);
        }

        const ImVec2 origin(12.0f, 12.0f);
        const float  lineHeight = ImGui::GetTextLineHeight();
        const float  pad = 6.0f;
        float maxWidth = 0.0f;
        for (int i = 0; i < lineCount; ++i)
            maxWidth = (std::max)(maxWidth, ImGui::CalcTextSize(lines[i]).x);

        drawList->AddRectFilled(
            ImVec2(origin.x - pad, origin.y - pad),
            ImVec2(origin.x + maxWidth + pad,
                   origin.y + lineHeight * lineCount + pad),
            IM_COL32(0, 0, 0, 200), 4.0f);

        for (int i = 0; i < lineCount; ++i)
        {
            ImU32 color = IM_COL32(230, 230, 230, 255);
            if (i == 0)
                color = IM_COL32(120, 180, 255, 255);
            else if (i == anchorLine)
                color = stats.goldPawn == stats.localPawn
                            ? IM_COL32(120, 230, 120, 255)
                            : IM_COL32(240, 150, 150, 255);
            else if (i == layoutLine)
                color = g_layout.discovered
                            ? IM_COL32(120, 230, 120, 255)
                            : IM_COL32(240, 200, 90, 255);
            else if (i == drawnLineActual)
                color = stats.drawn > 0 ? IM_COL32(120, 230, 120, 255)
                                        : IM_COL32(240, 200, 90, 255);
            else if (i >= slotsHeaderLine && i < boneHeaderLine)
                color = IM_COL32(180, 210, 255, 255);
            else if (i >= boneHeaderLine)
                color = IM_COL32(150, 235, 160, 255);
            drawList->AddText(ImVec2(origin.x, origin.y + lineHeight * i),
                              color, lines[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Aimbot
// ---------------------------------------------------------------------------
namespace aimbot
{
    Settings g_settings;

    namespace
    {
        struct QAngle { float pitch, yaw, roll; };

        constexpr float kPi       = 3.14159265358979323846f;
        constexpr float kDeg2Rad  = kPi / 180.0f;
        constexpr float kRad2Deg  = 180.0f / kPi;

        // Render thread -> usercmd entry thread command channel (seqlock).
        struct SilentShared
        {
            volatile std::int64_t seq = 0; // odd while the writer updates
            int                   active = 0;
            float                 pitch = 0.0f;
            float                 yaw = 0.0f;
            int                   target = 0;
            std::uint64_t         stampMs = 0;
        };
        SilentShared g_silent;

        void PublishSilent(bool active, float pitch, float yaw, int target)
        {
            InterlockedIncrement64(
                reinterpret_cast<volatile long long*>(&g_silent.seq));
            g_silent.active = active ? 1 : 0;
            g_silent.pitch  = pitch;
            g_silent.yaw    = yaw;
            g_silent.target = target;
            g_silent.stampMs = GetTickCount64();
            InterlockedIncrement64(
                reinterpret_cast<volatile long long*>(&g_silent.seq));
        }

        // Resolve the pawn entity index whose list slot holds `pawn`
        // (CHandle index width varies across builds).
        int ResolvePawnIndex(uintptr_t entitySystem, uint32_t handle,
                             uintptr_t pawn)
        {
            static const uint32_t kMasks[] = { 0x7FFFu, 0xFFFFu, 0x3FFFFu };
            for (uint32_t mask : kMasks)
            {
                const int index = static_cast<int>(handle & mask);
                if (index > 0 && GetEntity(entitySystem, index) == pawn)
                    return index;
            }
            return -1;
        }

        float NormalizeYaw(float yaw)
        {
            while (yaw > 180.0f)  yaw -= 360.0f;
            while (yaw < -180.0f) yaw += 360.0f;
            return yaw;
        }

        int TargetBoneIndex(int selector)
        {
            switch (selector)
            {
            case 1:  return 6;  // neck
            case 2:  return 2;  // pelvis
            default: return 7;  // head
            }
        }

        // Raw bone slot read with the human-box sanity check (same as ESP).
        bool ReadBonePos(uintptr_t boneArray, int index, const Vector3& origin,
                         Vector3* out)
        {
            if (boneArray == 0)
                return false;
            if (!SafeRead(reinterpret_cast<void*>(
                              boneArray +
                              static_cast<uintptr_t>(index) * kBoneStride),
                          out))
            {
                return false;
            }
            const float dx = out->x - origin.x;
            const float dy = out->y - origin.y;
            const float dz = out->z - origin.z;
            if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz))
                return false;
            if (dz < -10.0f || dz > 85.0f)
                return false;
            if (std::fabs(dx) > 40.0f || std::fabs(dy) > 40.0f)
                return false;
            return true;
        }

        void DrawFovCircle(const ViewMatrix& vm, const ImVec2& screen)
        {
            // Projection m[1][1] = cot(verticalFov/2).
            if (vm.m[1][1] <= 0.01f || !std::isfinite(vm.m[1][1]))
                return;
            const float vFovDeg = 2.0f * std::atan(1.0f / vm.m[1][1]) * kRad2Deg;
            const float radius = g_settings.fov * (screen.y / vFovDeg);
            if (radius < 2.0f || radius > screen.y)
                return;

            ImDrawList* dl = ImGui::GetBackgroundDrawList();
            dl->AddCircle(ImVec2(screen.x * 0.5f, screen.y * 0.5f), radius,
                          ImGui::GetColorU32(g_settings.fovColor), 64, 1.2f);
        }
    }

    void Run()
    {
        const HMODULE clientModule = GetModuleHandleA("client.dll");
        if (clientModule == nullptr)
            return;

        const uintptr_t clientBase =
            reinterpret_cast<uintptr_t>(clientModule);

        uintptr_t entitySystem = 0;
        if (!SafeRead(reinterpret_cast<void*>(clientBase + kDwEntityList),
                      &entitySystem) ||
            !IsReadablePointer(entitySystem))
        {
            return;
        }

        ViewMatrix vm{};
        const bool vmValid =
            SafeRead(reinterpret_cast<void*>(clientBase + kDwViewMatrix), &vm);

        uintptr_t localPawn = 0, localController = 0;
        SafeRead(reinterpret_cast<void*>(clientBase + kDwLocalPlayerPawn),
                 &localPawn);
        SafeRead(reinterpret_cast<void*>(clientBase + kDwLocalPlayerCtrl),
                 &localController);
        if (!IsReadablePointer(localPawn))
            localPawn = 0;
        if (!IsReadablePointer(localController))
            localController = 0;

        DiscoverLayout(entitySystem, localController, localPawn);

        const ImVec2 screenSize = ImGui::GetIO().DisplaySize;
        if (g_settings.enabled && g_settings.drawFov && vmValid)
            DrawFovCircle(vm, screenSize);

        if (!g_settings.enabled || localPawn == 0 || !g_layout.discovered)
            return;

        if ((GetAsyncKeyState(g_settings.holdKey) & 0x8000) == 0)
        {
            if (g_settings.silentAim)
                PublishSilent(false, 0.0f, 0.0f, 0);
            return;
        }

        QAngle angles{};
        if (!SafeRead(reinterpret_cast<void*>(clientBase + kDwViewAngles),
                      &angles) ||
            !std::isfinite(angles.pitch) || !std::isfinite(angles.yaw))
        {
            return;
        }

        uint8_t localTeam = 0;
        SafeRead(reinterpret_cast<void*>(localPawn + kTeamNum), &localTeam);

        Vector3 localOrigin{};
        uintptr_t localScene = 0;
        if (SafeRead(reinterpret_cast<void*>(localPawn + kGameSceneNode),
                     &localScene) &&
            IsReadablePointer(localScene))
        {
            SafeRead(reinterpret_cast<void*>(localScene + kVecAbsOrigin),
                     &localOrigin);
        }
        Vector3 viewOffset{};
        SafeRead(reinterpret_cast<void*>(localPawn + kVecViewOffset),
                 &viewOffset);
        const Vector3 eye{ localOrigin.x + viewOffset.x,
                           localOrigin.y + viewOffset.y,
                           localOrigin.z + viewOffset.z };

        const int targetBone = TargetBoneIndex(g_settings.targetBone);

        bool  haveTarget = false;
        float bestScore  = g_settings.fov;
        float bestPitch  = 0.0f, bestYaw = 0.0f;
        float bestWantPitch = 0.0f, bestWantYaw = 0.0f;
        int   bestIndex  = -1;

        for (int i = 1; i <= kMaxControllers; ++i)
        {
            const uintptr_t controller = GetEntity(entitySystem, i);
            if (controller == 0)
                continue;

            char className[32] = {};
            GetDesignerName(controller, className, sizeof(className));
            if (strstr(className, "player_controller") == nullptr)
                continue;

            uint32_t pawnHandle = 0;
            SafeRead(reinterpret_cast<void*>(controller + kHPlayerPawn),
                     &pawnHandle);
            if (pawnHandle == 0 || pawnHandle == 0xFFFFFFFFu)
                continue;

            const uintptr_t pawn =
                ResolvePawnHandle(entitySystem, pawnHandle, nullptr);
            if (pawn == 0 || pawn == localPawn)
                continue;

            const int pawnIndex =
                ResolvePawnIndex(entitySystem, pawnHandle, pawn);

            int   health = 0;
            uint8_t lifeState = 1, team = 0, dormant = 1;
            SafeRead(reinterpret_cast<void*>(pawn + kHealth), &health);
            SafeRead(reinterpret_cast<void*>(pawn + kLifeState), &lifeState);
            SafeRead(reinterpret_cast<void*>(pawn + kTeamNum), &team);
            if (lifeState != 0 || health <= 0)
                continue;
            if (!g_settings.teammates && team == localTeam)
                continue;

            uintptr_t scene = 0;
            if (!SafeRead(reinterpret_cast<void*>(pawn + kGameSceneNode),
                          &scene) ||
                !IsReadablePointer(scene))
            {
                continue;
            }
            SafeRead(reinterpret_cast<void*>(scene + kDormant), &dormant);
            if (dormant)
                continue;

            Vector3 origin{};
            if (!SafeRead(reinterpret_cast<void*>(scene + kVecAbsOrigin),
                          &origin))
            {
                continue;
            }

            uintptr_t boneArray = 0;
            SafeRead(reinterpret_cast<void*>(
                         scene + kModelState + kModelBoneArray),
                     &boneArray);
            if (!IsReadablePointer(boneArray))
                continue;

            Vector3 aimPoint{};
            if (!ReadBonePos(boneArray, targetBone, origin, &aimPoint))
                continue;
            if (targetBone == 7)
                aimPoint.z += 4.0f; // joint -> head center

            const float dx = aimPoint.x - eye.x;
            const float dy = aimPoint.y - eye.y;
            const float dz = aimPoint.z - eye.z;
            const float dist2d = std::sqrt(dx * dx + dy * dy);
            if (dist2d < 1.0f)
                continue;

            const float wantYaw   = std::atan2(dy, dx) * kRad2Deg;
            const float wantPitch = -std::atan2(dz, dist2d) * kRad2Deg;

            const float dYaw   = NormalizeYaw(wantYaw - angles.yaw);
            const float dPitch = wantPitch - angles.pitch;

            // Angular distance from the crosshair, in degrees. Yaw is
            // foreshortened by pitch like a real FOV cone.
            const float score =
                std::sqrt(dPitch * dPitch +
                          (dYaw * std::cos(angles.pitch * kDeg2Rad)) *
                          (dYaw * std::cos(angles.pitch * kDeg2Rad)));
            if (score >= bestScore)
                continue;

            // Line of sight: ask the trace service for an eye->bone ray.
            // A blocked result hides the candidate; stale/unknown results
            // count as visible so aim never bricks while a trace result is
            // still in flight or the service could not install.
            if (g_settings.visibleCheck && pawnIndex > 0)
            {
                const vis::Vec from{ eye.x, eye.y, eye.z };
                const vis::Vec to{ aimPoint.x, aimPoint.y, aimPoint.z };
                vis::PostEntity(pawnIndex, from, to);
                const vis::Result los = vis::GetEntity(pawnIndex, 250);
                if (los.fresh && los.blocked)
                    continue;
            }

            bestScore     = score;
            haveTarget    = true;
            bestPitch     = dPitch;
            bestYaw       = dYaw;
            bestWantPitch = wantPitch;
            bestWantYaw   = wantYaw;
            bestIndex     = pawnIndex;
        }

        if (!haveTarget)
        {
            if (g_settings.silentAim)
                PublishSilent(false, 0.0f, 0.0f, 0);
            return;
        }

        if (g_settings.silentAim)
        {
            // Snap the outgoing COMMAND angles; the camera global is never
            // written. The usercmd entry hook consumes this.
            float wantPitch = bestWantPitch;
            if (wantPitch > 89.0f)  wantPitch = 89.0f;
            if (wantPitch < -89.0f) wantPitch = -89.0f;
            PublishSilent(true, wantPitch, NormalizeYaw(bestWantYaw),
                          bestIndex);
            return;
        }

        const float factor =
            1.0f / (std::max)(1.0f, g_settings.smoothing);

        float newPitch = angles.pitch + bestPitch * factor;
        float newYaw   = NormalizeYaw(angles.yaw + bestYaw * factor);

        if (newPitch > 89.0f)  newPitch = 89.0f;
        if (newPitch < -89.0f) newPitch = -89.0f;

        const QAngle result{ newPitch, newYaw, 0.0f };
        SafeWrite(reinterpret_cast<void*>(clientBase + kDwViewAngles),
                  result);
    }

    bool GetSilentCommand(SilentCommand* out)
    {
        if (out == nullptr)
            return false;

        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const std::int64_t s1 = g_silent.seq;
            if ((s1 & 1LL) != 0)
            {
                SwitchToThread();
                continue;
            }

            SilentCommand tmp;
            tmp.active  = g_silent.active != 0;
            tmp.pitch   = g_silent.pitch;
            tmp.yaw     = g_silent.yaw;
            tmp.target  = g_silent.target;
            tmp.stampMs = g_silent.stampMs;

            MemoryBarrier();
            const std::int64_t s2 = g_silent.seq;
            if (s1 == s2 && (s2 & 1LL) == 0)
            {
                *out = tmp;
                return true;
            }
        }
        return false;
    }
}

namespace esp
{
    Settings g_settings;

    EntityLayoutInfo GetEntityLayout()
    {
        return {
            g_layout.chunkTableOffset,
            g_layout.identityStride,
            g_layout.instanceOffset,
            g_layout.discovered,
        };
    }

    void Render()
    {
        const HMODULE clientModule = GetModuleHandleA("client.dll");
        if (clientModule == nullptr)
            return;

        // Entity-layout discovery is cheap once resolved and must run even
        // when ESP drawing is off: other modules (overrides) depend on it.
        if (!g_settings.enabled)
        {
            const uintptr_t cb =
                reinterpret_cast<uintptr_t>(clientModule);
            uintptr_t es = 0, lp = 0, lc = 0;
            if (SafeRead(reinterpret_cast<void*>(cb + kDwEntityList), &es) &&
                IsReadablePointer(es))
            {
                SafeRead(reinterpret_cast<void*>(cb + kDwLocalPlayerCtrl), &lc);
                SafeRead(reinterpret_cast<void*>(cb + kDwLocalPlayerPawn), &lp);
                DiscoverLayout(es,
                               IsReadablePointer(lc) ? lc : 0,
                               IsReadablePointer(lp) ? lp : 0);
            }
            return;
        }

        DebugStats stats{};
        stats.clientBase = reinterpret_cast<uintptr_t>(clientModule);

        // dwEntityList holds a POINTER to the CGameEntitySystem singleton.
        // (cs2-dumper maps dwGameEntitySystem/dwEntityList to the same RVA.)
        if (!SafeRead(reinterpret_cast<void*>(stats.clientBase + kDwEntityList),
                      &stats.entitySystem) ||
            !IsReadablePointer(stats.entitySystem))
        {
            stats.entitySystem = 0;
        }

        ViewMatrix vm{};
        stats.matrixValid =
            SafeRead(reinterpret_cast<void*>(stats.clientBase + kDwViewMatrix), &vm);
        stats.m00 = vm.m[0][0];
        stats.m03 = vm.m[0][3];
        stats.m13 = vm.m[1][3];
        stats.m33 = vm.m[3][3];

        SafeRead(reinterpret_cast<void*>(stats.clientBase + kDwLocalPlayerPawn),
                 &stats.localPawn);
        if (!IsReadablePointer(stats.localPawn))
            stats.localPawn = 0;

        // Gold anchor: the game directly exposes the local player controller.
        SafeRead(reinterpret_cast<void*>(stats.clientBase + kDwLocalPlayerCtrl),
                 &stats.localController);
        if (!IsReadablePointer(stats.localController))
            stats.localController = 0;

        // Auto-discover the real entity-list layout via the two anchors.
        DiscoverLayout(stats.entitySystem, stats.localController,
                       stats.localPawn);

        const ImVec2 screenSize = ImGui::GetIO().DisplaySize;
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        // Without localPawn/entitySystem/matrix nothing downstream can work;
        // still draw the debug panel so the failing stage is visible.
        if (stats.localPawn == 0 || stats.entitySystem == 0 ||
            !stats.matrixValid)
        {
            if (g_settings.debug)
                DrawDebugOverlay(stats, drawList);
            return;
        }

        SafeRead(reinterpret_cast<void*>(stats.localPawn + kTeamNum),
                 &stats.localTeam);

        Vector3 localOrigin{};
        uintptr_t localBoneArray = 0;
        {
            uintptr_t scene = 0;
            if (SafeRead(reinterpret_cast<void*>(stats.localPawn + kGameSceneNode),
                         &scene) &&
                IsReadablePointer(scene))
            {
                SafeRead(reinterpret_cast<void*>(scene + kVecAbsOrigin),
                         &localOrigin);
                SafeRead(reinterpret_cast<void*>(
                             scene + kModelState + kModelBoneArray),
                         &localBoneArray);
                if (!IsReadablePointer(localBoneArray))
                    localBoneArray = 0;
            }
        }

        // Sample every bone slot of the local player for index discovery.
        if (localBoneArray != 0)
        {
            for (int b = 0; b < kMaxBone; ++b)
            {
                Vector3 pos{};
                if (!SafeRead(reinterpret_cast<void*>(
                                  localBoneArray +
                                  static_cast<uintptr_t>(b) * kBoneStride),
                              &pos))
                {
                    continue;
                }
                const float dx = pos.x - localOrigin.x;
                const float dy = pos.y - localOrigin.y;
                const float dz = pos.z - localOrigin.z;
                if (std::isfinite(dx) && std::isfinite(dy) &&
                    std::isfinite(dz) &&
                    dz >= -10.0f && dz <= 85.0f &&
                    std::fabs(dx) <= 40.0f && std::fabs(dy) <= 40.0f)
                {
                    stats.boneOk[b] = true;
                    stats.boneDz[b] = dz;
                    stats.boneR[b]  = std::sqrt(dx * dx + dy * dy);
                }
            }
        }

        for (int i = 1; i <= kMaxControllers; ++i)
        {
            const uintptr_t entity = GetEntity(stats.entitySystem, i);
            if (entity == 0)
                continue;
            ++stats.entitiesFound;

            // Identify the entity by its schema designer name.
            char className[32] = {};
            GetDesignerName(entity, className, sizeof(className));
            const bool isController =
                strstr(className, "player_controller") != nullptr;

            uint32_t pawnHandle = 0xFFFFFFFFu;
            SafeRead(reinterpret_cast<void*>(entity + kHPlayerPawn),
                     &pawnHandle);
            char usedMaskStorage = '-';
            const uintptr_t resolvedPawn =
                ResolvePawnHandle(stats.entitySystem, pawnHandle,
                                  &usedMaskStorage);

            // Record the first non-null slots regardless of class.
            if (stats.entitiesFound <= 14)
            {
                SlotDebug& row = stats.slots[stats.entitiesFound - 1];
                row.slot = i;
                strncpy_s(row.className, className, _TRUNCATE);
                row.handle = pawnHandle;
                row.mask   = usedMaskStorage;
                row.pawn   = resolvedPawn;
            }

            // Gold-anchor bookkeeping for the local player controller.
            if (stats.localController != 0 && entity == stats.localController)
            {
                stats.goldSlot  = i;
                stats.goldHandle = pawnHandle;
                stats.goldPawn  = resolvedPawn;
            }

            // Only actual player controllers participate in ESP.
            if (!isController)
                continue;
            ++stats.controllers;

            if (pawnHandle == 0 || pawnHandle == 0xFFFFFFFFu)
                continue;

            const uintptr_t pawn = resolvedPawn;
            if (pawn == 0 || pawn == stats.localPawn)
                continue;
            ++stats.pawnResolved;

            int health = 0;
            uint8_t lifeState = 1, team = 0, dormant = 1;
            uintptr_t scene = 0;
            Vector3 origin{};

            SafeRead(reinterpret_cast<void*>(pawn + kHealth), &health);
            SafeRead(reinterpret_cast<void*>(pawn + kLifeState), &lifeState);
            SafeRead(reinterpret_cast<void*>(pawn + kTeamNum), &team);

            if (lifeState != 0 || health <= 0)
                continue;
            ++stats.alive;

            if (!g_settings.teammates && team == stats.localTeam)
                continue;
            ++stats.enemy;

            if (!SafeRead(reinterpret_cast<void*>(pawn + kGameSceneNode),
                          &scene) ||
                !IsReadablePointer(scene))
            {
                continue;
            }
            SafeRead(reinterpret_cast<void*>(scene + kDormant), &dormant);
            if (g_settings.hideDormant && dormant)
                continue;
            ++stats.nonDormant;

            if (!SafeRead(reinterpret_cast<void*>(scene + kVecAbsOrigin), &origin))
                continue;

            const float dx = origin.x - localOrigin.x;
            const float dy = origin.y - localOrigin.y;
            const float dz = origin.z - localOrigin.z;
            const float distanceMeters =
                std::sqrt(dx * dx + dy * dy + dz * dz) / kUnitsPerMeter;
            if (g_settings.maxDistance > 0.0f &&
                distanceMeters > g_settings.maxDistance)
            {
                continue;
            }
            ++stats.inRange;

            PlayerData data{};
            data.boneArray = [&]() -> uintptr_t {
                uintptr_t bones = 0;
                SafeRead(reinterpret_cast<void*>(
                             scene + kModelState + kModelBoneArray),
                         &bones);
                return IsReadablePointer(bones) ? bones : 0;
            }();
            data.origin     = origin;
            data.health     = health;
            data.distance   = distanceMeters;
            data.vm         = &vm;
            data.screenSize = screenSize;
            data.drawList   = drawList;
            SafeReadString(reinterpret_cast<void*>(entity + kPlayerName),
                           data.name, sizeof(data.name));

            if (DrawPlayer(data))
                ++stats.drawn;
        }

        if (g_settings.debug)
            DrawDebugOverlay(stats, drawList);
    }
}

// ---------------------------------------------------------------------------
// Trigger bot (implementation lives here to share the entity/bone helpers)
//
// Target acquisition is geometric (the REAL camera ray must pass through an
// enemy bone sphere), and visibility is proven the same way as the aimbot:
// an eye->bone LOS probe through the vis service. We deliberately do NOT
// require the borrowed game trace to hit the enemy -- movement/ground traces
// use a world-only collision mask and pass through players. The LOS probe
// only has to distinguish wall geometry, which every movement mask
// contains, so occluded targets never fire while open ones do.
// ---------------------------------------------------------------------------
namespace triggerbot
{
    Settings g_settings;

    namespace
    {
        constexpr uintptr_t kDwAttackButton = 0x2231FD0;

        constexpr std::int32_t kAttackPress   = 65537; // 0x10001
        constexpr std::int32_t kAttackRelease = 256;   // 0x100

        constexpr float kPi2      = 3.14159265358979323846f;
        constexpr float kDeg2Rad2 = kPi2 / 180.0f;
        constexpr float kRad2Deg2 = 180.0f / kPi2;

        // Bone spheres the crosshair ray may pass through.
        constexpr float kHeadRadius = 11.0f;
        constexpr float kBodyRadius = 16.5f;
        constexpr int   kBones[] = { 7, 6, 5, 4, 3, 2 };

        // LOS result must be younger than this (a probe runs every game
        // tick via the borrowed movement traces, ~16ms).
        constexpr std::uint32_t kLosMaxAgeMs = 120;

        struct Target
        {
            int         pawnIndex = -1;
            Vector3     bone{};
            float       angular = 360.0f; // degrees off the crosshair
        };

        enum State { kIdle = 0, kWaiting = 1, kHolding = 2 };

        Diag   g_diag;
        State  g_state       = kIdle;
        bool   g_buttonHeld  = false;
        long long g_fireAtQpc = 0;
        unsigned long long g_rng = 0x9E3779B97F4A7C15ull;

        bool BoneWorld(uintptr_t boneArray, int index, const Vector3& origin,
                       Vector3* out)
        {
            if (boneArray == 0)
                return false;
            if (!SafeRead(reinterpret_cast<void*>(
                              boneArray +
                              static_cast<uintptr_t>(index) * kBoneStride),
                          out))
                return false;
            const float dx = out->x - origin.x;
            const float dy = out->y - origin.y;
            const float dz = out->z - origin.z;
            if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz))
                return false;
            if (dz < -10.0f || dz > 85.0f)
                return false;
            if (std::fabs(dx) > 40.0f || std::fabs(dy) > 40.0f)
                return false;
            return true;
        }

        // Entity index whose identity slot holds `pawn` (handle width
        // varies across Source 2 builds).
        int ResolvePawnIndex(uintptr_t entitySystem, uint32_t handle,
                             uintptr_t pawn)
        {
            static const uint32_t kMasks[] = { 0x7FFFu, 0xFFFFu, 0x3FFFFu };
            for (uint32_t mask : kMasks)
            {
                const int index = static_cast<int>(handle & mask);
                if (index > 0 && GetEntity(entitySystem, index) == pawn)
                    return index;
            }
            return -1;
        }

        // Angular distance (degrees, foreshortened by pitch like a real FOV
        // cone) between the camera angles and the eye->point direction.
        float AngularDistance(const Vector3& eye, const Vector3& point,
                              float camPitch, float camYaw)
        {
            const float dx = point.x - eye.x;
            const float dy = point.y - eye.y;
            const float dz = point.z - eye.z;
            const float dist2d = std::sqrt(dx * dx + dy * dy);
            if (dist2d < 1.0f)
                return 360.0f;

            const float wantYaw   = std::atan2(dy, dx) * kRad2Deg2;
            const float wantPitch = -std::atan2(dz, dist2d) * kRad2Deg2;
            const float dYaw   = wantYaw - camYaw;
            const float dPitch = wantPitch - camPitch;
            const float yawAdj =
                dYaw - std::round(dYaw / 360.0f) * 360.0f;

            return std::sqrt(
                dPitch * dPitch +
                (yawAdj * std::cos(camPitch * kDeg2Rad2)) *
                    (yawAdj * std::cos(camPitch * kDeg2Rad2)));
        }

        long long QpcNow()
        {
            LARGE_INTEGER li{};
            QueryPerformanceCounter(&li);
            return static_cast<long long>(li.QuadPart);
        }

        long long MsToQpc(int ms)
        {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return (static_cast<long long>(ms) * f.QuadPart) / 1000;
        }

        int RandomDelayMs()
        {
            g_rng ^= g_rng << 13;
            g_rng ^= g_rng >> 7;
            g_rng ^= g_rng << 17;
            int lo = g_settings.delayMin;
            int hi = g_settings.delayMax;
            if (lo < 0) lo = 0;
            if (hi < lo) hi = lo;
            if (hi == lo)
                return lo;
            const unsigned span =
                static_cast<unsigned>(hi - lo) + 1u;
            return lo + static_cast<int>(g_rng % span);
        }

        void ForceRelease(uintptr_t clientBase)
        {
            if (g_buttonHeld)
            {
                SafeWrite(reinterpret_cast<void*>(clientBase + kDwAttackButton),
                          kAttackRelease);
                g_buttonHeld = false;
                ++g_diag.releases;
            }
        }

        // Phase 1: geometrically find the living, non-dormant enemy whose
        // bone sphere the camera ray passes through (no traces yet).
        // Phase 2: prove line of sight to that single point with one
        // eye->bone probe through the vis service. The LOS probe only has
        // to distinguish wall geometry (every movement trace mask contains
        // world), so it works even though the borrowed game traces use a
        // player-less mask.
        bool AcquireTarget(uintptr_t entitySystem, uintptr_t localPawn,
                           uint8_t localTeam, const Vector3& eye,
                           float camPitch, float camYaw)
        {
            Target best;

            for (int i = 1; i <= kMaxControllers; ++i)
            {
                const uintptr_t controller = GetEntity(entitySystem, i);
                if (controller == 0)
                    continue;

                char className[32] = {};
                GetDesignerName(controller, className, sizeof(className));
                if (strstr(className, "player_controller") == nullptr)
                    continue;

                uint32_t pawnHandle = 0;
                SafeRead(reinterpret_cast<void*>(controller + kHPlayerPawn),
                         &pawnHandle);
                if (pawnHandle == 0 || pawnHandle == 0xFFFFFFFFu)
                    continue;

                const uintptr_t pawn =
                    ResolvePawnHandle(entitySystem, pawnHandle, nullptr);
                if (pawn == 0 || pawn == localPawn)
                    continue;

                const int pawnIndex =
                    ResolvePawnIndex(entitySystem, pawnHandle, pawn);
                if (pawnIndex <= 0)
                    continue;

                int health = 0;
                uint8_t lifeState = 1, team = 0, dormant = 1;
                SafeRead(reinterpret_cast<void*>(pawn + kHealth), &health);
                SafeRead(reinterpret_cast<void*>(pawn + kLifeState), &lifeState);
                SafeRead(reinterpret_cast<void*>(pawn + kTeamNum), &team);
                if (lifeState != 0 || health <= 0)
                    continue;
                if (!g_settings.teammates && team == localTeam)
                    continue;

                uintptr_t scene = 0;
                if (!SafeRead(reinterpret_cast<void*>(pawn + kGameSceneNode),
                              &scene) ||
                    !IsReadablePointer(scene))
                    continue;
                SafeRead(reinterpret_cast<void*>(scene + kDormant), &dormant);
                if (dormant)
                    continue;

                Vector3 origin{};
                if (!SafeRead(reinterpret_cast<void*>(scene + kVecAbsOrigin),
                              &origin))
                    continue;

                uintptr_t boneArray = 0;
                SafeRead(reinterpret_cast<void*>(
                             scene + kModelState + kModelBoneArray),
                         &boneArray);
                if (!IsReadablePointer(boneArray))
                    continue;

                for (int bi = 0; bi < static_cast<int>(
                         sizeof(kBones) / sizeof(kBones[0])); ++bi)
                {
                    const int boneId = kBones[bi];
                    Vector3 p{};
                    if (!BoneWorld(boneArray, boneId, origin, &p))
                        continue;
                    if (boneId == 7)
                        p.z += 4.0f; // joint -> head center

                    const float radius =
                        boneId == 7 ? kHeadRadius : kBodyRadius;
                    const float dist =
                        std::sqrt((p.x - eye.x) * (p.x - eye.x) +
                                  (p.y - eye.y) * (p.y - eye.y) +
                                  (p.z - eye.z) * (p.z - eye.z));
                    if (dist < 1.0f)
                        continue;

                    const float angular =
                        AngularDistance(eye, p, camPitch, camYaw);
                    if (angular >= best.angular)
                        continue;

                    // Bone sphere must cover the crosshair: ray within the
                    // angular radius of the sphere. Clamp so point-blank
                    // and extreme range stay sane.
                    float threshold = std::atan2(radius, dist) * kRad2Deg2;
                    if (threshold < 0.3f) threshold = 0.3f;
                    if (threshold > 6.0f) threshold = 6.0f;
                    if (angular > threshold)
                        continue;

                    best.pawnIndex = pawnIndex;
                    best.bone = p;
                    best.angular = angular;
                }
            }

            if (best.pawnIndex <= 0)
                return false;

            // Phase 2: one probe per frame, read the latest serviced result.
            vis::PostEntity(
                best.pawnIndex,
                { eye.x, eye.y, eye.z },
                { best.bone.x, best.bone.y, best.bone.z });
            const vis::Result los =
                vis::GetEntity(best.pawnIndex, kLosMaxAgeMs);

            g_diag.traceFresh = los.fresh;
            g_diag.traceBlocked = los.fresh && los.blocked;

            // Hard visibility guarantee: unknown/stale results do NOT fire;
            // blocked results do not fire.
            if (!los.fresh || los.blocked)
                return false;

            g_diag.nearest = best.angular;
            return true;
        }
    }

    void Poll()
    {
        g_diag.keyHeld = false;
        g_diag.pawnOk = false;
        g_diag.traceFresh = false;
        g_diag.traceBlocked = false;
        g_diag.enemyUnderCrosshair = false;
        g_diag.nearest = -1.0f;

        const HMODULE clientModule = GetModuleHandleA("client.dll");
        if (clientModule == nullptr)
            return;
        const uintptr_t clientBase =
            reinterpret_cast<uintptr_t>(clientModule);

        if (!g_settings.enabled)
        {
            if (g_state != kIdle || g_buttonHeld)
            {
                ForceRelease(clientBase);
                g_state = kIdle;
            }
            return;
        }

        // Do not react while the menu eats input.
        if (ImGui::GetIO().WantCaptureMouse)
        {
            ForceRelease(clientBase);
            g_state = kIdle;
            return;
        }

        const bool keyHeld =
            g_settings.holdKey == kKeyAlways ||
            (GetAsyncKeyState(g_settings.holdKey) & 0x8000) != 0;
        g_diag.keyHeld = keyHeld;
        if (!keyHeld)
        {
            ForceRelease(clientBase);
            g_state = kIdle;
            return;
        }

        uintptr_t entitySystem = 0;
        if (!SafeRead(reinterpret_cast<void*>(clientBase + kDwEntityList),
                      &entitySystem) ||
            !IsReadablePointer(entitySystem))
            return;

        uintptr_t localPawn = 0;
        SafeRead(reinterpret_cast<void*>(clientBase + kDwLocalPlayerPawn),
                 &localPawn);
        if (!IsReadablePointer(localPawn))
            return;

        int localHealth = 0;
        uint8_t localLife = 1, localTeam = 0;
        SafeRead(reinterpret_cast<void*>(localPawn + kHealth), &localHealth);
        SafeRead(reinterpret_cast<void*>(localPawn + kLifeState), &localLife);
        SafeRead(reinterpret_cast<void*>(localPawn + kTeamNum), &localTeam);
        if (localLife != 0 || localHealth <= 0)
        {
            ForceRelease(clientBase);
            g_state = kIdle;
            return;
        }
        g_diag.pawnOk = true;

        Vector3 localOrigin{};
        uintptr_t localScene = 0;
        if (SafeRead(reinterpret_cast<void*>(localPawn + kGameSceneNode),
                     &localScene) &&
            IsReadablePointer(localScene))
        {
            SafeRead(reinterpret_cast<void*>(localScene + kVecAbsOrigin),
                     &localOrigin);
        }
        Vector3 viewOffset{};
        SafeRead(reinterpret_cast<void*>(localPawn + kVecViewOffset),
                 &viewOffset);
        const Vector3 eye{
            localOrigin.x + viewOffset.x,
            localOrigin.y + viewOffset.y,
            localOrigin.z + viewOffset.z,
        };

        struct QAngle2 { float pitch, yaw, roll; };
        QAngle2 angles{};
        if (!SafeRead(reinterpret_cast<void*>(clientBase + kDwViewAngles),
                      &angles) ||
            !std::isfinite(angles.pitch) || !std::isfinite(angles.yaw))
            return;

        const bool targetVisible = AcquireTarget(
            entitySystem, localPawn, localTeam, eye,
            angles.pitch, angles.yaw);
        g_diag.enemyUnderCrosshair = targetVisible;

        if (!targetVisible)
        {
            ForceRelease(clientBase);
            g_state = kIdle;
            g_diag.state = static_cast<int>(g_state);
            return;
        }

        const long long now = QpcNow();

        switch (g_state)
        {
        case kIdle:
            g_fireAtQpc = now + MsToQpc(RandomDelayMs());
            g_state = kWaiting;
            break;

        case kWaiting:
            // Visibility is re-evaluated every frame while the delay runs;
            // losing LOS above aborts back to idle.
            if (now >= g_fireAtQpc)
            {
                if (SafeWrite(reinterpret_cast<void*>(
                                  clientBase + kDwAttackButton),
                              kAttackPress))
                {
                    g_buttonHeld = true;
                    ++g_diag.presses;
                }
                g_state = kHolding;
            }
            break;

        case kHolding:
            // Keep the button held while a visible enemy stays under the
            // crosshair; the game's fire-rate sampling handles cadence.
            SafeWrite(reinterpret_cast<void*>(clientBase + kDwAttackButton),
                      kAttackPress);
            g_buttonHeld = true;
            break;
        }
        g_diag.state = static_cast<int>(g_state);
    }

    Diag GetDiag()
    {
        return g_diag;
    }
}

