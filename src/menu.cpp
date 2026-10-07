#include "menu.h"

#include <windows.h>

#include <cstring>

#include "esp.h"
#include "aimbot.h"
#include "vis.h"
#include "triggerbot.h"
#include "thirdperson.h"
#include "overrides.h"
#include "bhop.h"
#include "antiaim.h"
#include "pro/license.h"
#include "imgui.h"

#include <ctime>
#include <cstdio>

namespace menu
{
    // -------------------------------------------------------------------
    // Theme
    // -------------------------------------------------------------------
    void ApplyTheme()
    {
        ImGuiStyle& st = ImGui::GetStyle();

        // ---- Metrics: generous padding, rounded everything. ----
        st.WindowPadding     = ImVec2(15.0f, 14.0f);
        st.FramePadding      = ImVec2(10.0f, 6.0f);
        st.CellPadding       = ImVec2(5.0f, 3.0f);
        st.ItemSpacing       = ImVec2(10.0f, 8.0f);
        st.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
        st.TouchExtraPadding = ImVec2(0.0f, 0.0f);
        st.IndentSpacing     = 18.0f;
        st.ScrollbarSize     = 11.0f;
        st.GrabMinSize       = 12.0f;

        st.WindowBorderSize  = 1.0f;
        st.ChildBorderSize   = 1.0f;
        st.PopupBorderSize   = 1.0f;
        st.FrameBorderSize   = 0.0f;
        st.TabBorderSize     = 0.0f;
        st.WindowRounding    = 9.0f;
        st.ChildRounding     = 7.0f;
        st.FrameRounding     = 6.0f;
        st.PopupRounding     = 7.0f;
        st.ScrollbarRounding = 8.0f;
        st.GrabRounding      = 6.0f;
        st.TabRounding       = 6.0f;

        st.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
        st.ButtonTextAlign   = ImVec2(0.5f, 0.5f);
        st.SeparatorTextBorderSize = 2.0f;
        st.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
        st.SeparatorTextPadding = ImVec2(6.0f, 4.0f);

        // ---- Palette: deep navy panels, electric-blue accent. ----
        ImVec4* c = st.Colors;
        auto set = [&](ImGuiCol idx, float r, float g, float b,
                       float a = 1.0f)
        {
            c[idx] = ImVec4(r, g, b, a);
        };

        set(ImGuiCol_Text,                  0.88f, 0.91f, 0.97f);
        set(ImGuiCol_TextDisabled,          0.43f, 0.48f, 0.60f);
        set(ImGuiCol_WindowBg,              0.055f, 0.063f, 0.090f, 0.985f);
        set(ImGuiCol_ChildBg,               0.078f, 0.090f, 0.125f, 0.55f);
        set(ImGuiCol_PopupBg,               0.062f, 0.072f, 0.103f, 0.985f);
        set(ImGuiCol_Border,                0.190f, 0.225f, 0.310f, 0.45f);
        set(ImGuiCol_BorderShadow,          0.0f, 0.0f, 0.0f, 0.0f);
        set(ImGuiCol_FrameBg,               0.115f, 0.135f, 0.190f, 0.95f);
        set(ImGuiCol_FrameBgHovered,        0.155f, 0.185f, 0.265f);
        set(ImGuiCol_FrameBgActive,         0.195f, 0.235f, 0.340f);
        set(ImGuiCol_TitleBg,               0.070f, 0.082f, 0.115f);
        set(ImGuiCol_TitleBgActive,         0.085f, 0.100f, 0.145f);
        set(ImGuiCol_TitleBgCollapsed,      0.070f, 0.082f, 0.115f, 0.6f);
        set(ImGuiCol_MenuBarBg,             0.078f, 0.090f, 0.125f);
        set(ImGuiCol_ScrollbarBg,           0.0f, 0.0f, 0.0f, 0.0f);
        set(ImGuiCol_ScrollbarGrab,         0.235f, 0.280f, 0.400f, 0.75f);
        set(ImGuiCol_ScrollbarGrabHovered,  0.330f, 0.400f, 0.580f, 0.9f);
        set(ImGuiCol_ScrollbarGrabActive,   0.420f, 0.520f, 0.760f);
        set(ImGuiCol_CheckMark,             0.620f, 0.730f, 1.000f);
        set(ImGuiCol_CheckboxSelectedBg,    0.300f, 0.420f, 0.720f, 0.55f);
        set(ImGuiCol_SliderGrab,            0.500f, 0.620f, 0.980f);
        set(ImGuiCol_SliderGrabActive,      0.380f, 0.500f, 0.900f);
        set(ImGuiCol_Button,                0.165f, 0.195f, 0.285f);
        set(ImGuiCol_ButtonHovered,         0.230f, 0.275f, 0.405f);
        set(ImGuiCol_ButtonActive,          0.290f, 0.350f, 0.510f);
        set(ImGuiCol_Header,                0.205f, 0.260f, 0.400f, 0.70f);
        set(ImGuiCol_HeaderHovered,         0.275f, 0.345f, 0.515f, 0.85f);
        set(ImGuiCol_HeaderActive,          0.330f, 0.420f, 0.620f, 0.95f);
        set(ImGuiCol_Separator,             0.190f, 0.225f, 0.310f, 0.50f);
        set(ImGuiCol_SeparatorHovered,      0.470f, 0.610f, 1.000f, 0.70f);
        set(ImGuiCol_SeparatorActive,       0.470f, 0.610f, 1.000f);
        set(ImGuiCol_ResizeGrip,            0.350f, 0.450f, 0.700f, 0.25f);
        set(ImGuiCol_ResizeGripHovered,     0.420f, 0.540f, 0.820f, 0.70f);
        set(ImGuiCol_ResizeGripActive,      0.470f, 0.610f, 1.000f);
        set(ImGuiCol_Tab,                   0.100f, 0.120f, 0.175f, 0.90f);
        set(ImGuiCol_TabHovered,            0.255f, 0.330f, 0.510f);
        set(ImGuiCol_TabSelected,           0.290f, 0.380f, 0.600f);
        set(ImGuiCol_TabSelectedOverline,   0.470f, 0.610f, 1.000f);
        set(ImGuiCol_TabDimmed,             0.100f, 0.120f, 0.175f, 0.60f);
        set(ImGuiCol_TabDimmedSelected,     0.185f, 0.235f, 0.360f, 0.80f);
        set(ImGuiCol_TabDimmedSelectedOverline, 0.340f, 0.430f, 0.660f);
        set(ImGuiCol_TextSelectedBg,        0.300f, 0.400f, 0.640f, 0.45f);
        set(ImGuiCol_DragDropTarget,        0.470f, 0.610f, 1.000f, 0.90f);
        set(ImGuiCol_NavCursor,             0.470f, 0.610f, 1.000f, 0.80f);
        set(ImGuiCol_ModalWindowDimBg,      0.0f, 0.0f, 0.0f, 0.55f);
        set(ImGuiCol_TableHeaderBg,         0.120f, 0.145f, 0.205f);
        set(ImGuiCol_TableBorderStrong,     0.190f, 0.225f, 0.310f, 0.65f);
        set(ImGuiCol_TableBorderLight,      0.190f, 0.225f, 0.310f, 0.30f);
        set(ImGuiCol_TableRowBg,            0.0f, 0.0f, 0.0f, 0.0f);
        set(ImGuiCol_TableRowBgAlt,         1.0f, 1.0f, 1.0f, 0.03f);
    }

namespace
{
    void StatusDot(bool ok)
    {
        const ImU32 col = ok
            ? IM_COL32(105, 220, 140, 255)
            : IM_COL32(235, 110, 110, 255);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float r = 4.0f * (ImGui::GetFontSize() / 13.0f);
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f),
            r, col);
        ImGui::Dummy(ImVec2(r * 2.0f + 8.0f, ImGui::GetTextLineHeight()));
    }

    void TabInfo()
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float uiScale = ImGui::GetFontSize() / 13.0f;

        ImGui::SeparatorText("Status");
        ImGui::AlignTextToFramePadding();
        StatusDot(true);
        ImGui::SameLine();
        ImGui::Text("Overlay injected and rendering");

        ImGui::Spacing();
        ImGui::Text("FPS");
        ImGui::SameLine(130.0f * uiScale);
        ImGui::TextColored(ImVec4(0.52f, 0.65f, 1.00f, 1.0f),
                           "%.1f", io.Framerate);
        ImGui::Text("Frame time");
        ImGui::SameLine(130.0f * uiScale);
        ImGui::TextColored(ImVec4(0.52f, 0.65f, 1.00f, 1.0f),
                           "%.3f ms", 1000.0f / io.Framerate);
        ImGui::Text("Display");
        ImGui::SameLine(130.0f * uiScale);
        ImGui::TextColored(ImVec4(0.52f, 0.65f, 1.00f, 1.0f),
                           "%.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
        ImGui::Text("UI backend");
        ImGui::SameLine(130.0f * uiScale);
        ImGui::Text("Dear ImGui %s  (DX11)", IMGUI_VERSION);

        ImGui::Spacing();
        ImGui::SeparatorText("Controls");
        ImGui::BulletText("INSERT  -  show / hide this window");
        ImGui::BulletText("Mouse   -  toggle options, drag sliders");
        ImGui::TextWrapped("Feature-specific hotkeys are configured on "
                           "their own tabs.");
    }

    void TabSettings()
    {
        ImGui::SeparatorText("License");
        {
            pro::Init();
            const pro::State ls = pro::GetState();
            if (ls.ok)
            {
                const std::time_t exp =
                    static_cast<std::time_t>(ls.expiresAt);
                std::tm tmv{};
                localtime_s(&tmv, &exp);
                char date[32] = {};
                std::strftime(date, sizeof(date), "%Y-%m-%d", &tmv);

                static const char* kNames[] = {
                    "aimbot_pro", "cloud_cfg", "premium_esp",
                    "fov", "thirdperson", "antiaim"
                };
                char feats[128] = {};
                std::size_t used = 0;
                for (int i = 0; i < 6; ++i)
                {
                    if ((ls.featureMask & (1u << i)) != 0)
                        used += static_cast<std::size_t>(std::snprintf(
                            feats + used, sizeof(feats) - used, "%s%s",
                            used ? ", " : "", kNames[i]));
                }

                ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.6f, 1.0f),
                                   "Pro active  (key %llu)",
                                   (unsigned long long)ls.keyId);
                ImGui::Text("expires %s", date);
                ImGui::TextDisabled("features: %s",
                                    used ? feats : "none");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                                   "Community edition");
                ImGui::TextDisabled("%s", ls.failReason);
                ImGui::TextDisabled("Third-person view, FOV tweak and "
                                    "anti-aim are Pro features.");
            }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Movement");
        {
            bhop::Settings& bh = bhop::g_settings;

            ImGui::Checkbox("Auto bunny hop", &bh.enabled);

            const char* keyNames[] = {
                "Space", "Mouse X2 (side)", "V", "B"
            };
            const int keyCodes[] = { 0x20, 0x06, 0x56, 0x42 };
            int keySel = 0;
            for (int k = 0; k < 4; ++k)
                if (keyCodes[k] == bh.jumpKey)
                    keySel = k;
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Jump key", &keySel, keyNames, 4))
                bh.jumpKey = keyCodes[keySel];

            const bhop::Diag bd = bhop::GetDiag();
            ImGui::Text("client: %d  pawn: %d  key: %d  ground: %d  jumps: %d",
                        bd.clientFound ? 1 : 0,
                        bd.pawnOk ? 1 : 0,
                        bd.keyHeld ? 1 : 0,
                        bd.onGround ? 1 : 0,
                        bd.jumpPulses);
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Anti-aim");
        {
            antiaim::Settings& aa = antiaim::g_settings;

            ImGui::Checkbox("Face camera + look down (third person) [Pro]",
                            &aa.enabled);
            ImGui::Checkbox("Send real angles while firing",
                            &aa.realWhileAttack);
            ImGui::TextDisabled("Rewrites the outgoing user command angles; "
                                "the camera itself is untouched.");

            const antiaim::Diag ad = antiaim::GetDiag();
            ImGui::Text("client: %d  hook: %d/%d  faking: %d  firing: %d  "
                        "ticks: %d  localfix: %d",
                        ad.clientFound ? 1 : 0,
                        ad.hookInstalled ? 1 : 0,
                        ad.setHookInstalled ? 1 : 0,
                        ad.faking ? 1 : 0,
                        ad.firing ? 1 : 0,
                        ad.ticks,
                        ad.localFixes);
            ImGui::Text("real yaw: %.1f  fake yaw: %.1f",
                        ad.realYaw, ad.fakeYaw);
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Camera");
        {
            thirdperson::Settings& tp = thirdperson::g_settings;

            if (!thirdperson::Ready())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                    "Third person: resolving client.dll camera state...");
                const thirdperson::Diag d = thirdperson::GetDiag();
                ImGui::Text("client.dll: %d  getter hook: %d  state: %d",
                            d.clientFound ? 1 : 0,
                            d.hookInstalled ? 1 : 0,
                            d.stateResolved ? 1 : 0);
                ImGui::Text("pawn: %d  gate: %d  vfunc: %06X",
                            d.pawnFound ? 1 : 0,
                            d.gateActive,
                            d.vfuncRva);
                ImGui::Text("resolve attempts: %d", d.resolveAttempts);
            }
            else
            {
                ImGui::Checkbox("Third-person view [Pro]", &tp.enabled);

                const char* keyNames[] = {
                    "F5", "F6", "F7", "F8", "V", "B"
                };
                const int keyCodes[] = {
                    0x74, 0x75, 0x76, 0x77, 0x56, 0x42 // F5..F8, V, B
                };
                int keySel = 0;
                for (int k = 0; k < 6; ++k)
                    if (keyCodes[k] == tp.toggleKey)
                        keySel = k;
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::Combo("Toggle key", &keySel, keyNames, 6))
                    tp.toggleKey = keyCodes[keySel];

                ImGui::SetNextItemWidth(200.0f);
                ImGui::SliderFloat("Camera distance", &tp.distance,
                                   30.0f, 300.0f, "%.0f");

                ImGui::Text("State: %s",
                            thirdperson::Active() ? "third person"
                                                  : "first person");

                const thirdperson::Diag d2 = thirdperson::GetDiag();
                ImGui::Text("pawn: %d  gate: %d  vfunc: %06X",
                            d2.pawnFound ? 1 : 0,
                            d2.gateActive,
                            d2.vfuncRva);
            }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Overrides");
        {
            overrides::Settings& ov = overrides::g_settings;

            ImGui::Checkbox("Anti-flash", &ov.antiFlash);

            ImGui::Checkbox("First-person FOV [Pro]", &ov.fovEnabled);
            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderInt("##fovValue", &ov.fov, 80, 130,
                             "FOV %d");
            ImGui::SameLine();
            ImGui::TextDisabled("(first person only)");

            ImGui::Checkbox("Sky color", &ov.skyEnabled);
            ImGui::SameLine();
            ImGui::ColorEdit4("##skyColor", ov.skyColor,
                              ImGuiColorEditFlags_NoInputs |
                                  ImGuiColorEditFlags_NoLabel);

            const overrides::Diag od = overrides::GetDiag();
            ImGui::Text("client: %d  pawn: %d  controller: %d  camSvc: %d  sky: %d",
                        od.clientFound ? 1 : 0,
                        od.pawnOk ? 1 : 0,
                        od.ctrlOk ? 1 : 0,
                        od.camSvcOk ? 1 : 0,
                        od.skyFound ? 1 : 0);
            if (ov.skyEnabled)
            {
                if (!od.skyFound)
                    ImGui::TextDisabled("scanning sky/fog entities...");
                for (int i = 0; i < od.skyCandCount; ++i)
                {
                    const auto& c = od.skyCandidates[i];
                    if (std::strncmp(c.name, "env_sky", 7) == 0)
                        ImGui::Text("  [%d] %s  active: %d", c.index, c.name,
                                    static_cast<int>(c.active));
                    else
                        ImGui::TextDisabled("  [%d] %s", c.index, c.name);
                }
            }
        }
    }

    void TabAimbot()
    {
        aimbot::Settings& a = aimbot::g_settings;

        ImGui::Checkbox("Enable aimbot", &a.enabled);
        ImGui::Separator();

        const char* keyNames[] = {
            "Left ALT", "Right mouse button", "Mouse X1 (side)",
            "Left SHIFT", "Left CTRL"
        };
        const int keyCodes[] = { 0x12, 0x02, 0x05, 0x10, 0x11 };
        int keySel = 0;
        for (int k = 0; k < 5; ++k)
            if (keyCodes[k] == a.holdKey)
                keySel = k;
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("Hold key", &keySel, keyNames, 5))
            a.holdKey = keyCodes[keySel];

        const char* boneNames[] = { "Head", "Neck", "Pelvis" };
        ImGui::SetNextItemWidth(220.0f);
        ImGui::Combo("Target bone", &a.targetBone, boneNames, 3);

        ImGui::SetNextItemWidth(260.0f);
        ImGui::SliderFloat("FOV (degrees)", &a.fov, 0.5f, 20.0f, "%.1f");
        ImGui::SetNextItemWidth(260.0f);
        ImGui::SliderFloat("Smoothing (1 = snap)", &a.smoothing, 1.0f, 20.0f,
                           "%.1f");

        ImGui::Spacing();
        ImGui::Checkbox("Target teammates too", &a.teammates);
        ImGui::Checkbox("Draw FOV circle", &a.drawFov);
        ImGui::SameLine(200.0f * (ImGui::GetFontSize() / 13.0f));
        ImGui::ColorEdit4("##fovColor", &a.fovColor.x,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);

        ImGui::Spacing();
        ImGui::SeparatorText("Silent aim / visibility");
        ImGui::Checkbox("Silent aim (command angles only)", &a.silentAim);
        ImGui::SameLine();
        ImGui::TextDisabled("(camera does not move)");
        ImGui::Checkbox("Visible check (trace line of sight)",
                        &a.visibleCheck);
        if (a.silentAim)
            ImGui::TextDisabled("Note: smoothing applies to classic mode "
                                "only; silent snaps.");

        ImGui::Spacing();
        ImGui::SeparatorText("Recoil control");
        ImGui::Checkbox("RCS (counter per-shot recoil)", &a.rcs);
        ImGui::SetNextItemWidth(260.0f);
        ImGui::SliderFloat("RCS scale", &a.rcsScale, -2.0f, 2.0f, "%.2f");
        ImGui::TextDisabled("Only new recoil is countered; 1.00 = full. "
                            "Use a negative value if it pulls wrong.");

        ImGui::Spacing();
        ImGui::SeparatorText("Trigger bot");
        triggerbot::Settings& t = triggerbot::g_settings;
        ImGui::Checkbox("Enable trigger bot", &t.enabled);

        const char* tbKeyNames[] = {
            "Always on", "Left ALT", "Right mouse button",
            "Mouse X1 (side)", "Left SHIFT", "Left CTRL"
        };
        const int tbKeyCodes[] = { -1, 0x12, 0x02, 0x05, 0x10, 0x11 };
        int tbSel = 0;
        for (int k = 0; k < 6; ++k)
            if (tbKeyCodes[k] == t.holdKey)
                tbSel = k;
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("Trigger key", &tbSel, tbKeyNames, 6))
            t.holdKey = tbKeyCodes[tbSel];

        ImGui::SetNextItemWidth(260.0f);
        ImGui::SliderInt("Delay min (ms)", &t.delayMin, 0, 300);
        ImGui::SetNextItemWidth(260.0f);
        ImGui::SliderInt("Delay max (ms)", &t.delayMax, 0, 300);
        if (t.delayMax < t.delayMin)
            t.delayMax = t.delayMin;
        ImGui::Checkbox("Trigger on teammates too", &t.teammates);

        const triggerbot::Diag td = triggerbot::GetDiag();
        const vis::Diag& vd = vis::g_diag;
        ImGui::Spacing();
        ImGui::Text("vis: hook %d  calls %llu  probes %llu/%llu  %ums ago",
                    vd.hookInstalled ? 1 : 0,
                    static_cast<unsigned long long>(vd.calls),
                    static_cast<unsigned long long>(vd.probesBlocked),
                    static_cast<unsigned long long>(vd.probesRun),
                    vd.lastProbeMs
                        ? static_cast<unsigned>(GetTickCount64() -
                                                vd.lastProbeMs)
                        : 0xFFFFFFFFu);
        ImGui::Text("trig: key %d  LOS %s/%s  enemy %d  state %d  "
                    "press %d rel %d  ang %.2f",
                    td.keyHeld ? 1 : 0,
                    td.traceFresh ? "ok" : "--",
                    td.traceBlocked ? "wall" : "open",
                    td.enemyUnderCrosshair ? 1 : 0,
                    td.state, td.presses, td.releases,
                    static_cast<double>(td.nearest));
    }

    void TabVisuals()
    {
        esp::Settings& s = esp::g_settings;

        ImGui::Checkbox("Enable ESP (INSERT toggles this window)", &s.enabled);

        ImGui::Spacing();
        ImGui::SeparatorText("Box");
        const char* boxNames[] = { "Off", "Full", "Corners", "Rounded" };
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("##boxStyle", &s.boxStyle, boxNames, 4);
        ImGui::SameLine();
        ImGui::Checkbox("Glow##boxGlow", &s.boxGlow);
        ImGui::SameLine();
        ImGui::Checkbox("Fill##boxFill", &s.boxFill);

        const char* modeNames[] = { "Static", "Health", "Rainbow" };
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("Accent color", &s.colorMode, modeNames, 3);
        ImGui::BeginDisabled(s.colorMode != esp::kColorStatic);
        ImGui::ColorEdit4("##boxColor", &s.boxColor.x,
                          ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine();
        ImGui::TextDisabled("box accent");
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::SeparatorText("Player");
        ImGui::Checkbox("Skeleton", &s.skeleton);
        ImGui::SameLine(160.0f * (ImGui::GetFontSize() / 13.0f));
        ImGui::Checkbox("bone glow", &s.skeletonGlow);
        ImGui::SameLine(300.0f * (ImGui::GetFontSize() / 13.0f));
        ImGui::Checkbox("joints", &s.skeletonJoints);
        ImGui::ColorEdit4("##skelColor", &s.skeletonColor.x,
                          ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine();
        ImGui::TextDisabled("skeleton color");

        ImGui::Checkbox("Player name", &s.name);
        ImGui::Checkbox("Health bar", &s.healthBar);
        ImGui::SameLine();
        ImGui::Checkbox("HP number", &s.hpNumber);
        ImGui::Checkbox("Distance", &s.distanceText);
        ImGui::Checkbox("Head ring", &s.headRing);

        ImGui::Spacing();
        ImGui::SeparatorText("Snapline");
        ImGui::Checkbox("Enable snapline", &s.snapline);
        const char* snapNames[] = { "Bottom edge", "Screen center", "Top edge" };
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("##snapOrigin", &s.snapOrigin, snapNames, 3);
        ImGui::ColorEdit4("##snapColor", &s.snapColor.x,
                          ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine();
        ImGui::TextDisabled("snapline color");

        ImGui::Spacing();
        ImGui::SeparatorText("Filters");
        ImGui::Checkbox("Show teammates", &s.teammates);
        ImGui::Checkbox("Hide dormant players", &s.hideDormant);
        ImGui::Checkbox("Debug pipeline overlay", &s.debug);

        ImGui::SetNextItemWidth(220.0f);
        ImGui::SliderFloat("Max distance (m, 0 = unlimited)",
                           &s.maxDistance, 0.0f, 300.0f, "%.0f");
        if (s.maxDistance < 1.0f)
            s.maxDistance = 0.0f;
    }
}

void Render(bool* pOpen)
{
    // Keep the window proportional to the global UI scale (hooks.cpp).
    const float uiScale = ImGui::GetFontSize() / 13.0f;
    ImGui::SetNextWindowSize(ImVec2(600.0f * uiScale, 430.0f * uiScale),
                             ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80.0f, 80.0f), ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(16.0f * uiScale, 14.0f * uiScale));
    if (!ImGui::Begin("MifiInj", pOpen, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::PopStyleVar();
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll))
    {
        if (ImGui::BeginTabItem("Info"))
        {
            TabInfo();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Visuals"))
        {
            TabVisuals();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Aimbot"))
        {
            TabAimbot();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings"))
        {
            TabSettings();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace menu
