#include "hudfix.h"

#include <MinHook.h>

#include <algorithm>
#include <cmath>

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
        ScaleformRendererCtorFn oScaleformRendererCtor = nullptr;

        enum class Scope { None, Icons, Killfeed, Minimap };

        // this UI frame's scale, set before the engine's renderJob
        float g_scale = 1.0f;

        thread_local Scope t_scope = Scope::None;
        // screen = anchor + (origin - anchor) * global + (p - origin) * scale: the element's origin keeps its
        // global-scale place and only the element grows around it
        thread_local bool t_anchored = false;
        thread_local fb::Vec2 t_anchor{ };
        thread_local fb::Vec2 t_origin{ };
        thread_local float t_global = 1.0f;
        thread_local float t_scale = 1.0f; // global scale * the scope's element factor
        thread_local float t_alpha = 1.0f; // the scope's element opacity

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
            }
            return visible;
        }

        void __fastcall hkDraw3dIcons(void* _this, void*, float dt)
        {
            ScopeGuard scope(Scope::Icons, false, fb::Vec2{ }, fb::Vec2{ }, elementFactor(Nametags), elementOpacity(Nametags));
            oDraw3dIcons(_this, dt);
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
        fb::Vec2* __fastcall hkDrawFromAtlas(void* _this, void*, fb::Vec2* outSize, const fb::UIHudIconDrawParams* params, float time, float pad, float rotation)
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

        // out is the text's local rect at the base size (callers multiply it by their own scale), so it stays as is
        // the text cache matches rgb only, so a faded alpha updates the cached line instead of adding one
        fb::GRectF* __fastcall hkDrawText(fb::UIHud* _this, void*, fb::GRectF* out, const fb::Vec2* pos, const char* text, float size, float glow, int halign, int valign, char snap)
        {
            const bool scale = anchored();
            if (!scale && t_alpha == 1.0f)
                return oDrawText(_this, out, pos, text, size, glow, halign, valign, snap);

            const float textAlpha = _this->m_textColor[3];
            const float glowAlpha = _this->m_glowColor[3];
            _this->m_textColor[3] = textAlpha * t_alpha;
            _this->m_glowColor[3] = glowAlpha * t_alpha;

            const fb::Vec2 at = scale ? toScreen(*pos) : *pos;
            fb::GRectF* result = oDrawText(_this, out, &at, text, scale ? size * t_scale : size, glow, halign, valign, snap);

            _this->m_textColor[3] = textAlpha;
            _this->m_glowColor[3] = glowAlpha;
            return result;
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
        hook(OFF_UIScaleformRenderer_ctor, hkScaleformRendererCtor, &oScaleformRendererCtor);
    }
}
