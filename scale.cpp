#include "hudfix.h"

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hudfix
{
    namespace
    {
        constexpr float SCLAEFORM_WIDTH = 1280.0f;
        constexpr float SCLAEFORM_HEIGHT = 720.0f;

        using RenderJobFn = int(__cdecl*)(fb::UISystem* system, float dt);
        using OnViewResizedFn = void(__thiscall*)(fb::UIMovieInstance* movie, unsigned int x, unsigned int y, unsigned int w, unsigned int h);
        using UpdatePositionFn = bool(__thiscall*)(void* icon, float dt, fb::Vec2* pos, bool toMovie);
        using GetScreenCoordinateFn = bool(__thiscall*)(void* comp, float dt, void* icon, fb::UI3dPosInfo* out);
        using DrawPassFn = void(__thiscall*)(void* comp, float dt);
        using MinimapScaleFn = void(__thiscall*)(fb::UIMinimap* minimap);
        using MinimapRenderFn = int(__thiscall*)(fb::UIMinimap* minimap, const void* transform, int a3, int a4);
        using DrawFromAtlasFn = fb::Vec2*(__thiscall*)(void* hud, fb::Vec2* outSize, const fb::UIHudIconDrawParams* params, float time, float pad, float rotation);
        using DrawTextFn = fb::GRectF*(__thiscall*)(fb::UIHud* hud, fb::GRectF* out, const fb::Vec2* pos, const char* text, float size, float glow, int halign, int valign, char snap);
        using UpdateTextLineFn = int(__thiscall*)(void* hud, unsigned char* line, const fb::Vec2* pos);
        using DrawPercentageBarFn = fb::Vec2*(__thiscall*)(void* hud, fb::Vec2* outSize, const fb::Vec2* center, const fb::Vec2* size, int relation, const void* color, float scale, float percent);
        using IconPassFn = void*(__thiscall*)(void* comp, float dt);
        using SetDisplayInfoFn = bool(__thiscall*)(void* iface, void* data, const fb::GFxDisplayInfo* info);
        using DrawPlayerIconFn = int(__thiscall*)(void* comp, void* player, float dt, void* soldier, void* vehicle, int fade, float health, int seat, int status);
        using ScaleformRendererCtorFn = void*(__thiscall*)(void* renderer, void* a2, void* a3, bool smallGlyphCache);

        RenderJobFn oRenderJob = nullptr;
        OnViewResizedFn oOnViewResized = nullptr;
        UpdatePositionFn oUpdatePosition = nullptr;
        GetScreenCoordinateFn oGetScreenCoordinate = nullptr;
        DrawPassFn oDraw3dIcons = nullptr;
        DrawPassFn oKillfeed = nullptr;
        MinimapScaleFn oMinimapScale = nullptr;
        MinimapRenderFn oMinimapRender = nullptr;
        DrawFromAtlasFn oDrawFromAtlas = nullptr;
        DrawTextFn oDrawText = nullptr;
        UpdateTextLineFn oUpdateTextLine = nullptr;
        DrawPercentageBarFn oDrawPercentageBar = nullptr;
        DrawPlayerIconFn oDrawPlayerIcon = nullptr;
        IconPassFn oIconPass = nullptr;
        SetDisplayInfoFn oSetDisplayInfo = nullptr;
        ScaleformRendererCtorFn oScaleformRendererCtor = nullptr;

        enum class Scope { None, Icons, Killfeed, Minimap };

        // this UI frame's scale, set before the engine's renderJob
        float g_scale = 1.0f;

        // the local soldier is zoomed, read once per 3D icon pass
        bool g_zoomed = false;

        thread_local Scope t_scope = Scope::None;
        // screen = anchor + (origin - anchor) * global + (p - origin) * scale: the element's origin keeps its
        // global-scale place and only the element grows around it
        thread_local bool t_anchored = false;
        thread_local fb::Vec2 t_anchor{ };
        thread_local fb::Vec2 t_origin{ };
        thread_local float t_global = 1.0f;
        thread_local float t_scale = 1.0f; // global scale * the scope's element factor
        thread_local float t_alpha = 1.0f; // the scope's element opacity
        thread_local float t_textFade = 1.0f; // inside drawText: our fade on top of the text's own alpha
        thread_local bool t_playerIcon = false; // inside drawPlayerIcon: its getScreenCoordinate picks team or enemy

        // the element's opacity, times its scoped percent while the local soldier is zoomed
        float iconAlpha(Element element)
        {
            return elementOpacity(element) * (g_zoomed ? settings().scoped[element] / 100.0f : 1.0f);
        }

        const fb::ScreenViewport* viewport()
        {
            auto* dx = fb::DxRenderer::GetInstance();
            return dx ? &dx->m_viewport : nullptr;
        }

        float desiredScale()
        {
            const float full = autoScale();
            if (full <= 1.0f)
                return 1.0f;
            return settings().autoScale ? full : std::clamp(settings().scale / 100.0f, 1.0f, full);
        }

        bool anchored()
        {
            return t_anchored && (t_scale != 1.0f || t_global != 1.0f);
        }

        fb::Vec2 toScreen(const fb::Vec2& p)
        {
            return t_anchor + (t_origin - t_anchor) * t_global + (p - t_origin) * t_scale;
        }

        // fb::Color32
        unsigned int fade(unsigned int color)
        {
            const unsigned int alpha = static_cast<unsigned int>((color >> 24) * t_alpha + 0.5f);
            return (color & 0x00FFFFFF) | (alpha << 24);
        }

        // the ctor picks a 1024 or 2048 glyph cache on its last arg, UIEngine create sub_1770A80 always passes 1
        // resizing the live cache instead re-rasterizes glyphs that queued text still points at, which flickers
        void* __fastcall hkScaleformRendererCtor(void* _this, void*, void* a2, void* a3, bool)
        {
            log("glyph cache 2048x2048");
            return oScaleformRendererCtor(_this, a2, a3, false);
        }

        struct ScopeGuard
        {
            Scope prevScope;
            bool prevAnchored;
            fb::Vec2 prevAnchor;
            fb::Vec2 prevOrigin;
            float prevGlobal;
            float prevScale;
            float prevAlpha;

            ScopeGuard(Scope s, bool a, const fb::Vec2& at, const fb::Vec2& origin, float factor, float alpha)
                : prevScope(t_scope), prevAnchored(t_anchored), prevAnchor(t_anchor), prevOrigin(t_origin), prevGlobal(t_global), prevScale(t_scale), prevAlpha(t_alpha)
            {
                t_scope = s;
                t_anchored = a;
                t_anchor = at;
                t_origin = origin;
                t_global = g_scale;
                t_scale = g_scale * factor;
                t_alpha = alpha;
            }

            ~ScopeGuard()
            {
                t_scope = prevScope;
                t_anchored = prevAnchored;
                t_anchor = prevAnchor;
                t_origin = prevOrigin;
                t_global = prevGlobal;
                t_scale = prevScale;
                t_alpha = prevAlpha;
            }
        };

        // a part of a 3D icon sized apart from it
        struct PartGuard
        {
            fb::Vec2 prevAnchor;
            fb::Vec2 prevOrigin;
            float prevGlobal;
            float prevScale;
            float prevAlpha;

            PartGuard(const fb::Vec2& origin, float factor, float alpha)
                : prevAnchor(t_anchor), prevOrigin(t_origin), prevGlobal(t_global), prevScale(t_scale), prevAlpha(t_alpha)
            {
                if (t_anchored)
                {
                    t_anchor = toScreen(origin);
                    t_origin = origin;
                    t_global = 0.0f;
                    t_scale = prevScale * factor;
                }
                t_alpha = alpha;
            }

            ~PartGuard()
            {
                t_anchor = prevAnchor;
                t_origin = prevOrigin;
                t_global = prevGlobal;
                t_scale = prevScale;
                t_alpha = prevAlpha;
            }
        };

        bool localZoomed()
        {
            fb::ClientPlayer* player = fb::ClientPlayer::GetLocal();
            fb::ClientSoldierEntity* soldier = player ? player->getSoldier() : nullptr;
            fb::ClientSoldierWeapon* weapon = soldier && soldier->m_weapons ? soldier->m_weapons->currentWeapon() : nullptr;
            return weapon && weapon->m_aiming && weapon->m_aiming->m_zoomLevel > 0;
        }

        int __cdecl hkRenderJob(fb::UISystem* system, float dt)
        {
            const float scale = desiredScale();
            if (scale != g_scale)
            {
                g_scale = scale;
                system->m_backbufferHeight = 0; // the original re-runs onFramebufferResized then root movie onViewResized
            }

            return oRenderJob(system, dt);
        }

        // GViewport::m_scale = 1/s: the stage becomes (w/s, h/s), drawn s times larger
        void __fastcall hkOnViewResized(fb::UIMovieInstance* _this, void*, unsigned int x, unsigned int y, unsigned int w, unsigned int h)
        {
            const float scale = g_scale;
            const auto* vp = viewport();
            if (scale <= 1.0f || !vp || w < SCLAEFORM_WIDTH || h < SCLAEFORM_HEIGHT)
            {
                oOnViewResized(_this, x, y, w, h);
                return;
            }

            const float width = w / scale;
            const float height = h / scale;
            // 0.97 is the original floating point, see this function in ida
            const float left = std::floor((width - _this->m_safeAreaFractionWidth * width * 0.97f) * 0.5f + 0.5f);
            const float top = std::floor((height - _this->m_safeAreaFractionHeight * height * 0.97f) * 0.5f + 0.5f);
            const fb::GRectF safe{ left, top, width - left, height - top };

            fb::GViewport view{ };
            view.m_bufferWidth = vp->m_width;
            view.m_bufferHeight = vp->m_height;
            view.m_left = static_cast<int>(x);
            view.m_top = static_cast<int>(y);
            view.m_width = static_cast<int>(w);
            view.m_height = static_cast<int>(h);
            view.m_scale = 1.0f / scale;
            view.m_aspectRatio = 1.0f;

            fb::GFxMovieView* movie = _this->m_movieView;
            movie->setSafeRect(&safe);
            movie->setViewScaleMode(fb::GFxScaleMode_NoScale);
            movie->setViewAlignment(fb::GFxAlign_Center);
            movie->setViewport(&view);
        }

        // toMovie is the Flash icon path (sub_955B20)
        // the original only converts below 720p
        bool __fastcall hkUpdatePosition(void* _this, void*, float dt, fb::Vec2* pos, bool toMovie)
        {
            const bool animating = oUpdatePosition(_this, dt, pos, toMovie);
            if (toMovie && g_scale > 1.0f)
                *pos = *pos / g_scale;
            return animating;
        }

        bool __fastcall hkGetScreenCoordinate(void* _this, void*, float dt, void* icon, fb::UI3dPosInfo* out)
        {
            const bool visible = oGetScreenCoordinate(_this, dt, icon, out);
            if (t_scope == Scope::Icons)
            {
                t_anchored = visible;
                if (visible)
                {
                    t_anchor = out->m_pos;
                    t_origin = out->m_pos;
                }
                if (t_playerIcon)
                {
                    const Element element = static_cast<const fb::UI3dIcon*>(icon)->m_relation == fb::UI3dIconRelation_Enemy ? NametagsEnemy : Nametags;
                    t_scale = g_scale * elementFactor(element);
                    t_alpha = iconAlpha(element);
                }
            }
            return visible;
        }

        void __fastcall hkDraw3dIcons(void* _this, void*, float dt)
        {
            g_zoomed = localZoomed();
            ScopeGuard scope(Scope::Icons, false, fb::Vec2{ }, fb::Vec2{ }, elementFactor(Nametags), elementOpacity(Nametags));
            oDraw3dIcons(_this, dt);
        }

        int __fastcall hkDrawPlayerIcon(void* _this, void*, void* player, float dt, void* soldier, void* vehicle, int fade, float health, int seat, int status)
        {
            const float scale = t_scale;
            const float alpha = t_alpha;
            t_playerIcon = true;

            const int result = oDrawPlayerIcon(_this, player, dt, soldier, vehicle, fade, health, seat, status);
            t_playerIcon = false;
            t_scale = scale;
            t_alpha = alpha;
            return result;
        }

        constexpr unsigned short DISPLAY_XSCALE = 0x8, DISPLAY_YSCALE = 0x10, DISPLAY_ALPHA = 0x20; // GFxDisplayInfo::m_varsSet
        thread_local bool t_flagPass = false; // inside the capture point tags Flash icon pass

        // UI3dIconComp's Flash icons (sub_955B20): each icon's clip gets a GFxDisplayInfo with its distance scale and alpha
        void* __fastcall hkIconPass(void* _this, void*, float dt)
        {
            const char* const* data = *reinterpret_cast<const char* const* const*>(static_cast<char*>(_this) + 4);
            const char* name = data ? data[2] : nullptr; // UIComponentData name +8
            const bool previous = t_flagPass;
            t_flagPass = name && std::strstr(name, "UICapturepointtagComp");
            void* result = oIconPass(_this, dt);
            t_flagPass = previous;
            return result;
        }

        // the clips grow around their own registration point
        bool __fastcall hkSetDisplayInfo(void* _this, void*, void* data, const fb::GFxDisplayInfo* info)
        {
            if (!t_flagPass || !info)
                return oSetDisplayInfo(_this, data, info);

            fb::GFxDisplayInfo changed = *info;
            const double k = elementFactor(Flags);
            if (changed.m_varsSet & DISPLAY_XSCALE)
                changed.m_xScale *= k;
            if (changed.m_varsSet & DISPLAY_YSCALE)
                changed.m_yScale *= k;
            if (changed.m_varsSet & DISPLAY_ALPHA)
                changed.m_alpha *= iconAlpha(Flags);
            return oSetDisplayInfo(_this, data, &changed);
        }

        // background, two caps and the fill around center; a 0% bar still draws (transparent) for the name's layout
        fb::Vec2* __fastcall hkDrawPercentageBar(void* _this, void*, fb::Vec2* outSize, const fb::Vec2* center, const fb::Vec2* size, int relation, const void* color, float scale, float percent)
        {
            const Element element = relation == fb::UI3dIconRelation_Enemy ? HealthBarsEnemy : HealthBars;
            PartGuard part(*center, elementFactor(element), iconAlpha(element));
            return oDrawPercentageBar(_this, outSize, center, size, relation, color, scale, percent);
        }

        // rows are right-aligned at floor(0.95w)+x, the first one at y = 100 * min(w/1280, 1)
        void __fastcall hkKillfeed(void* _this, void*, float dt)
        {
            const auto* vp = viewport();
            const fb::Vec2 anchor = vp ? fb::vec2(std::floor(vp->m_width * 0.95f) + vp->m_x, 0.0f) : fb::Vec2{ };
            const float top = vp ? 100.0f * (std::min)(vp->m_width / SCLAEFORM_WIDTH, 1.0f) : 0.0f;
            ScopeGuard scope(Scope::Killfeed, vp != nullptr, anchor, fb::vec2(anchor.m_x, top), elementFactor(KillLog), elementOpacity(KillLog));
            oKillfeed(_this, dt);
        }

        void __fastcall hkMinimapScale(fb::UIMinimap* _this, void*)
        {
            oMinimapScale(_this);
            const float base = g_scale > 1.0f ? g_scale : _this->m_resolutionScale;
            _this->m_resolutionScale = base * elementFactor(MinimapIcons);
        }

        // only fades, the icons' size is m_resolutionScale
        int __fastcall hkMinimapRender(fb::UIMinimap* _this, void*, const void* transform, int a3, int a4)
        {
            ScopeGuard scope(Scope::Minimap, false, fb::Vec2{ }, fb::Vec2{ }, 1.0f, elementOpacity(MinimapIcons));
            return oMinimapRender(_this, transform, a3, a4);
        }

        // callers lay out in 720p units around the anchor
        // positions and sizes are magnified, the returned size is not
        fb::Vec2* drawFromAtlas(void* _this, fb::Vec2* outSize, const fb::UIHudIconDrawParams* params, float time, float pad, float rotation)
        {
            const bool scale = anchored();
            if (!scale && t_alpha == 1.0f)
                return oDrawFromAtlas(_this, outSize, params, time, pad, rotation);

            fb::UIHudIconDrawParams changed = *params;
            if (scale)
            {
                changed.m_pos = toScreen(params->m_pos);
                changed.m_scale = params->m_scale * t_scale;
            }
            changed.m_color = fade(params->m_color);
            fb::Vec2* result = oDrawFromAtlas(_this, outSize, &changed, time, pad, rotation);
            if (scale)
                *outSize = *outSize / t_scale;
            return result;
        }

        // man down and revive icons are centered on m_pos (icon row sub_918450)
        fb::Vec2* __fastcall hkDrawFromAtlas(void* _this, void*, fb::Vec2* outSize, const fb::UIHudIconDrawParams* params, float time, float pad, float rotation)
        {
            if (t_scope != Scope::Icons || (params->m_icon != fb::UIHudIcon_PlayerDead && params->m_icon != fb::UIHudIcon_Revive))
                return drawFromAtlas(_this, outSize, params, time, pad, rotation);

            PartGuard part(params->m_pos, elementFactor(ReviveIcons), iconAlpha(ReviveIcons));
            return drawFromAtlas(_this, outSize, params, time, pad, rotation);
        }

        // out is the text's local rect at the base size (callers multiply it by their own scale), so it stays as is
        // the colors stay as they are: they key the line cache, and the text alpha only ever reaches the glow
        fb::GRectF* __fastcall hkDrawText(fb::UIHud* _this, void*, fb::GRectF* out, const fb::Vec2* pos, const char* text, float size, float glow, int halign, int valign, char snap)
        {
            const bool scale = anchored();
            if (!scale && t_alpha == 1.0f)
                return oDrawText(_this, out, pos, text, size, glow, halign, valign, snap);

            t_textFade = t_alpha;
            const fb::Vec2 at = scale ? toScreen(*pos) : *pos;
            fb::GRectF* result = oDrawText(_this, out, &at, text, scale ? size * t_scale : size, glow, halign, valign, snap);
            t_textFade = 1.0f;
            return result;
        }

        // the line is displayed right here, so its GFxDrawText's cxform alpha fades text and glow together. Lines are
        // cached and shared, so every update sets it, back to 1 outside a fade
        int __fastcall hkUpdateTextLine(void* _this, void*, unsigned char* line, const fb::Vec2* pos)
        {
            if (auto* text = *reinterpret_cast<fb::GFxDrawText**>(line + 0x58))
            {
                float cxform[4][2];
                std::memcpy(cxform, text->getCxform(), sizeof(cxform));
                if (cxform[3][0] != t_textFade)
                {
                    cxform[3][0] = t_textFade;
                    text->setCxform(&cxform[0][0]);
                }
            }
            return oUpdateTextLine(_this, line, pos);
        }
    }

    float autoScale()
    {
        const auto* vp = viewport();
        if (!vp || vp->m_width < SCLAEFORM_WIDTH || vp->m_height < SCLAEFORM_HEIGHT)
            return 1.0f;

        return (std::min)(vp->m_width / SCLAEFORM_WIDTH, vp->m_height / SCLAEFORM_HEIGHT);
    }

    float currentScale()
    {
        return g_scale;
    }

    void installScaleHooks()
    {
        hook(OFF_UISystem_renderJob, hkRenderJob, &oRenderJob);
        hook(OFF_UIMovieInstance_onViewResized, hkOnViewResized, &oOnViewResized);
        hook(OFF_UI3dIcon_updatePosition, hkUpdatePosition, &oUpdatePosition);
        hook(OFF_UI3dIconComp_getScreenCoordinate, hkGetScreenCoordinate, &oGetScreenCoordinate);
        hook(OFF_UIHud_draw3dIcons, hkDraw3dIcons, &oDraw3dIcons);
        hook(OFF_UIKillfeed_draw, hkKillfeed, &oKillfeed);
        hook(OFF_UIMinimap_updateScale, hkMinimapScale, &oMinimapScale);
        hook(OFF_UIMinimap_render, hkMinimapRender, &oMinimapRender);
        hook(OFF_UIHud_drawFromAtlas, hkDrawFromAtlas, &oDrawFromAtlas);
        hook(OFF_UIHud_drawText, hkDrawText, &oDrawText);
        hook(OFF_UIHud_updateTextLine, hkUpdateTextLine, &oUpdateTextLine);
        hook(OFF_UIHud_drawPercentageBar, hkDrawPercentageBar, &oDrawPercentageBar);
        hook(OFF_UIHud3dIcons_drawPlayerIcon, hkDrawPlayerIcon, &oDrawPlayerIcon);
        hook(OFF_UI3dIconComp_updateFlashIcons, hkIconPass, &oIconPass);
        hook(OFF_GFxValue_setDisplayInfo, hkSetDisplayInfo, &oSetDisplayInfo);
        hook(OFF_UIScaleformRenderer_ctor, hkScaleformRendererCtor, &oScaleformRendererCtor);
    }
}
