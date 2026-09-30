#include "hudfix.h"

#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <cassert>

namespace hudfix
{
    namespace
    {
        using GetDisplayInfoFn = bool(__thiscall*)(void* iface, void* data, fb::GFxDisplayInfo* out);
        using SetDisplayInfoFn = bool(__thiscall*)(void* iface, void* data, const fb::GFxDisplayInfo* info);
        using ObjectReleaseFn = void(__thiscall*)(void* iface, fb::GFxValue* value, void* data);
        using InvokeFn = bool(__thiscall*)(void* iface, void* data, fb::GFxValue* result, const char* name, const fb::GFxValue* args, unsigned int count, bool isDisplayObject);
        using GetMemberFn = bool(__thiscall*)(void* iface, void* data, const char* name, fb::GFxValue* out, bool isDisplayObject);
        using SetMemberFn = bool(__thiscall*)(void* iface, void* data, const char* name, const fb::GFxValue* value, bool isDisplayObject);
        using DisplayFn = void(__thiscall*)(fb::GFxMovieView* movie);
        using AdvanceFn = float(__thiscall*)(fb::GFxMovieView* movie, float dt, unsigned int catchUp);
        using InitializeScreenFn = void(__thiscall*)(fb::UIScreenManager* manager, const char* screen);

        DisplayFn oDisplay = nullptr;
        AdvanceFn oAdvance = nullptr;
        InitializeScreenFn oInitializeScreen = nullptr;

        constexpr unsigned int WIDGET_MANAGED = 0x40;
        constexpr unsigned short VARS_TRANSFORM = 0x1 | 0x2 | 0x8 | 0x10;
        constexpr unsigned short VARS_ALPHA = 0x20; // SetDisplayInfo sub_12F0240: m_alpha in percent

        enum WidgetType
        {
            NUMBER = 3,
            STRING = 4
        };

        // parent -> stage: stage = offset + scale * p
        struct Frame
        {
            double x = 0.0, y = 0.0, sx = 1.0, sy = 1.0;
        };

        class Clip
        {
        public:
            Clip() = default;
            Clip(const Clip&) = delete;
            Clip& operator=(const Clip&) = delete;

            ~Clip()
            {
                if (m_value.m_type & WIDGET_MANAGED)
                    reinterpret_cast<ObjectReleaseFn>(OFF_GFxValue_objectRelease)(m_value.m_objectInterface, &m_value, m_value.m_data);
            }

            bool resolve(fb::GFxMovieView* movie, const char* path)
            {
                return movie->getVariable(&m_value, path) && m_value.m_objectInterface;
            }

            bool read(fb::GFxDisplayInfo* info)
            {
                return reinterpret_cast<GetDisplayInfoFn>(OFF_GFxValue_getDisplayInfo)(m_value.m_objectInterface, m_value.m_data, info);
            }

            void write(const fb::GFxDisplayInfo* info)
            {
                reinterpret_cast<SetDisplayInfoFn>(OFF_GFxValue_setDisplayInfo)(m_value.m_objectInterface, m_value.m_data, info);
            }

            bool set(const char* member, const fb::GFxValue& value)
            {
                return reinterpret_cast<SetMemberFn>(OFF_GFxValue_setMember)(m_value.m_objectInterface, m_value.m_data, member, &value, true);
            }

            // ActionScript method call
            bool invoke(const char* method, const fb::GFxValue* args, unsigned int count, double* number = nullptr)
            {
                fb::GFxValue result{ };
                const bool ok = reinterpret_cast<InvokeFn>(OFF_GFxValue_invoke)(m_value.m_objectInterface, m_value.m_data, &result, method, args, count, true);

                if (number)
                    *number = result.m_type == NUMBER ? result.m_number : 0.0;
                if (result.m_type & WIDGET_MANAGED)
                    reinterpret_cast<ObjectReleaseFn>(OFF_GFxValue_objectRelease)(result.m_objectInterface, &result, result.m_data);

                return ok;
            }

            // MovieClip.getBounds()
            bool localBounds(double out[4])
            {
                fb::GFxValue result{ };
                const bool invoked = reinterpret_cast<InvokeFn>(OFF_GFxValue_invoke)(m_value.m_objectInterface, m_value.m_data, &result, "getBounds", nullptr, 0, true);
                
                assert(invoked && result.m_objectInterface);

                constexpr const char* names[4] = { "xMin", "yMin", "xMax", "yMax" };
                bool ok = result.m_objectInterface != nullptr;
                for (int i = 0; ok && i < 4; ++i)
                {
                    fb::GFxValue member{ };
                    ok = reinterpret_cast<GetMemberFn>(OFF_GFxValue_getMember)(result.m_objectInterface, result.m_data, names[i], &member, false) && member.m_type == NUMBER;
                    out[i] = member.m_number;
                }
                if (result.m_type & WIDGET_MANAGED)
                    reinterpret_cast<ObjectReleaseFn>(OFF_GFxValue_objectRelease)(result.m_objectInterface, &result, result.m_data);
                return ok;
            }

        private:
            fb::GFxValue m_value{ };
        };

        struct Scaled
        {
            Clip clip;
            fb::GFxDisplayInfo base{ };
        };
        std::vector<std::unique_ptr<Scaled>> g_scaled;

        // measured after Advance (getBounds runs ActionScript, which hangs inside Display)
        std::unordered_map<std::string, std::pair<double, double>> g_corners;

        Frame clipFrame(fb::GFxMovieView* movie, const std::string& path)
        {
            Frame frame;
            Clip clip;
            fb::GFxDisplayInfo info{ };

            if (clip.resolve(movie, path.c_str()) && clip.read(&info))
                frame = Frame{ info.m_x, info.m_y, info.m_xScale / 100.0, info.m_yScale / 100.0 };

            return frame;
        }

        // "<screen>" under _root, then its "instance1": the widgets' parent
        Frame screenFrame(fb::GFxMovieView* movie, const char* screen)
        {
            const Frame a = clipFrame(movie, screen);
            const Frame b = clipFrame(movie, std::string(screen) + ".instance1");
            return Frame{ a.x + a.sx * b.x, a.y + a.sy * b.y, a.sx * b.sx, a.sy * b.sy };
        }

        // the point of the visible stage the widget is aligned to, in its parent's space
        void alignAnchor(const fb::WidgetNode* widget, const fb::GRectF& visible, const Frame& parent, double& ax, double& ay)
        {
            double x = (visible.m_left + visible.m_right) * 0.5;
            if (widget->m_HorisontalAlign == fb::WHA_Left)
                x = visible.m_left;
            else if (widget->m_HorisontalAlign == fb::WHA_Right)
                x = visible.m_right;

            double y = (visible.m_top + visible.m_bottom) * 0.5;
            if (widget->m_VerticalAlign == fb::WVA_Top)
                y = visible.m_top;
            else if (widget->m_VerticalAlign == fb::WVA_Bottom)
                y = visible.m_bottom;

            ax = (x - parent.x) / parent.sx;
            ay = (y - parent.y) / parent.sy;
        }

        bool hasHudList(const fb::UIScreenData* screen)
        {
            for (fb::WidgetNode** w = screen->m_widgetsBegin; w != screen->m_widgetsEnd; ++w)
            {
                if (*w && (*w)->m_InstanceName && std::strcmp((*w)->m_InstanceName, HUD_LIST_NAME) == 0)
                    return true;
            }
            return false;
        }

        // the Gameplay movie has no clip for the added list: List_01 is duplicated before the page's initializeScreen sets
        // its widgets up by instance name (UIScreen.as)
        void createHudListClip(fb::GFxMovieView* movie, fb::UIScreenData* screen)
        {
            constexpr double kGap = 18.0;
            const std::string parent = std::string(screen->m_instanceName) + ".instance1";
            Clip holder, main, created;
            fb::GFxDisplayInfo mainInfo{ }, info{ };
            double depth = 0.0, mainBounds[4], ownBounds[4];
            fb::GFxValue args[2]{ };
            args[0].m_type = STRING;
            args[0].m_data = const_cast<char*>(HUD_LIST_NAME);
            args[1].m_type = NUMBER;
            bool made = holder.resolve(movie, parent.c_str()) && holder.invoke("getNextHighestDepth", nullptr, 0, &depth)
                && main.resolve(movie, (parent + ".List_01").c_str()) && main.read(&mainInfo) && main.localBounds(mainBounds)
                && (args[1].m_number = depth, main.invoke("duplicateMovieClip", args, 2))
                && created.resolve(movie, (parent + "." + HUD_LIST_NAME).c_str()) && created.read(&info) && created.localBounds(ownBounds);
            if (made)
            {
                // List_01's mask spans x -5..width+5 and y 0..height
                const double left = mainInfo.m_x + (mainBounds[2] - 5.0) + kGap;
                const double width = -mainInfo.m_x - left;
                const double height = mainBounds[3];
                info.m_x = left;
                info.m_y = mainInfo.m_y;
                info.m_xScale = width / (ownBounds[2] - ownBounds[0]) * 100.0;
                info.m_yScale = height / (ownBounds[3] - ownBounds[1]) * 100.0;
                info.m_varsSet = 0x1 | 0x2 | 0x8 | 0x10;
                created.write(&info);
                made = created.invoke("onLoad", nullptr, 0);
                log("{}: clip on {} at ({:.0f}, {:.0f}) {:.0f} x {:.0f}, depth {:.0f}", HUD_LIST_NAME, screen->m_instanceName, left, info.m_y, width, height, depth);
            }
            if (made)
            {
                fb::GFxValue none{ };
                none.m_type = 1U;
                created.set("onLoad", none);
                return;
            }

            log("{}: clip NOT created on {}", HUD_LIST_NAME, screen->m_instanceName);
            fb::WidgetNode** end = std::remove_if(screen->m_widgetsBegin, screen->m_widgetsEnd, [](const fb::WidgetNode* w)
            {
                return w && w->m_InstanceName && std::strcmp(w->m_InstanceName, HUD_LIST_NAME) == 0;
            });
            screen->m_widgetsEnd = end;
        }

        struct WidgetContext
        {
            fb::GFxMovieView* movie;
            const fb::UIScreenData* screen;
            const fb::WidgetNode* widget;
            std::string path;
            double k, layout;
            double alpha;
            double ax, ay; // the aligned screen corner in the widget's parent space
        };

        // every scaled widget of the root movie's screens
        template <typename Visit>
        void forEachScaledWidget(fb::GFxMovieView* movie, Visit&& visit)
        {
            fb::UISystem* system = fb::UISystem::GetInstance();
            fb::UIScreenManager* screens = system ? system->m_screenManager : nullptr;
            if (!screens || !screens->m_rootMovie || screens->m_rootMovie->m_movieView != movie)
                return;

            fb::GRectF visible{ };
            bool haveVisible = false;
            for (fb::UIScreenData** s = screens->m_screensBegin; s != screens->m_screensEnd; ++s)
            {
                const fb::UIScreenData* screen = *s;
                if (!screen)
                    continue;

                bool haveFrame = false;
                Frame parent;
                for (fb::WidgetNode** w = screen->m_widgetsBegin; w != screen->m_widgetsEnd; ++w)
                {
                    const fb::WidgetNode* widget = *w;
                    if (!widget || !widget->m_InstanceName || !widget->m_WidgetAsset || !widget->m_WidgetAsset->m_Name)
                        continue;

                    WidgetGroup group;
                    if (!widgetGroup(widget->m_WidgetAsset->m_Name, group))
                        continue;

                    WidgetContext context{ movie, screen, widget };
                    context.k = elementFactor(group.element);
                    context.layout = elementFactor(group.layout);
                    context.alpha = elementOpacity(group.element);
                    if (context.k == 1.0 && context.layout == 1.0 && context.alpha == 1.0)
                        continue;

                    if (!haveVisible)
                    {
                        movie->getVisibleFrameRect(&visible);
                        haveVisible = true;
                    }
                    if (!haveFrame)
                    {
                        parent = screenFrame(movie, screen->m_instanceName);
                        haveFrame = true;
                    }

                    alignAnchor(widget, visible, parent, context.ax, context.ay);
                    context.path = std::string(screen->m_instanceName) + ".instance1." + widget->m_InstanceName;
                    visit(context);
                }
            }
        }

        // after ActionScript: the bounds corner nearest to the widget's screen corner, for widgets sized apart from their layout
        void measureWidgets(fb::GFxMovieView* movie)
        {
            g_corners.clear();
            forEachScaledWidget(movie, [](const WidgetContext& c)
            {
                if (c.k == c.layout)
                    return;

                Clip clip;
                fb::GFxDisplayInfo info{ };
                double bounds[4];

                if (!clip.resolve(c.movie, c.path.c_str()) || !clip.read(&info) || !clip.localBounds(bounds))
                    return;

                const double x0 = info.m_x + bounds[0] * info.m_xScale / 100.0, x1 = info.m_x + bounds[2] * info.m_xScale / 100.0;
                const double y0 = info.m_y + bounds[1] * info.m_yScale / 100.0, y1 = info.m_y + bounds[3] * info.m_yScale / 100.0;
                const double cx = std::abs(x0 - c.ax) <= std::abs(x1 - c.ax) ? x0 : x1;
                const double cy = std::abs(y0 - c.ay) <= std::abs(y1 - c.ay) ? y0 : y1;
                g_corners[c.path] = { cx - info.m_x, cy - info.m_y };
            });
        }

        // ActionScript never sees the scaled transforms: they only exist while the movie is drawn
        void scaleWidgets(fb::GFxMovieView* movie)
        {
            forEachScaledWidget(movie, [](const WidgetContext& c)
            {
                auto scaled = std::make_unique<Scaled>();
                if (!scaled->clip.resolve(c.movie, c.path.c_str()) || !scaled->clip.read(&scaled->base))
                    return;

                // the corner follows the layout factor, the widget grows from it
                fb::GFxDisplayInfo info = scaled->base;
                double cx = info.m_x, cy = info.m_y;

                if (const auto corner = g_corners.find(c.path); corner != g_corners.end())
                {
                    cx += corner->second.first;
                    cy += corner->second.second;
                }

                const double tx = c.ax + (cx - c.ax) * c.layout;
                const double ty = c.ay + (cy - c.ay) * c.layout;
                info.m_x = tx + (info.m_x - cx) * c.k;
                info.m_y = ty + (info.m_y - cy) * c.k;
                info.m_xScale *= c.k;
                info.m_yScale *= c.k;
                info.m_alpha *= c.alpha; // on top of the widget's own fades
                info.m_varsSet = VARS_TRANSFORM | VARS_ALPHA;
                scaled->clip.write(&info);
                g_scaled.push_back(std::move(scaled));
            });
        }

        void restoreWidgets()
        {
            for (auto& scaled : g_scaled)
            {
                scaled->base.m_varsSet = VARS_TRANSFORM | VARS_ALPHA;
                scaled->clip.write(&scaled->base);
            }
            g_scaled.clear();
        }

        void __fastcall hkInitializeScreen(fb::UIScreenManager* _this, void*, const char* name)
        {
            linkHudList();
            for (fb::UIScreenData** s = _this->m_screensBegin; s != _this->m_screensEnd; ++s)
            {
                if (*s && std::strcmp((*s)->m_instanceName, name) == 0 && hasHudList(*s) && _this->m_rootMovie)
                    createHudListClip(_this->m_rootMovie->m_movieView, *s);
            }
            oInitializeScreen(_this, name);
        }

        float __fastcall hkAdvance(fb::GFxMovieView* _this, void*, float dt, unsigned int catchUp)
        {
            const float next = oAdvance(_this, dt, catchUp);
            measureWidgets(_this);
            return next;
        }

        void __fastcall hkDisplay(fb::GFxMovieView* _this, void*)
        {
            scaleWidgets(_this);
            oDisplay(_this);
            restoreWidgets();
        }
    }

    void installWidgetHooks()
    {
        hook(OFF_UIScreenManager_initializeScreen, hkInitializeScreen, &oInitializeScreen);
        hook(OFF_GFxMovieRoot_advance, hkAdvance, &oAdvance);
        hook(OFF_GFxMovieRoot_display, hkDisplay, &oDisplay);
    }
}
