#include "hudfix.h"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cassert>

namespace hudfix
{
    namespace
    {
        using GetDisplayInfoFn = bool(__thiscall*)(void* iface, void* data, fb::GFxDisplayInfo* out);
        using SetDisplayInfoFn = bool(__thiscall*)(void* iface, void* data, const fb::GFxDisplayInfo* info);
        using GetCxformFn = bool(__thiscall*)(void* iface, void* data, fb::GFxCxform* out);
        using SetCxformFn = bool(__thiscall*)(void* iface, void* data, const fb::GFxCxform* cxform);
        using ObjectReleaseFn = void(__thiscall*)(void* iface, fb::GFxValue* value, void* data);
        using ResolveCharacterFn = unsigned char*(__thiscall*)(void* handle, void* movieRoot);
        using GetHandleFn = unsigned char*(__thiscall*)(unsigned char* character);
        using ObjectTypeFn = int(__thiscall*)(const unsigned char* object);
        using InvokeFn = bool(__thiscall*)(void* iface, void* data, fb::GFxValue* result, const char* name, const fb::GFxValue* args, unsigned int count, bool isDisplayObject);
        using GetMemberFn = bool(__thiscall*)(void* iface, void* data, const char* name, fb::GFxValue* out, bool isDisplayObject);
        using SetMemberFn = bool(__thiscall*)(void* iface, void* data, const char* name, const fb::GFxValue* value, bool isDisplayObject);
        struct MemberVisitor;
        using VisitMembersFn = void(__thiscall*)(void* iface, void* data, MemberVisitor* visitor, bool isDisplayObject);
        using DisplayFn = void(__thiscall*)(fb::GFxMovieView* movie);
        using AdvanceFn = float(__thiscall*)(fb::GFxMovieView* movie, float dt, unsigned int catchUp);
        using InitializeScreenFn = void(__thiscall*)(fb::UIScreenManager* manager, const char* screen);
        using SetScreenVisibleFn = bool(__thiscall*)(fb::UIScreenManager* manager, fb::UIScreenData* screen, bool visible);
        using OnMessageFn = void(__thiscall*)(void* component, const unsigned char* message);
        using GetPlayerFn = fb::ClientPlayer*(__thiscall*)(void* manager, unsigned int id);

        DisplayFn oDisplay = nullptr;
        AdvanceFn oAdvance = nullptr;
        InitializeScreenFn oInitializeScreen = nullptr;
        SetScreenVisibleFn oSetScreenVisible = nullptr;
        OnMessageFn oWeaponMessage = nullptr;
        OnMessageFn oHudTextMessage = nullptr;
        OnMessageFn oBulletHit = nullptr;

        constexpr int MESSAGE_DAMAGE_GIVEN = 93974635; // UIDamageGivenToEnemyMessage
        constexpr int MESSAGE_KILLED_OTHER = -1963112840; // UINetworkKilledOtherPlayerMessage
        constexpr int MESSAGE_BULLET_HIT = 0x47F6C6F0; // BulletToSoldierComponentsHitMessage
        constexpr int BULLET_HIT_BONE = 0x58;
        constexpr int BULLET_HIT_SHOOTER = 0x5C;
        constexpr unsigned int HEAD_BONE = 1;
        constexpr int COMPONENT_SOLDIER = 0x70; // the soldier a soldier component belongs to
        constexpr int SOLDIER_TEAM = 0x78;
        constexpr int PLAYER_MANAGER = 0x30;

        // ghetto
        constexpr long long KILL_WINDOW_MS = 300;
        constexpr long long HEADSHOT_WINDOW_MS = 1000;

        std::atomic<Element> g_hitMarker{ HitMarkers };
        std::atomic<long long> g_lastKillTime{ -KILL_WINDOW_MS };
        std::atomic<long long> g_lastHeadshotTime{ -HEADSHOT_WINDOW_MS };

        // the commo rose picks its options by mouse distance in screen pixels, so ActionScript sees its screen at the unscaled size
        constexpr unsigned int COMMO_ROSE_SCREEN[4] = { 0xC9639154, 0x4E99D107, 0x9569DEB4, 0x66673E52 };

        constexpr unsigned int WIDGET_MANAGED = 0x40;
        constexpr unsigned short VARS_TRANSFORM = 0x1 | 0x2 | 0x8 | 0x10;
        constexpr unsigned short VARS_ALPHA = 0x20; // SetDisplayInfo sub_12F0240: m_alpha in percent
        constexpr unsigned short VARS_VISIBLE = 0x40;
        
        // for chase veh crosshair
        constexpr int THICKEN_PASSES = 8;
        constexpr double THICKEN_PX = 1.5;
        
        constexpr int CHARACTER_MATRIX = 0x44;
        constexpr int GET_BOUNDS_SLOT = 13;
        // a shape character's definition (GFxGenericCharacter::pDef, +0x78 in the 2010 build), OFF_vt_GFxConstShapeCharacterDef's styles
        constexpr int CHARACTER_DEF = 0x7C;
        constexpr int SHAPE_FILL_STYLES = 0x3C;
        constexpr int SHAPE_LINE_STYLES = 0x40;
        constexpr int SHAPE_FILL_COUNT = 0x44;
        constexpr int SHAPE_LINE_COUNT = 0x48;
        constexpr int FILL_STYLE_SIZE = 40;
        constexpr int FILL_STYLE_COLOR = 4; // GColor 0xAARRGGBB, Type +0 is 0 for a solid fill
        constexpr int LINE_STYLE_SIZE = 20;
        constexpr int LINE_STYLE_COLOR = 8;
        constexpr unsigned int MAX_STYLES = 256;

        // a color transform per channel: mult, then add (0..1)
        struct Recolor
        {
            float mult[3];
            float add[3];
        };
        constexpr int MAX_SHAPES = 256;
        constexpr int SHAPE_LEVELS = 6;
        struct ShapeMatrix
        {
            float* matrix;
            float saved[6];
        };
        ShapeMatrix g_shapes[MAX_SHAPES];
        int g_shapeCount = 0;
        constexpr int CHARACTER_CXFORM = 0x24; // GRenderer::Cxform, rows of { mult, add }
        struct ShapeCxform
        {
            fb::GFxCxform* cxform;
            fb::GFxCxform saved;
        };
        // every shape of every vehicle screen in the list, shown or not, about 35 a widget
        constexpr int MAX_SHAPE_CXFORMS = 8192;
        ShapeCxform g_shapeCxforms[MAX_SHAPE_CXFORMS];
        int g_shapeCxformCount = 0;
        // Flash blend modes (GRenderer::BlendType)
        constexpr int SPRITE_BLEND_MODE = 0xA8;
        constexpr unsigned char BLEND_NORMAL = 1;
        constexpr unsigned char BLEND_ADD = 8;
        struct BlendSave
        {
            unsigned char* blend;
            unsigned char saved;
        };
        BlendSave g_blendSaves[MAX_SHAPES];
        int g_blendSaveCount = 0;
        // GFxASCharacter Flags (GetAcceptAnimMoves sub_1316D40)
        constexpr int CHARACTER_FLAGS = 0xA0;
        constexpr unsigned int ACCEPT_ANIM_MOVES = 0x1000;

        enum WidgetType
        {
            NUMBER = 3,
            STRING = 4
        };

        // GFxSprite display list (getInstanceAtDepth sub_136DC90)
        constexpr int CHARACTER_OBJECT = 124;
        constexpr int CHARACTER_FLAGS_BYTE = 122;
        constexpr unsigned char CHARACTER_SCRIPTED = 0x80;
        constexpr int CHARACTER_HANDLE = 148;
        constexpr int HANDLE_NAME = 8;
        constexpr int SPRITE_CHILDREN = 196;
        constexpr int SPRITE_CHILD_COUNT = 200;
        // GFxCharacter Depth, a timeline mask's ClipDepth: it masks the children after it up to that depth (the hit test mask list sub_137EB70)
        constexpr int CHARACTER_DEPTH = 0x14;
        constexpr int CHARACTER_CLIP_DEPTH = 0x78;
        constexpr int SPRITE_MASK = 0x1CC;
        constexpr unsigned int CHARACTER_VISIBLE = 0x1;
        constexpr unsigned int CHARACTER_USED_AS_MASK = 0x8000;
        constexpr float TWIPS = 20.0f;
        constexpr int OUTLINE_LEVELS = 8;
        constexpr int OBJECT_SPRITE = 2;
        constexpr int MAX_CHILDREN = 64;

        constexpr int CHILD_LEVELS = 4;
        constexpr double MOVED = 0.5;
        constexpr ULONGLONG RESCAN_MS = 1000;

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

            bool readCxform(fb::GFxCxform* cxform)
            {
                return reinterpret_cast<GetCxformFn>(OFF_GFxValue_getCxform)(m_value.m_objectInterface, m_value.m_data, cxform);
            }

            void writeCxform(const fb::GFxCxform* cxform)
            {
                reinterpret_cast<SetCxformFn>(OFF_GFxValue_setCxform)(m_value.m_objectInterface, m_value.m_data, cxform);
            }

            // ObjectInterface: movie root +0x0, the value's data is the character handle
            unsigned char* character()
            {
                void* movieRoot = m_value.m_objectInterface ? *static_cast<void**>(m_value.m_objectInterface) : nullptr;
                return movieRoot ? reinterpret_cast<ResolveCharacterFn>(OFF_GFxCharacterHandle_resolve)(m_value.m_data, movieRoot) : nullptr;
            }

            unsigned int* characterFlags()
            {
                unsigned char* owner = character();
                return owner ? reinterpret_cast<unsigned int*>(owner + CHARACTER_FLAGS) : nullptr;
            }

            // replace keeps only the alpha (add is in 0..255, Cxform::Transform
            // else the clip's own colors are multiplied
            void tint(const Color& color, bool replace)
            {
                fb::GFxCxform tinted{ };
                if (!readCxform(&tinted))
                    return;

                const float rgb[3] = { color.r, color.g, color.b };
                for (int i = 0; i < 3; ++i)
                {
                    if (replace)
                    {
                        tinted.m_rows[i][0] = 0.0f;
                        tinted.m_rows[i][1] = rgb[i] * 255.0f;
                    }
                    else
                    {
                        tinted.m_rows[i][0] *= rgb[i];
                        tinted.m_rows[i][1] *= rgb[i];
                    }
                }
                writeCxform(&tinted);
            }

            // per channel: the clip's colors times mult plus add (0..1)
            void recolor(const Recolor& r)
            {
                fb::GFxCxform cxform{ };
                if (!readCxform(&cxform))
                    return;
                for (int i = 0; i < 3; ++i)
                {
                    cxform.m_rows[i][1] = cxform.m_rows[i][1] * r.mult[i] + r.add[i] * 255.0f;
                    cxform.m_rows[i][0] *= r.mult[i];
                }
                writeCxform(&cxform);
            }

            bool set(const char* member, const fb::GFxValue& value)
            {
                return reinterpret_cast<SetMemberFn>(OFF_GFxValue_setMember)(m_value.m_objectInterface, m_value.m_data, member, &value, true);
            }

            // a method's result kept in out, like TextField.getTextFormat
            bool invokeInto(const char* method, const fb::GFxValue* args, unsigned int count, Clip& out)
            {
                return reinterpret_cast<InvokeFn>(OFF_GFxValue_invoke)(m_value.m_objectInterface, m_value.m_data, &out.m_value, method, args, count, true)
                    && out.m_value.m_objectInterface;
            }

            // the value itself, to pass as an argument
            const fb::GFxValue* value() const
            {
                return &m_value;
            }

            // a member of a plain object (not a display object), like a TextFormat's size
            bool setField(const char* member, const fb::GFxValue& value)
            {
                return reinterpret_cast<SetMemberFn>(OFF_GFxValue_setMember)(m_value.m_objectInterface, m_value.m_data, member, &value, false);
            }

            double fieldNumber(const char* member, double fallback)
            {
                fb::GFxValue out{ };
                const bool ok = reinterpret_cast<GetMemberFn>(OFF_GFxValue_getMember)(m_value.m_objectInterface, m_value.m_data, member, &out, false);
                const double result = ok && out.m_type == NUMBER ? out.m_number : fallback;
                if (out.m_type & WIDGET_MANAGED)
                    reinterpret_cast<ObjectReleaseFn>(OFF_GFxValue_objectRelease)(out.m_objectInterface, &out, out.m_data);
                return result;
            }

            // a string member, empty if it is none
            std::string string(const char* member)
            {
                fb::GFxValue out{ };
                const bool ok = reinterpret_cast<GetMemberFn>(OFF_GFxValue_getMember)(m_value.m_objectInterface, m_value.m_data, member, &out, true);
                // a string of the movie is managed: its data points at the text's pointer
                const char* chars = nullptr;
                if (ok && (out.m_type & 0x3F) == STRING && out.m_data)
                    chars = out.m_type & WIDGET_MANAGED ? *static_cast<const char* const*>(out.m_data) : static_cast<const char*>(out.m_data);
                std::string result = chars ? chars : "";
                if (out.m_type & WIDGET_MANAGED)
                    reinterpret_cast<ObjectReleaseFn>(OFF_GFxValue_objectRelease)(out.m_objectInterface, &out, out.m_data);
                return result;
            }

            // the names of its display object members (child clips, text fields)
            std::vector<std::string> children();

            double number(const char* member, double fallback)
            {
                fb::GFxValue out{ };
                const bool ok = reinterpret_cast<GetMemberFn>(OFF_GFxValue_getMember)(m_value.m_objectInterface, m_value.m_data, member, &out, true);
                const double result = ok && out.m_type == NUMBER ? out.m_number : fallback;
                if (out.m_type & WIDGET_MANAGED)
                    reinterpret_cast<ObjectReleaseFn>(OFF_GFxValue_objectRelease)(out.m_objectInterface, &out, out.m_data);
                return result;
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

            // MovieClip.getBounds(), in the space of target if given
            bool localBounds(double out[4], const Clip* target = nullptr)
            {
                fb::GFxValue result{ };
                const bool invoked = reinterpret_cast<InvokeFn>(OFF_GFxValue_invoke)(m_value.m_objectInterface, m_value.m_data, &result, "getBounds",
                    target ? target->value() : nullptr, target ? 1 : 0, true);
                
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
            fb::GFxCxform baseCxform{ };
            bool tinted = false;
            unsigned int* flags = nullptr;
            unsigned int acceptAnimMoves = 0;

            // before any write
            void keepTimeline()
            {
                flags = clip.characterFlags();
                acceptAnimMoves = flags ? *flags & ACCEPT_ANIM_MOVES : 0;
            }
        };
        std::vector<std::unique_ptr<Scaled>> g_scaled;

        // measured after Advance (getBounds runs ActionScript, which hangs inside Display)
        std::unordered_map<std::string, std::pair<double, double>> g_corners;
        // screen -> the minimap and compass bounds { x0, y0, x1, y1 } in the widgets' parent space, measured unscaled
        std::unordered_map<std::string, std::array<double, 4>> g_minimapOutline;
        // the minimap background's path -> its bounds { x0, y0, x1, y1 } in the parent space, measured unscaled
        std::unordered_map<std::string, std::array<double, 4>> g_backgroundBounds;

        // a clip inside a vehicle widget
        struct Child
        {
            std::string path;
            double restX = 0.0, restY = 0.0;
            bool moving = false;
        };
        struct ChildSet
        {
            std::vector<Child> clips;
            ULONGLONG scanned = 0;
        };
        // vehicle widget path -> its clips, looked for again every RESCAN_MS
        std::unordered_map<std::string, ChildSet> g_children;

        // a part of the minimap or its compass with its bounds in the widget's space, while they hold still
        struct OutlinePart
        {
            std::array<double, 4> bounds{ };
            bool varied = false;
        };
        // character -> what is known of it, the parts drawn this frame kept
        std::unordered_map<const unsigned char*, OutlinePart> g_outlineParts;

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

        bool hasWidget(const fb::UIScreenData* screen, const char* instanceName)
        {
            for (fb::WidgetNode** w = screen->m_widgetsBegin; w != screen->m_widgetsEnd; ++w)
            {
                if (*w && (*w)->m_InstanceName && std::strcmp((*w)->m_InstanceName, instanceName) == 0)
                    return true;
            }
            return false;
        }

        // a widget whose clip could not be made, so initializeScreen does not look for it
        void removeWidget(fb::UIScreenData* screen, const char* instanceName)
        {
            fb::WidgetNode** end = std::remove_if(screen->m_widgetsBegin, screen->m_widgetsEnd, [instanceName](const fb::WidgetNode* w)
            {
                return w && w->m_InstanceName && std::strcmp(w->m_InstanceName, instanceName) == 0;
            });
            screen->m_widgetsEnd = end;
        }

        constexpr double TABS_GAP = 8.0;
        constexpr int TABS_FIT_FRAMES = 120; // how long the tab bar may take to lay its tabs out
        // list's height has to be known before its onLoad, which turns its clip scale into its mask size
        constexpr double TABS_BOUNDS[4] = { -1.0, -8.0, 1091.0, 64.0 };

        struct HudTabsArea
        {
            fb::GFxMovieView* movie = nullptr;
            std::string parent;
            double left = 0.0, top = 0.0, width = 0.0;
            int fitFrames = 0; // left to try, 0 when placed or without tabs
        };
        HudTabsArea g_tabsArea;

        double tabsScale(double width, const double bounds[4])
        {
            return (std::min)(1.0, width / (bounds[2] - bounds[0]));
        }

        // scaled into the list's width over its top, by the bounds the tabs really got
        void fitHudTabs(fb::GFxMovieView* movie)
        {
            HudTabsArea& a = g_tabsArea;
            if (!a.fitFrames || movie != a.movie)
                return;

            --a.fitFrames;
            Clip tabs;
            fb::GFxDisplayInfo info{ };
            double b[4];
            if (!tabs.resolve(movie, (a.parent + "." + HUD_TABS_NAME).c_str()) || !tabs.localBounds(b) || !tabs.read(&info))
                return;

            const bool placeholder = b[0] == 0.0 && b[1] == 0.0 && b[2] == 100.0 && b[3] == 100.0;
            if (placeholder || b[2] <= b[0] || b[3] <= b[1])
                return;

            const double scale = tabsScale(a.width, b);
            info.m_x = a.left - b[0] * scale;
            info.m_y = a.top - b[1] * scale;
            info.m_xScale = info.m_yScale = scale * 100.0;
            info.m_varsSet = 0x1 | 0x2 | 0x8 | 0x10;
            tabs.write(&info);
            a.fitFrames = 0;
            log("{}: tabs {:.0f},{:.0f} .. {:.0f},{:.0f} at {:.0f}%", HUD_TABS_NAME, b[0], b[1], b[2], b[3], scale * 100.0);
        }

        // TabBar_01 duplicated over the list's top; initializeScreen then gives it HUD_TABS_NAME's tabs, fitHudTabs places it
        double createHudTabsClip(fb::GFxMovieView* movie, fb::UIScreenData* screen, Clip& holder, const std::string& parent, double left, double top, double width)
        {
            if (!hasWidget(screen, HUD_TABS_NAME))
                return 0.0;

            Clip main, created;
            fb::GFxDisplayInfo info{ };
            double depth = 0.0;
            fb::GFxValue args[2]{ };
            args[0].m_type = STRING;
            args[0].m_data = const_cast<char*>(HUD_TABS_NAME);
            args[1].m_type = NUMBER;
            bool made = holder.invoke("getNextHighestDepth", nullptr, 0, &depth)
                && main.resolve(movie, (parent + ".TabBar_01").c_str())
                && (args[1].m_number = depth, main.invoke("duplicateMovieClip", args, 2))
                && created.resolve(movie, (parent + "." + HUD_TABS_NAME).c_str()) && created.read(&info);
            if (made)
            {
                info.m_x = left;
                info.m_y = top;
                info.m_varsSet = 0x1 | 0x2;
                created.write(&info);
                made = created.invoke("onLoad", nullptr, 0);
                log("{}: clip on {} at ({:.0f}, {:.0f})", HUD_TABS_NAME, screen->m_instanceName, info.m_x, info.m_y);
            }
            if (made)
            {
                fb::GFxValue none{ };
                none.m_type = 1U;
                created.set("onLoad", none);
                g_tabsArea = { movie, parent, left, top, width, TABS_FIT_FRAMES };
                return (TABS_BOUNDS[3] - TABS_BOUNDS[1]) * tabsScale(width, TABS_BOUNDS) + TABS_GAP;
            }

            log("{}: clip NOT created on {}", HUD_TABS_NAME, screen->m_instanceName);
            removeWidget(screen, HUD_TABS_NAME);
            return 0.0;
        }

        // GFxValue::ObjectInterface::ObjVisitor
        struct MemberVisitor
        {
            std::vector<std::string> found;

            virtual ~MemberVisitor() = default;
            virtual void visit(const char* name, const fb::GFxValue* value)
            {
                if (name && (value->m_type & 0x3F) == 8)
                    found.emplace_back(name);
            }
        };

        std::vector<std::string> Clip::children()
        {
            MemberVisitor visitor;
            reinterpret_cast<VisitMembersFn>(OFF_GFxValue_visitMembers)(m_value.m_objectInterface, m_value.m_data, &visitor, true);
            return visitor.found;
        }

        // all paddings are not automated by UI framework, because it was designed to work in flash
        constexpr const char* PICKER_NAME = "HudFixPicker";
        constexpr double ROW_HEIGHT = 28.0; // TextSelectorRow_28px
        constexpr double ROW_PITCH = 30.0;
        constexpr double ROW_BOUNDS_BOTTOM = 33.0; // a row clip's local bounds: -1,-1 .. 461,33
        constexpr double PANEL_GAP = 8.0; // under List_01's last row
        constexpr double PANEL_PAD = 10.0;
        constexpr double SQUARE = 124.0;
        constexpr double HUE_GAP = 8.0;
        constexpr double HUE_WIDTH = 16.0;
        constexpr double SWATCH_WIDTH = 150.0;
        constexpr double SWATCH_HEIGHT = 36.0;
        constexpr double CAPTION_HEIGHT = 20.0;
        constexpr double BODY_Y = ROW_PITCH; // the body under the header row, a row spacing apart
        constexpr double BODY_HEIGHT = PANEL_PAD + SQUARE + PANEL_PAD;
        constexpr double SQUARE_X = PANEL_PAD;
        constexpr double SQUARE_Y = BODY_Y + PANEL_PAD;
        constexpr double HUE_X = SQUARE_X + SQUARE + HUE_GAP;
        constexpr double ALPHA_X = HUE_X + HUE_WIDTH + HUE_GAP; // the opacity strip
        constexpr double SWATCH_X = ALPHA_X + HUE_WIDTH + 3.0 * PANEL_PAD;
        constexpr double OPACITY_LOW = 0.1; // the options' lowest opacity
        constexpr int ALPHA_STEPS = 24;
        constexpr double CHECKER = 8.0; // behind what has opacity
        constexpr int CHECKER_DARK = 0x2A2A2A;
        constexpr int CHECKER_LIGHT = 0x6A6A6A;
        constexpr ULONGLONG CLOSE_DELAY = 400; // ms another row stays highlighted before the picker closes
        constexpr double HEX_X = SWATCH_X + SWATCH_WIDTH + 2.0 * PANEL_PAD; // the hex input, right of the swatches
        constexpr double HEX_WIDTH = 120.0;
        constexpr int HEX_BOX = 0x0E141A;
        constexpr int ROW_TOP = 0x273640;
        constexpr int ROW_BOTTOM = 0x202D37;
        constexpr int ROW_LINE = 0x040810;
        constexpr double ROW_ALPHA = 92.0;
        constexpr int SHADES = 8; // steps of the row's shading
        constexpr int PICKER_STEPS = 40; // steps of the square's fades (beginGradientFill stays black)
        constexpr int HUE_SLICES = 72;

        enum class Drag
        {
            None,
            Square,
            Hue,
            Alpha
        };

        struct Picker
        {
            fb::GFxMovieView* movie = nullptr;
            std::string path; // the picker clip
            std::string listPath; // the shown HUD list
            std::string mainListPath; // List_01, the panel goes under its rows
            double x = 0.0, width = 0.0, listY = 0.0;
            int slot = -1; // color slot shown, -1 hidden
            int original = 0xFFFFFF; // the slot's color when it was shown
            double originalAlpha = 1.0;
            int element = ElementCount; // whose opacity it sets, ElementCount for none
            bool custom = true; // its row is on CUSTOM, else only the opacity is set
            bool hexEditing = false; // the hex box is typed into: the game gets no keyboard meanwhile
            std::string hexTyped;
            bool keyWas[256] = { }; // key states of the last frame, to see presses
            double h = 0.0, s = 0.0, v = 1.0;
            double alpha = 1.0;
            Drag drag = Drag::None;
            bool wasDown = false;
            ULONGLONG otherRowSince = 0; // another row is highlighted since, 0 if not
        };
        Picker g_picker;

        fb::GFxValue number(double value)
        {
            fb::GFxValue v{ };
            v.m_type = NUMBER;
            v.m_number = value;
            return v;
        }

        fb::GFxValue text(const char* value)
        {
            fb::GFxValue v{ };
            v.m_type = STRING;
            v.m_data = const_cast<char*>(value);
            return v;
        }

        fb::GFxValue boolean(bool value)
        {
            fb::GFxValue v{ };
            v.m_type = 2;
            *reinterpret_cast<bool*>(&v.m_data) = value;
            return v;
        }

        int hsvToRgb(double h, double s, double v)
        {
            const double c = v * s;
            const double x = c * (1.0 - std::fabs(std::fmod(h * 6.0, 2.0) - 1.0));
            const double m = v - c;
            double r = 0.0, g = 0.0, b = 0.0;
            switch (static_cast<int>(h * 6.0) % 6)
            {
            case 0: r = c; g = x; break;
            case 1: r = x; g = c; break;
            case 2: g = c; b = x; break;
            case 3: g = x; b = c; break;
            case 4: r = x; b = c; break;
            default: r = c; b = x; break;
            }
            const auto byte = [m](double channel) { return static_cast<int>(std::lround((channel + m) * 255.0)); };
            return byte(r) << 16 | byte(g) << 8 | byte(b);
        }

        void rgbToHsv(int rgb, double& h, double& s, double& v)
        {
            const double r = ((rgb >> 16) & 0xFF) / 255.0, g = ((rgb >> 8) & 0xFF) / 255.0, b = (rgb & 0xFF) / 255.0;
            const double high = (std::max)({ r, g, b }), low = (std::min)({ r, g, b }), d = high - low;
            v = high;
            s = high > 0.0 ? d / high : 0.0;
            if (d <= 0.0)
                return; // grey keeps the hue it had
            if (high == r)
                h = std::fmod((g - b) / d + 6.0, 6.0) / 6.0;
            else if (high == g)
                h = ((b - r) / d + 2.0) / 6.0;
            else
                h = ((r - g) / d + 4.0) / 6.0;
        }

        int mixRgb(int a, int b, double t)
        {
            int out = 0;
            for (int shift = 16; shift >= 0; shift -= 8)
            {
                const double channel = ((a >> shift) & 0xFF) * (1.0 - t) + ((b >> shift) & 0xFF) * t;
                out |= static_cast<int>(std::lround(channel)) << shift;
            }
            return out;
        }

        void drawRect(Clip& clip, double x, double y, double w, double h)
        {
            const fb::GFxValue start[2] = { number(x), number(y) };
            clip.invoke("moveTo", start, 2);
            const double corners[4][2] = { { x + w, y }, { x + w, y + h }, { x, y + h }, { x, y } };
            for (const auto& corner : corners)
            {
                const fb::GFxValue to[2] = { number(corner[0]), number(corner[1]) };
                clip.invoke("lineTo", to, 2);
            }
        }

        void fillRect(Clip& clip, double x, double y, double w, double h, int rgb, double alpha)
        {
            const fb::GFxValue fill[2] = { number(rgb), number(alpha) };
            clip.invoke("beginFill", fill, 2);
            drawRect(clip, x, y, w, h);
            clip.invoke("endFill", nullptr, 0);
        }

        // a frame just outside the rect
        void frameRect(Clip& clip, double x, double y, double w, double h, int rgb)
        {
            fillRect(clip, x - 1.0, y - 1.0, w + 2.0, 1.0, rgb, 100.0);
            fillRect(clip, x - 1.0, y + h, w + 2.0, 1.0, rgb, 100.0);
            fillRect(clip, x - 1.0, y, 1.0, h, rgb, 100.0);
            fillRect(clip, x + w, y, 1.0, h, rgb, 100.0);
        }

        // a color fading over the rect in strips, down when vertical
        void fadeRect(Clip& clip, double x, double y, double w, double h, int rgb, double fromAlpha, double toAlpha, bool vertical)
        {
            for (int i = 0; i < PICKER_STEPS; ++i)
            {
                const double alpha = fromAlpha + (toAlpha - fromAlpha) * (i + 0.5) / PICKER_STEPS;
                if (vertical)
                    fillRect(clip, x, y + h * i / PICKER_STEPS, w, h / PICKER_STEPS, rgb, alpha);
                else
                    fillRect(clip, x + w * i / PICKER_STEPS, y, w / PICKER_STEPS, h, rgb, alpha);
            }
        }

        // one of the menu's rows: shaded down, its dark line under it
        void rowRect(Clip& clip, double x, double y, double w, double h)
        {
            for (int i = 0; i < SHADES; ++i)
                fillRect(clip, x, y + h * i / SHADES, w, h / SHADES, mixRgb(ROW_TOP, ROW_BOTTOM, (i + 0.5) / SHADES), ROW_ALPHA);
            fillRect(clip, x, y + h, w, 1.0, ROW_LINE, 100.0);
        }

        void checker(Clip& clip, double x, double y, double w, double h)
        {
            fillRect(clip, x, y, w, h, CHECKER_DARK, 100.0);
            for (double cy = 0.0; cy < h; cy += CHECKER)
            {
                for (double cx = std::fmod(cy / CHECKER, 2.0) * CHECKER; cx < w; cx += 2.0 * CHECKER)
                    fillRect(clip, x + cx, y + cy, (std::min)(CHECKER, w - cx), (std::min)(CHECKER, h - cy), CHECKER_LIGHT, 100.0);
            }
        }

        // a color at an opacity over the checker
        void swatchRect(Clip& clip, double x, double y, double w, double h, int rgb, double alpha)
        {
            if (alpha < 1.0)
                checker(clip, x, y, w, h);
            fillRect(clip, x, y, w, h, rgb, alpha * 100.0);
        }

        bool pickerPart(const char* part, Clip& clip)
        {
            return clip.resolve(g_picker.movie, (g_picker.path + "." + part).c_str());
        }

        void moveClip(Clip& clip, double x, double y)
        {
            fb::GFxDisplayInfo info{ };
            info.m_x = x;
            info.m_y = y;
            info.m_varsSet = 0x1 | 0x2;
            clip.write(&info);
        }

        // List_01's row label format: a text field of one of its rows
        bool listTextFormat(Clip& format)
        {
            static bool logged = false;
            Clip holder;
            if (!holder.resolve(g_picker.movie, (g_picker.mainListPath + ".m_ListHolder").c_str()))
                return false;

            for (const std::string& row : holder.children())
            {
                if (row.rfind("row_", 0) != 0)
                    continue;

                // the row's label (TextSelectorRow_28px), not its dummyTextField!!!
                std::vector<std::pair<std::string, std::string>> texts;
                const std::string rowPath = g_picker.mainListPath + ".m_ListHolder." + row;

                Clip label;
                if (label.resolve(g_picker.movie, (rowPath + ".Label.txtField").c_str()) && !label.string("text").empty())
                    texts.emplace_back(rowPath + ".Label.txtField", label.string("text"));
                for (const auto& [path, value] : texts)
                {
                    Clip field;
                    if (!field.resolve(g_picker.movie, path.c_str()) || !field.invokeInto("getTextFormat", nullptr, 0, format))
                        continue;

                    if (!logged)
                    {
                        logged = true;
                        log("{}: font of {} ('{}'), size {:.0f}", PICKER_NAME, path, value, format.fieldNumber("size", 0.0));
                    }
                    return true;
                }
            }
            if (!logged)
            {
                logged = true;
                log("{}: no text field in {} rows", PICKER_NAME, g_picker.mainListPath);
            }
            return false;
        }

        // a text field in the list's font, at size percent of its rows' labels, aligned right when right
        void createLabel(Clip& picker, const char* name, int depth, double x, double y, double w, double h, double size, bool right)
        {
            const fb::GFxValue args[6] = { text(name), number(depth), number(x), number(y), number(w), number(h) };
            picker.invoke("createTextField", args, 6);
            Clip field, format;
            if (!pickerPart(name, field))
                return;

            field.set("selectable", boolean(false));
            if (listTextFormat(format))
            {
                field.set("embedFonts", boolean(true));
                const double base = format.fieldNumber("size", 0.0);

                if (base > 0.0)
                    format.setField("size", number(std::round(base * size)));
                if (right)
                    format.setField("align", text("right"));

                const fb::GFxValue formatArg[1] = { *format.value() };
                field.invoke("setNewTextFormat", formatArg, 1);
            }
        }

        void setLabel(const char* name, const char* value)
        {
            Clip field;
            if (pickerPart(name, field))
                field.set("text", text(value));
        }

        // the opacity strip's x: next to the hue strip, or in the square's place on DEFAULT
        double alphaX()
        {
            return g_picker.custom ? ALPHA_X : SQUARE_X;
        }

        void setVisible(Clip& clip, bool shown)
        {
            fb::GFxDisplayInfo info{ };
            info.m_visible = shown;
            info.m_varsSet = VARS_VISIBLE;
            clip.write(&info);
        }

        // the square for the hue, the opacity strip for the color, the new color's swatch, the value and the marks
        void drawPicker(bool square, bool strip)
        {
            Picker& p = g_picker;
            const int rgb = hsvToRgb(p.h, p.s, p.v);
            const bool hasAlpha = p.element != ElementCount;
            Clip part;
            if (square && pickerPart("square", part))
            {
                // the hue, white over it fading out to the right, black over both fading in downwards
                part.invoke("clear", nullptr, 0);
                fillRect(part, SQUARE_X, SQUARE_Y, SQUARE, SQUARE, hsvToRgb(p.h, 1.0, 1.0), 100.0);
                fadeRect(part, SQUARE_X, SQUARE_Y, SQUARE, SQUARE, 0xFFFFFF, 100.0, 0.0, false);
                fadeRect(part, SQUARE_X, SQUARE_Y, SQUARE, SQUARE, 0x000000, 0.0, 100.0, true);
                frameRect(part, SQUARE_X, SQUARE_Y, SQUARE, SQUARE, ROW_LINE);
            }
            Clip alphaStrip;
            if (strip && pickerPart("alpha", alphaStrip))
            {
                // the color from opaque at the top to the lowest opacity at the bottom
                alphaStrip.invoke("clear", nullptr, 0);
                if (hasAlpha)
                {
                    const double x = alphaX();
                    const int shown = p.custom ? rgb : 0xFFFFFF;
                    checker(alphaStrip, x, SQUARE_Y, HUE_WIDTH, SQUARE);
                    for (int i = 0; i < ALPHA_STEPS; ++i)
                    {
                        const double alpha = 1.0 - (1.0 - OPACITY_LOW) * (i + 0.5) / ALPHA_STEPS;
                        fillRect(alphaStrip, x, SQUARE_Y + SQUARE * i / ALPHA_STEPS, HUE_WIDTH, SQUARE / ALPHA_STEPS, shown, alpha * 100.0);
                    }
                    frameRect(alphaStrip, x, SQUARE_Y, HUE_WIDTH, SQUARE, ROW_LINE);
                }
            }
            Clip swatch;
            if (pickerPart("swatch", swatch))
            {
                swatch.invoke("clear", nullptr, 0);
                swatchRect(swatch, SWATCH_X, SQUARE_Y + SQUARE - SWATCH_HEIGHT, SWATCH_WIDTH, SWATCH_HEIGHT, rgb, p.alpha);
                frameRect(swatch, SWATCH_X, SQUARE_Y + SQUARE - SWATCH_HEIGHT, SWATCH_WIDTH, SWATCH_HEIGHT, ROW_LINE);
            }
            char value[32];
            const int percent = static_cast<int>(std::lround(p.alpha * 100.0));
            if (!p.custom)
                std::snprintf(value, sizeof(value), "DEFAULT  %d%%", percent);
            else if (hasAlpha)
                std::snprintf(value, sizeof(value), "#%06X  %d%%", rgb, percent);
            else
                std::snprintf(value, sizeof(value), "#%06X", rgb);
            setLabel("value", value);

            if (!p.hexEditing)
            {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "#%06X", rgb);
                setLabel("hex", hex);
            }

            Clip mark;
            if (pickerPart("squareMark", mark))
                moveClip(mark, SQUARE_X + p.s * SQUARE, SQUARE_Y + (1.0 - p.v) * SQUARE);
            Clip hueMark;
            if (pickerPart("hueMark", hueMark))
                moveClip(hueMark, HUE_X, SQUARE_Y + p.h * SQUARE);
            Clip alphaMark;
            if (pickerPart("alphaMark", alphaMark))
            {
                moveClip(alphaMark, alphaX(), SQUARE_Y + (1.0 - p.alpha) / (1.0 - OPACITY_LOW) * SQUARE);
                setVisible(alphaMark, hasAlpha);
            }
        }

        void showPicker(bool shown)
        {
            Clip picker;
            if (!picker.resolve(g_picker.movie, g_picker.path.c_str()))
                return;
            fb::GFxDisplayInfo info{ };
            info.m_visible = shown;
            info.m_varsSet = VARS_VISIBLE;
            picker.write(&info);
        }

        // under List_01's rows, which are only laid out once the page is set up
        void placePicker()
        {
            Picker& p = g_picker;
            Clip holder, picker;
            double b[4];
            if (!picker.resolve(p.movie, p.path.c_str()))
                return;
            // its last row starts ROW_BOUNDS_BOTTOM above the rows' bounds end
            const double rows = holder.resolve(p.movie, (p.mainListPath + ".m_ListHolder").c_str()) && holder.localBounds(b) && b[3] > b[1]
                ? std::round((b[3] - ROW_BOUNDS_BOTTOM) / ROW_PITCH) * ROW_PITCH + ROW_PITCH : 0.0;
            moveClip(picker, p.x, p.listY + rows + PANEL_GAP);
        }

        // the static parts: the header row, the body behind the square, the hue strip, the old color, the marks
        void buildPicker(Clip& picker)
        {
            const Picker& p = g_picker;
            constexpr const char* PARTS[9] = { "background", "square", "hue", "alpha", "original", "swatch", "squareMark", "hueMark", "alphaMark" };
            for (int i = 0; i < 9; ++i)
            {
                const fb::GFxValue part[2] = { text(PARTS[i]), number(i + 1) };
                picker.invoke("createEmptyMovieClip", part, 2);
            }

            Clip part;
            if (pickerPart("background", part))
            {
                rowRect(part, 0.0, 0.0, p.width, ROW_HEIGHT);
                rowRect(part, 0.0, BODY_Y, p.width, BODY_HEIGHT);
            }
            Clip hue;
            if (pickerPart("hue", hue))
            {
                for (int i = 0; i < HUE_SLICES; ++i)
                    fillRect(hue, HUE_X, SQUARE_Y + SQUARE * i / HUE_SLICES, HUE_WIDTH, SQUARE / HUE_SLICES + 0.5, hsvToRgb((i + 0.5) / HUE_SLICES, 1.0, 1.0), 100.0);
                frameRect(hue, HUE_X, SQUARE_Y, HUE_WIDTH, SQUARE, ROW_LINE);
            }
            Clip mark;
            if (pickerPart("squareMark", mark))
            {
                frameRect(mark, -4.0, -4.0, 8.0, 8.0, 0x000000);
                frameRect(mark, -3.0, -3.0, 6.0, 6.0, 0xFFFFFF);
            }
            for (const char* stripMark : { "hueMark", "alphaMark" })
            {
                Clip markClip;
                if (pickerPart(stripMark, markClip))
                {
                    frameRect(markClip, -2.0, -2.0, HUE_WIDTH + 4.0, 4.0, 0x000000);
                    frameRect(markClip, -1.0, -1.0, HUE_WIDTH + 2.0, 2.0, 0xFFFFFF);
                }
            }

            createLabel(picker, "title", 10, PANEL_PAD, 0.0, p.width / 2.0, ROW_HEIGHT, 1.0, false);
            createLabel(picker, "value", 11, p.width / 2.0, 0.0, p.width / 2.0 - PANEL_PAD, ROW_HEIGHT, 1.0, true);
            createLabel(picker, "before", 12, SWATCH_X, SQUARE_Y - 2.0, SWATCH_WIDTH, CAPTION_HEIGHT, 0.7, false);
            createLabel(picker, "after", 13, SWATCH_X, SQUARE_Y + SQUARE - SWATCH_HEIGHT - CAPTION_HEIGHT - 2.0, SWATCH_WIDTH, CAPTION_HEIGHT, 0.7, false);
            createLabel(picker, "note", 14, SQUARE_X + HUE_WIDTH + 2.0 * PANEL_PAD, SQUARE_Y + SQUARE / 2.0 - ROW_HEIGHT / 2.0, p.width / 2.0, ROW_HEIGHT, 1.0, false);

            // the hex box: the menu gives Flash input fields no typing, so its keys are read here (typeHex only)
            const fb::GFxValue hexBoxPart[2] = { text("hexBox"), number(15) };
            picker.invoke("createEmptyMovieClip", hexBoxPart, 2);
            Clip hexBox;
            if (pickerPart("hexBox", hexBox))
            {
                fillRect(hexBox, HEX_X, SQUARE_Y + CAPTION_HEIGHT, HEX_WIDTH, SWATCH_HEIGHT, HEX_BOX, 100.0);
                frameRect(hexBox, HEX_X, SQUARE_Y + CAPTION_HEIGHT, HEX_WIDTH, SWATCH_HEIGHT, ROW_LINE);
            }
            createLabel(picker, "hexCaption", 16, HEX_X, SQUARE_Y - 2.0, HEX_WIDTH, CAPTION_HEIGHT, 0.7, false);
            setLabel("hexCaption", "HEX");
            createLabel(picker, "hex", 17, HEX_X + 6.0, SQUARE_Y + CAPTION_HEIGHT + (SWATCH_HEIGHT - ROW_HEIGHT) / 2.0, HEX_WIDTH - 12.0, ROW_HEIGHT, 1.0, false);

            setLabel("before", "CURRENT");
            setLabel("after", "NEW");
            setLabel("note", "DEFAULT");
        }

        // CUSTOM shows the color parts, DEFAULT only the opacity strip and its note
        void showMode()
        {
            const bool custom = g_picker.custom;
            for (const char* part : { "square", "hue", "original", "swatch", "squareMark", "hueMark", "before", "after", "hexBox", "hexCaption", "hex" })
            {
                Clip clip;
                if (pickerPart(part, clip))
                    setVisible(clip, custom);
            }
            Clip note;
            if (pickerPart("note", note))
                setVisible(note, !custom);
        }

        // the old color's swatch, drawn once per opening
        void drawOriginal()
        {
            Clip part;
            if (pickerPart("original", part))
            {
                part.invoke("clear", nullptr, 0);
                swatchRect(part, SWATCH_X, SQUARE_Y + CAPTION_HEIGHT, SWATCH_WIDTH, SWATCH_HEIGHT, g_picker.original, g_picker.originalAlpha);
                frameRect(part, SWATCH_X, SQUARE_Y + CAPTION_HEIGHT, SWATCH_WIDTH, SWATCH_HEIGHT, ROW_LINE);
            }
        }

        // under List_01, as wide as its rows; built when first shown, once the list's rows exist to copy a font from
        void createPicker(fb::GFxMovieView* movie, Clip& holder, const std::string& parent, double x, double width, double listY)
        {
            double depth = 0.0;
            fb::GFxValue args[2] = { text(PICKER_NAME), number(0.0) };
            Clip picker;
            if (!holder.invoke("getNextHighestDepth", nullptr, 0, &depth) || (args[1].m_number = depth, !holder.invoke("createEmptyMovieClip", args, 2))
                || !picker.resolve(movie, (parent + "." + PICKER_NAME).c_str()))
            {
                log("{}: clip NOT created", PICKER_NAME);
                return;
            }
            g_picker = Picker{ movie, parent + "." + PICKER_NAME, parent + "." + HUD_LIST_NAMES[hudTab()], parent + ".List_01", x, width, listY };

            showPicker(false);
            log("{}: clip at x {:.0f}, {:.0f} wide", PICKER_NAME, x, width);
        }

        bool gameHasFocus()
        {
            DWORD process = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &process);
            return process == GetCurrentProcessId();
        }

        void endHex();

        // opened for slot, or closed with -1
        // a drag cut short (the picker closed or switched rows, the options left) is saved as if released: the profile has to
        // differ from its saved copy for the options to ask to save
        void endDrag()
        {
            Picker& p = g_picker;
            if (p.drag == Drag::Square || p.drag == Drag::Hue)
                pickColor(static_cast<ColorSlot>(p.slot), hsvToRgb(p.h, p.s, p.v), true);
            else if (p.drag == Drag::Alpha)
                saveSettings();
            p.drag = Drag::None;
        }

        void openPicker(int slot)
        {
            Picker& p = g_picker;
            if (p.slot >= 0)
                endDrag();
            if (p.hexEditing)
                endHex();
            Clip picker;
            if (!picker.resolve(p.movie, p.path.c_str()))
                return;

            if (slot >= 0 && p.slot < 0 && picker.children().empty())
                buildPicker(picker);
            p.slot = slot;
            if (slot >= 0)
            {
                placePicker();
                p.custom = settings().color[slot] == CUSTOM_COLOR;
                p.original = p.custom ? settings().customColor[slot] : 0xFFFFFF;
                p.element = COLOR_ELEMENTS[slot];
                p.alpha = p.originalAlpha = p.element != ElementCount ? settings().opacity[p.element] / 100.0 : 1.0;
                rgbToHsv(p.original, p.h, p.s, p.v);
                setLabel("title", colorRowName(slot));
                showMode();
                drawOriginal();
                drawPicker(true, true);
            }
            // DEFAULT without an opacity leaves nothing to set
            showPicker(slot >= 0 && (p.custom || p.element != ElementCount));
        }

        // a color row's swatch, just left of its value: its color, an empty frame on DEFAULT
        constexpr const char* ROW_SWATCH = "hudFixSwatch";
        constexpr double ROW_SWATCH_X = 268.0;
        constexpr double ROW_SWATCH_Y = 7.0;
        constexpr double ROW_SWATCH_WIDTH = 26.0;
        constexpr double ROW_SWATCH_HEIGHT = 14.0;
        constexpr double ROW_SWATCH_GAP = 8.0; // to the value's text
        std::unordered_map<std::string, int> g_rowSwatches; // row clip path -> what its swatch shows, -2 hidden


        void updateRowSwatches(fb::GFxMovieView* movie)
        {
            const Picker& p = g_picker;
            Clip holder;
            if (!holder.resolve(movie, (p.listPath + ".m_ListHolder").c_str()))
                return;

            for (const std::string& row : holder.children())
            {
                if (row.rfind("row_", 0) != 0)
                    continue;
                const std::string rowPath = p.listPath + ".m_ListHolder." + row;
                Clip label;
                const std::string name = label.resolve(movie, (rowPath + ".Label.txtField").c_str()) ? label.string("text") : "";

                int slot = -1;
                for (int i = 0; i < ColorSlotCount && slot < 0 && !name.empty(); ++i)
                {
                    if (colorRowShown(i) && name == colorRowName(i))
                        slot = i;
                }

                Clip rowClip, swatch;
                if (!rowClip.resolve(movie, rowPath.c_str()))
                    continue;
                const std::string swatchPath = rowPath + "." + ROW_SWATCH;
                bool has = swatch.resolve(movie, swatchPath.c_str());
                const int state = slot < 0 ? -2 : settings().color[slot] == CUSTOM_COLOR ? settings().customColor[slot] : -1;
                auto drawn = g_rowSwatches.find(rowPath);
                if (has && drawn != g_rowSwatches.end() && drawn->second == state)
                {
                    // drawn already...
                }
                else if (slot < 0)
                {
                    if (has)
                    {
                        swatch.invoke("clear", nullptr, 0);
                        g_rowSwatches[rowPath] = state;
                    }
                }
                else
                {
                    if (!has)
                    {
                        const fb::GFxValue args[2] = { text(ROW_SWATCH), number(1000.0) };
                        rowClip.invoke("createEmptyMovieClip", args, 2);
                        has = swatch.resolve(movie, swatchPath.c_str());
                    }
                    if (has)
                    {
                        swatch.invoke("clear", nullptr, 0);
                        if (state >= 0)
                            fillRect(swatch, 0.0, 0.0, ROW_SWATCH_WIDTH, ROW_SWATCH_HEIGHT, state, 100.0);
                        frameRect(swatch, 0.0, 0.0, ROW_SWATCH_WIDTH, ROW_SWATCH_HEIGHT, state >= 0 ? ROW_LINE : 0xA9BDCC);
                        g_rowSwatches[rowPath] = state;
                    }
                }

                if (slot < 0 || !has)
                    continue;

                // left of the value's text, which the selector centers in its label field
                double swatchX = ROW_SWATCH_X;
                Clip holderClip, value;
                fb::GFxDisplayInfo holderInfo{ }, valueInfo{ };
                if (holderClip.resolve(movie, (rowPath + ".stHolder").c_str()) && holderClip.read(&holderInfo)
                    && value.resolve(movie, (rowPath + ".stHolder.selectorLabel").c_str()) && value.read(&valueInfo))
                {
                    const double fieldWidth = value.number("_width", 0.0), textWidth = value.number("textWidth", 0.0);
                    if (fieldWidth > 0.0 && textWidth > 0.0)
                        swatchX = holderInfo.m_x + valueInfo.m_x + (fieldWidth - textWidth) / 2.0 - ROW_SWATCH_GAP - ROW_SWATCH_WIDTH;
                }
                moveClip(swatch, swatchX, ROW_SWATCH_Y);
            }
        }

        // the hex box's frame, lit while it is typed into
        void drawHexBox()
        {
            Clip box;
            if (!pickerPart("hexBox", box))
                return;
            box.invoke("clear", nullptr, 0);
            fillRect(box, HEX_X, SQUARE_Y + CAPTION_HEIGHT, HEX_WIDTH, SWATCH_HEIGHT, HEX_BOX, 100.0);
            frameRect(box, HEX_X, SQUARE_Y + CAPTION_HEIGHT, HEX_WIDTH, SWATCH_HEIGHT, g_picker.hexEditing ? 0xA9BDCC : ROW_LINE);
        }

        void startHex()
        {
            Picker& p = g_picker;
            p.hexEditing = true;
            p.hexTyped.clear();
            for (int key = 0; key < 256; ++key)
                p.keyWas[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
            holdKeyboard();
            drawHexBox();
        }

        // the keyboard is kept a moment more, so the Enter or Escape that ended it does not reach the menu
        void endHex()
        {
            Picker& p = g_picker;
            p.hexEditing = false;
            holdKeyboard();
            drawHexBox();
            drawPicker(false, false);
        }

        std::string clipboardHex()
        {
            std::string digits;
            if (!OpenClipboard(nullptr))
                return digits;

            if (HANDLE data = GetClipboardData(CF_TEXT))
            {
                if (const char* chars = static_cast<const char*>(GlobalLock(data)))
                {
                    for (const char* c = chars; *c && digits.size() < 6; ++c)
                    {
                        if (std::isxdigit(static_cast<unsigned char>(*c)))
                            digits += static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
                        else if (*c != '#' && !digits.empty())
                            break;
                    }
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
            return digits;
        }

		// we allow strict rules only: 0-9, A-F, Backspace, Ctrl+V, Enter or Escape to end
        void typeHex()
        {
            Picker& p = g_picker;
            holdKeyboard();
            const auto press = [&p](int key)
            {
                const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
                const bool pressed = down && !p.keyWas[key];
                p.keyWas[key] = down;
                return pressed;
            };
            const auto add = [&p](char c)
            {
                if (p.hexTyped.size() < 6)
                    p.hexTyped += c;
            };

            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            for (int key = '0'; key <= '9'; ++key)
            {
                if (press(key))
                    add(static_cast<char>(key));
            }
            for (int key = VK_NUMPAD0; key <= VK_NUMPAD9; ++key)
            {
                if (press(key))
                    add(static_cast<char>('0' + key - VK_NUMPAD0));
            }
            for (int key = 'A'; key <= 'F'; ++key)
            {
                if (press(key) && !ctrl)
                    add(static_cast<char>(key));
            }
            if (press('V') && ctrl)
                p.hexTyped = clipboardHex();
            if (press(VK_BACK) && !p.hexTyped.empty())
                p.hexTyped.pop_back();
            if (press(VK_RETURN) || press(VK_ESCAPE))
            {
                endHex();
                return;
            }

            if (p.hexTyped.size() == 6)
            {
                const int rgb = std::stoi(p.hexTyped, nullptr, 16);
                if (rgb != hsvToRgb(p.h, p.s, p.v))
                {
                    rgbToHsv(rgb, p.h, p.s, p.v);
                    drawPicker(true, true);
                    pickColor(static_cast<ColorSlot>(p.slot), rgb, true);
                }
            }
            const bool caret = GetTickCount64() / 500 % 2 == 0;
            setLabel("hex", ("#" + p.hexTyped + (caret && p.hexTyped.size() < 6 ? "_" : "")).c_str());
        }

        void updatePicker(fb::GFxMovieView* movie)
        {
            Picker& p = g_picker;
            if (movie != p.movie || p.path.empty())
                return;

            Clip list, picker;
            if (!list.resolve(movie, p.listPath.c_str()) || !picker.resolve(movie, p.path.c_str()))
            {
                if (p.slot >= 0)
                    endDrag();
                return;
            }

            updateRowSwatches(movie);
            const int row = static_cast<int>(list.number("m_highlightedIndex", -1.0));
            if (row >= 0 && p.drag == Drag::None)
            {
                const int slot = highlightedColorSlot(row);
                const ULONGLONG now = GetTickCount64();
                if (slot >= 0)
                {
                    p.otherRowSince = 0;
                    if (slot != p.slot)
                        openPicker(slot);
                }
                else if (p.slot >= 0)
                {
                    if (!p.otherRowSince)
                        p.otherRowSince = now;
                    else if (now - p.otherRowSince >= CLOSE_DELAY)
                        openPicker(-1);
                }
            }
            if (p.slot >= 0 && !colorRowShown(p.slot))
                openPicker(-1);
            else if (p.slot >= 0 && p.drag == Drag::None && (settings().color[p.slot] == CUSTOM_COLOR) != p.custom)
                openPicker(p.slot);
            if (p.slot < 0)
            {
                p.wasDown = false;
                return;
            }

            const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && gameHasFocus();
            const bool pressed = down && !p.wasDown;
            p.wasDown = down;

            const double x = picker.number("_xmouse", -1.0), y = picker.number("_ymouse", -1.0);
            const bool inHex = p.custom && x >= HEX_X && x <= HEX_X + HEX_WIDTH && y >= SQUARE_Y + CAPTION_HEIGHT && y <= SQUARE_Y + CAPTION_HEIGHT + SWATCH_HEIGHT;
            if (pressed && inHex && !p.hexEditing)
                startHex();
            else if (pressed && !inHex && p.hexEditing)
                endHex();
            if (p.hexEditing)
                typeHex();

            if (pressed && !inHex)
            {
                const bool inRows = y >= SQUARE_Y && y <= SQUARE_Y + SQUARE;
                if (inRows && p.element != ElementCount && x >= alphaX() && x <= alphaX() + HUE_WIDTH)
                    p.drag = Drag::Alpha;
                else if (inRows && p.custom && x >= SQUARE_X && x <= SQUARE_X + SQUARE)
                    p.drag = Drag::Square;
                else if (inRows && p.custom && x >= HUE_X && x <= HUE_X + HUE_WIDTH)
                    p.drag = Drag::Hue;
            }

            const ColorSlot slot = static_cast<ColorSlot>(p.slot);
            if (!down)
            {
                if (p.drag == Drag::Alpha)
                    saveSettings();
                else if (p.drag != Drag::None)
                    pickColor(slot, hsvToRgb(p.h, p.s, p.v), true);
                p.drag = Drag::None;
                return;
            }
            if (p.drag == Drag::None)
                return;

            const auto unit = [](double value) { return std::clamp(value / SQUARE, 0.0, 1.0); };
            if (p.drag == Drag::Alpha)
            {
                // whole percents, as the options keep them
                p.alpha = std::round((1.0 - unit(y - SQUARE_Y) * (1.0 - OPACITY_LOW)) * 100.0) / 100.0;
                settings().opacity[p.element] = static_cast<int>(std::lround(p.alpha * 100.0));
                drawPicker(false, false);
                return;
            }
            if (p.drag == Drag::Square)
            {
                p.s = unit(x - SQUARE_X);
                p.v = 1.0 - unit(y - SQUARE_Y);
            }
            else
            {
                p.h = (std::min)(unit(y - SQUARE_Y), 0.9999);
            }
            drawPicker(p.drag == Drag::Hue, true);
            pickColor(slot, hsvToRgb(p.h, p.s, p.v), false);
        }

        int g_shownHudList = -1; // the HUD list shown, -1 before the lists exist
        // ghetto to prevent artifacts
        constexpr int HUD_LIST_HOLD_FRAMES = 120;
        int g_hudListHold = 0;

        void createHudListClip(fb::GFxMovieView* movie, fb::UIScreenData* screen)
        {
            constexpr double kGap = 18.0;
            const std::string parent = std::string(screen->m_instanceName) + ".instance1";
            Clip holder, main;
            fb::GFxDisplayInfo mainInfo{ };
            double mainBounds[4];
            bool made = holder.resolve(movie, parent.c_str()) && main.resolve(movie, (parent + ".List_01").c_str()) && main.read(&mainInfo)
                && main.localBounds(mainBounds);
            // List_01's mask spans x -5..width+5 and y 0..height
            const double left = mainInfo.m_x + (mainBounds[2] - 5.0) + kGap;
            const double width = -mainInfo.m_x - left;
            const double tabs = made ? createHudTabsClip(movie, screen, holder, parent, left, mainInfo.m_y, width) : 0.0;
            const double height = mainBounds[3] - tabs;
            for (int tab = 0; made && tab < HUD_LIST_COUNT; ++tab)
            {
                Clip created;
                fb::GFxDisplayInfo info{ };
                double depth = 0.0, ownBounds[4];
                fb::GFxValue args[2]{ };
                args[0].m_type = STRING;
                args[0].m_data = const_cast<char*>(HUD_LIST_NAMES[tab]);
                args[1].m_type = NUMBER;
                made = holder.invoke("getNextHighestDepth", nullptr, 0, &depth) && (args[1].m_number = depth, main.invoke("duplicateMovieClip", args, 2))
                    && created.resolve(movie, (parent + "." + HUD_LIST_NAMES[tab]).c_str()) && created.read(&info) && created.localBounds(ownBounds);
                if (!made)
                    break;

                info.m_x = left;
                info.m_y = mainInfo.m_y + tabs;
                info.m_xScale = width / (ownBounds[2] - ownBounds[0]) * 100.0;
                info.m_yScale = height / (ownBounds[3] - ownBounds[1]) * 100.0;
                info.m_visible = tab == hudTab();
                info.m_varsSet = 0x1 | 0x2 | 0x8 | 0x10 | VARS_VISIBLE;
                created.write(&info);
                made = created.invoke("onLoad", nullptr, 0);
                fb::GFxValue none{ };
                none.m_type = 1U;
                created.set("onLoad", none);
                log("{}: clip on {} at ({:.0f}, {:.0f}) {:.0f} x {:.0f}, depth {:.0f}", HUD_LIST_NAMES[tab], screen->m_instanceName, left, info.m_y, width, height, depth);
            }
            if (made)
            {
                g_shownHudList = hudTab();
                g_hudListHold = HUD_LIST_HOLD_FRAMES;
                createPicker(movie, holder, parent, mainInfo.m_x, mainBounds[2] - 5.0, mainInfo.m_y);
                return;
            }

            log("HUD lists: clips NOT created on {}", screen->m_instanceName);
            for (const char* name : HUD_LIST_NAMES)
                removeWidget(screen, name);
            removeWidget(screen, HUD_TABS_NAME);
        }

        // the shown tab's list visible, the others hidden with their rows kept
        void showHudList(fb::GFxMovieView* movie)
        {
            Picker& p = g_picker;
            const int tab = hudTab();
            if (movie != p.movie || p.path.empty())
                return;
            if (g_shownHudList != tab)
                g_hudListHold = HUD_LIST_HOLD_FRAMES;
            if (!g_hudListHold)
                return;

            --g_hudListHold;
            const std::string parent = p.path.substr(0, p.path.rfind('.'));
            for (int i = 0; i < HUD_LIST_COUNT; ++i)
            {
                Clip list;
                fb::GFxDisplayInfo info{ };
                if (list.resolve(movie, (parent + "." + HUD_LIST_NAMES[i]).c_str()) && list.read(&info) && info.m_visible != (i == tab))
                    setVisible(list, i == tab);
            }
            p.listPath = parent + "." + HUD_LIST_NAMES[tab];
            g_shownHudList = tab;
        }

        struct WidgetContext
        {
            fb::GFxMovieView* movie;
            const fb::UIScreenData* screen;
            const fb::WidgetNode* widget;
            std::string path;
            double k, layout;
            double alpha;
            const Color* color;
            Color tint;
            bool recolored;
            bool replaceColor; // drawn in one color
            bool ownCenter; // scaled around its own origin
            bool minimapEdge; // the minimap or its compass (g_minimapOutline)
            bool followsMinimap; // placed by the minimap's size, its own differs (tickets, squad list, chat)
            bool minimapBackground; // the panel behind the minimap and the tickets: stretched over both (g_backgroundBounds)
            double ax, ay; // the aligned screen corner in the widget's parent space
        };

        // the vehicle HUD's art is this yellow-green (the cockpit HUD shader's gains have its shape), with a near-white core
        // was dumped from shaders
        constexpr float HUD_ART[3] = { 0.64f, 1.0f, 0.235f };

        void cross(const float (&a)[3], const float (&b)[3], float (&out)[3])
        {
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
        }

        // we have to do that to not lose quality, brightness etc... basically keep original color feeling
        void recolorMatrix(const Color& color, const float (&stock)[3], float (&m)[3][3])
        {
            const float target[3] = { color.r, color.g, color.b };
            const float grey[3] = { 1.0f, 1.0f, 1.0f };
            const float stockMean = (stock[0] + stock[1] + stock[2]) / 3.0f;
            const float targetMean = (target[0] + target[1] + target[2]) / 3.0f;
            const float stockChroma[3] = { stock[0] - stockMean, stock[1] - stockMean, stock[2] - stockMean };
            const float targetChroma[3] = { target[0] - targetMean, target[1] - targetMean, target[2] - targetMean };
            float from[3][3], to[3][3], fromSide[3], toSide[3];
            cross(stockChroma, grey, fromSide);
            cross(targetChroma, grey, toSide);
            for (int i = 0; i < 3; ++i)
            {
                from[i][0] = stock[i];
                from[i][1] = 1.0f;
                from[i][2] = fromSide[i];
                to[i][0] = target[i];
                to[i][1] = 1.0f;
                to[i][2] = toSide[i];
            }

            // m = to * inverse(from); a grey stock color has no chroma to turn, everything goes to the color by its brightness
            const float det = from[0][0] * (from[1][1] * from[2][2] - from[1][2] * from[2][1])
                - from[0][1] * (from[1][0] * from[2][2] - from[1][2] * from[2][0])
                + from[0][2] * (from[1][0] * from[2][1] - from[1][1] * from[2][0]);
            if (std::fabs(det) < 1e-6f)
            {
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        m[i][j] = stockMean > 0.0f ? target[i] / (3.0f * stockMean) : 0.0f;
                return;
            }
            float inverse[3][3];
            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                {
                    const int r0 = (j + 1) % 3, r1 = (j + 2) % 3, c0 = (i + 1) % 3, c1 = (i + 2) % 3;
                    inverse[i][j] = (from[r0][c0] * from[r1][c1] - from[r0][c1] * from[r1][c0]) / det;
                }
            }
            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                {
                    m[i][j] = 0.0f;
                    for (int n = 0; n < 3; ++n)
                        m[i][j] += to[i][n] * inverse[n][j];
                }
            }
        }

        void addColor(std::vector<unsigned int>& colors, unsigned int raw)
        {
            const unsigned int rgb = raw & 0xFFFFFF;
            if ((raw >> 24) && std::find(colors.begin(), colors.end(), rgb) == colors.end())
                colors.push_back(rgb);
        }

        // the solid colors of a SWF shape
        void shapeColors(const unsigned char* def, std::vector<unsigned int>& colors)
        {
            {
                const unsigned char* fills = *reinterpret_cast<const unsigned char* const*>(def + SHAPE_FILL_STYLES);
                const unsigned int fillCount = *reinterpret_cast<const unsigned int*>(def + SHAPE_FILL_COUNT);
                for (unsigned int f = 0; fills && f < fillCount && f < MAX_STYLES; ++f)
                {
                    if (fills[f * FILL_STYLE_SIZE] == 0)
                        addColor(colors, *reinterpret_cast<const unsigned int*>(fills + f * FILL_STYLE_SIZE + FILL_STYLE_COLOR));
                }
                const unsigned char* lines = *reinterpret_cast<const unsigned char* const*>(def + SHAPE_LINE_STYLES);
                const unsigned int lineCount = *reinterpret_cast<const unsigned int*>(def + SHAPE_LINE_COUNT);
                for (unsigned int l = 0; lines && l < lineCount && l < MAX_STYLES; ++l)
                    addColor(colors, *reinterpret_cast<const unsigned int*>(lines + l * LINE_STYLE_SIZE + LINE_STYLE_COLOR));
            }
        }

        bool replacesColor(const fb::GFxCxform& cxform)
        {
            return std::fabs(cxform.m_rows[0][0]) < 1e-3f && std::fabs(cxform.m_rows[1][0]) < 1e-3f && std::fabs(cxform.m_rows[2][0]) < 1e-3f;
        }

        // what decides the colors below a sprite (a sprite being a child of the sprite's own class): a sprite whose cxform
        // replaces its children's colors (not entered, they don't show), a SWF shape with its definition, anything else (text
        // fields) with none
        template <typename Visit>
        void forEachColorNode(unsigned char* sprite, int levels, Visit&& visit)
        {
            const void* spriteVtable = *reinterpret_cast<void* const*>(sprite);
            unsigned char* const* children = *reinterpret_cast<unsigned char* const* const*>(sprite + SPRITE_CHILDREN);
            const unsigned int count = *reinterpret_cast<const unsigned int*>(sprite + SPRITE_CHILD_COUNT);
            for (unsigned int i = 0; children && i < count && i < MAX_CHILDREN * 4; ++i)
            {
                unsigned char* child = children[i];
                if (!child)
                    continue;
                if (*reinterpret_cast<void* const*>(child) == spriteVtable)
                {
                    if (replacesColor(*reinterpret_cast<const fb::GFxCxform*>(child + CHARACTER_CXFORM)))
                        visit(child, nullptr, false);
                    else if (levels > 1)
                        forEachColorNode(child, levels - 1, visit);
                    continue;
                }
                const unsigned char* def = nullptr;
                if (reinterpret_cast<uintptr_t>(*reinterpret_cast<void* const*>(child)) == OFF_vt_GFxGenericCharacter)
                {
                    def = *reinterpret_cast<const unsigned char* const*>(child + CHARACTER_DEF);
                    if (!def || reinterpret_cast<uintptr_t>(*reinterpret_cast<void* const*>(def)) != OFF_vt_GFxConstShapeCharacterDef)
                        continue;
                }
                visit(child, def, !def);
            }
        }

        void unpack(unsigned int raw, float (&p)[3])
        {
            p[0] = ((raw >> 16) & 0xFF) / 255.0f;
            p[1] = ((raw >> 8) & 0xFF) / 255.0f;
            p[2] = (raw & 0xFF) / 255.0f;
        }

        // the cxform taking each art color to where recolorMatrix takes it, times brightness: per channel a line through them,
        // exact for a shape's one color. Without solid art (text, bitmaps) the stock color measured on screen stands in
        Recolor fitRecolor(std::vector<unsigned int> colors, const Color& color, const float (&stock)[3], float brightness)
        {
            if (colors.empty())
                colors = { 0xA3FF3C };
            float m[3][3];
            recolorMatrix(color, stock, m);

            Recolor r{ };
            const float n = static_cast<float>(colors.size());
            for (int c = 0; c < 3; ++c)
            {
                float sx = 0.0f, sy = 0.0f, sxx = 0.0f, sxy = 0.0f;
                for (unsigned int raw : colors)
                {
                    float p[3];
                    unpack(raw, p);
                    const float y = (m[c][0] * p[0] + m[c][1] * p[1] + m[c][2] * p[2]) * brightness;
                    sx += p[c];
                    sy += y;
                    sxx += p[c] * p[c];
                    sxy += p[c] * y;
                }
                const float spread = n * sxx - sx * sx;
                if (spread > 1e-4f)
                {
                    r.mult[c] = (n * sxy - sx * sy) / spread;
                    r.add[c] = (sy - r.mult[c] * sx) / n;
                }
                else
                {
                    r.mult[c] = sx > 1e-4f ? sy / sx : 1.0f;
                    r.add[c] = sx > 1e-4f ? 0.0f : sy / n;
                }
            }
            return r;
        }

        // every additive sprite from this one down drawn normal; how many
        int drawNormal(unsigned char* sprite, int levels)
        {
            if (reinterpret_cast<uintptr_t>(*reinterpret_cast<void* const*>(sprite)) != OFF_vt_GFxSprite)
                return 0;
            int count = 0;
            unsigned char* blend = sprite + SPRITE_BLEND_MODE;
            if (*blend == BLEND_ADD && g_blendSaveCount < MAX_SHAPES)
            {
                g_blendSaves[g_blendSaveCount++] = { blend, *blend };
                *blend = BLEND_NORMAL;
                ++count;
            }
            unsigned char* const* children = *reinterpret_cast<unsigned char* const* const*>(sprite + SPRITE_CHILDREN);
            const unsigned int childCount = *reinterpret_cast<const unsigned int*>(sprite + SPRITE_CHILD_COUNT);
            for (unsigned int i = 0; levels > 1 && children && i < childCount && i < MAX_CHILDREN * 4; ++i)
            {
                if (children[i])
                    count += drawNormal(children[i], levels - 1);
            }
            return count;
        }

        // what a shape shows, from its art colors: through its own cxform, which may replace them
        void shownColors(const fb::GFxCxform& cxform, std::vector<unsigned int>& colors)
        {
            if (colors.empty() || replacesColor(cxform))
                colors = { 0xA3FF3C };
            for (unsigned int& raw : colors)
            {
                float p[3];
                unpack(raw, p);
                unsigned int shown = 0;
                for (int i = 0; i < 3; ++i)
                {
                    const float c = std::clamp(p[i] * cxform.m_rows[i][0] + cxform.m_rows[i][1] / 255.0f, 0.0f, 1.0f);
                    shown = shown << 8 | static_cast<unsigned int>(c * 255.0f + 0.5f);
                }
                raw = shown;
            }
            std::sort(colors.begin(), colors.end());
            colors.erase(std::unique(colors.begin(), colors.end()), colors.end());
        }

        // the widget's art recolored shape by shape: each shape's cxform followed by one taking what it shows to the color
        void recolorClip(Clip& clip, const Color& color, bool throughShader)
        {
            unsigned char* character = clip.character();
            if (character)
                drawNormal(character, SHAPE_LEVELS);
            const float brightness = throughShader ? 1.0f : settings().vehicleBrightness / 100.0f;

            // every node deciding a color, with the colors it shows, and the most saturated of them: the stock color the hue is
            // turned from. Text shows that color too (the HUD's own text is drawn in it)
            struct ArtShape
            {
                fb::GFxCxform* cxform;
                std::vector<unsigned int> colors;
                bool text;
            };
            std::vector<ArtShape> shapes;
            float stock[3] = { HUD_ART[0], HUD_ART[1], HUD_ART[2] };
            float stockSaturation = -1.0f;
            if (character)
            {
                forEachColorNode(character, SHAPE_LEVELS, [&](unsigned char* node, const unsigned char* def, bool text)
                {
                    fb::GFxCxform* cxform = reinterpret_cast<fb::GFxCxform*>(node + CHARACTER_CXFORM);
                    ArtShape art{ cxform, { }, text };
                    if (text)
                    {
                        shapes.push_back(std::move(art));
                        return;
                    }
                    if (def)
                        shapeColors(def, art.colors);
                    shownColors(*cxform, art.colors);
                    for (unsigned int raw : art.colors)
                    {
                        float p[3], h, s, v;
                        unpack(raw, p);
                        hsv(p, h, s, v);
                        if (s > stockSaturation && v > 0.0f)
                        {
                            stockSaturation = s;
                            std::copy(std::begin(p), std::end(p), std::begin(stock));
                        }
                    }
                    shapes.push_back(std::move(art));
                });
            }

            // each shape's own cxform followed by the recolor of what it shows, for this draw only (restoreShapes)
            for (ArtShape& art : shapes)
            {
                if (g_shapeCxformCount >= MAX_SHAPE_CXFORMS)
                {
                    static bool warned = false;
                    if (!warned)
                        log("vehicle recolor: more than {} shapes in a frame, the rest keep their colors", MAX_SHAPE_CXFORMS);
                    warned = true;
                    break;
                }
                ShapeCxform& saved = g_shapeCxforms[g_shapeCxformCount++];
                saved.cxform = art.cxform;
                saved.saved = *art.cxform;
                if (art.text)
                {
                    unsigned int shown = 0;
                    for (float c : stock)
                        shown = shown << 8 | static_cast<unsigned int>(c * 255.0f + 0.5f);
                    art.colors = { shown };
                }
                // each node turned from its own main color (the most saturated it shows), so every one lands on the color:
                // a widget mixes art of different stock colors (orange lines, yellow-green bitmaps, green markers)
                float own[3] = { stock[0], stock[1], stock[2] };
                float ownSaturation = -1.0f;
                for (unsigned int raw : art.colors)
                {
                    float p[3], h, sat, v;
                    unpack(raw, p);
                    hsv(p, h, sat, v);
                    if (sat > ownSaturation && v > 0.0f)
                    {
                        ownSaturation = sat;
                        std::copy(std::begin(p), std::end(p), std::begin(own));
                    }
                }
                const Recolor r = fitRecolor(art.colors, color, own, brightness);
                for (int i = 0; i < 3; ++i)
                {
                    art.cxform->m_rows[i][1] = art.cxform->m_rows[i][1] * r.mult[i] + r.add[i] * 255.0f;
                    art.cxform->m_rows[i][0] *= r.mult[i];
                }
            }
            if (shapes.empty())
                clip.recolor(fitRecolor({}, color, stock, brightness));
        }

        const Color* widgetColor(Element element)
        {
            switch (element)
            {
            case Crosshair:
                return slotColor(CrosshairColor);
            case VehicleCrosshair:
                return slotColor(VehicleCrosshairColor);
            case VehicleHud:
                return slotColor(VehicleHudColor);
            case ChaseCrosshair:
                return slotColor(ChaseCrosshairColor);
            case HitMarkers:
                return slotColor(HitColor);
            case HeadshotMarkers:
                return slotColor(HeadshotColor);
            case KillMarkers:
                return slotColor(KillColor);
            default:
                return nullptr;
            }
        }

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
                    if (!widgetGroup(widget->m_WidgetAsset->m_Name, group, widget->m_InstanceName))
                        continue;

                    if (group.element == HitMarkers) // the hit indicator looks like what the last hit did
                        group.element = group.layout = g_hitMarker;
                    // chase cam screens hold the vehicle crosshair alone; the 1st person one sits in the vehicle HUD
                    if (group.element == VehicleCrosshair && screen->m_widgetsEnd - screen->m_widgetsBegin == 1)
                        group.element = group.layout = ChaseCrosshair;
                    const bool cockpit = group.element == VehicleCrosshair || group.element == VehicleHud;

                    WidgetContext context{ movie, screen, widget };
                    // the chase crosshair's size thickens its lines instead (thickenChaseCrosshair)
                    context.k = group.element == ChaseCrosshair ? 1.0 : elementFactor(group.element);
                    context.layout = group.element == ChaseCrosshair ? 1.0 : elementFactor(group.layout);
                    context.alpha = elementOpacity(group.element);
                    context.color = widgetColor(group.element);
                    if (!context.color && group.element == VehicleCrosshair) // the HUD's color when it has none of its own
                        context.color = widgetColor(VehicleHud);
                    if (context.color)
                        context.tint = *context.color;
                    context.recolored = cockpit;
                    // the crosshair multiplies: replacing also paints its darker edge pixels, which made it look blurry
                    context.replaceColor = group.element == ChaseCrosshair;
                    // centered on its own origin wherever its screen puts it, so it grows in place
                    context.ownCenter = cockpit || group.element == ChaseCrosshair;
                    if (context.ownCenter)
                        context.layout = 1.0;
                    context.minimapEdge = !_stricmp(widget->m_WidgetAsset->m_Name, "UI/Assets/Minimap") || !_stricmp(widget->m_WidgetAsset->m_Name, "UI/Assets/Compass");
                    context.followsMinimap = group.layout == Minimap && group.element != Minimap;
                    context.minimapBackground = !_stricmp(widget->m_WidgetAsset->m_Name, "UI/Assets/HudBackgroundWidget");
                    // without a ticket panel the panel is cut at the minimap's outline at any size
                    const bool ticketPanel = settings().removeTicketPanel && (context.minimapEdge || context.minimapBackground);
                    if (context.k == 1.0 && context.layout == 1.0 && context.alpha == 1.0 && !context.color && !ticketPanel)
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

        // the named scripted clips of a sprite, by its display list
        int childNames(const unsigned char* sprite, const char* (&names)[MAX_CHILDREN])
        {
            int found = 0;
            const unsigned char* object = sprite ? sprite + CHARACTER_OBJECT : nullptr;
            if (!object || reinterpret_cast<ObjectTypeFn>((*reinterpret_cast<void* const* const*>(object))[2])(object) != OBJECT_SPRITE)
                return 0;

            unsigned char* const* children = *reinterpret_cast<unsigned char* const* const*>(sprite + SPRITE_CHILDREN);
            const unsigned int count = *reinterpret_cast<const unsigned int*>(sprite + SPRITE_CHILD_COUNT);
            for (unsigned int i = 0; children && i < count && found < MAX_CHILDREN; ++i)
            {
                unsigned char* child = children[i];
                if (!child || !(child[CHARACTER_FLAGS_BYTE] & CHARACTER_SCRIPTED))
                    continue;
                unsigned char* handle = *reinterpret_cast<unsigned char**>(child + CHARACTER_HANDLE);
                if (!handle)
                    handle = reinterpret_cast<GetHandleFn>(OFF_GFxCharacter_getHandle)(child);
                const unsigned char* node = handle ? *reinterpret_cast<unsigned char* const*>(handle + HANDLE_NAME) : nullptr;
                const char* name = node ? *reinterpret_cast<const char* const*>(node) : nullptr;
                if (name && *name)
                    names[found++] = name;
            }
            return found;
        }

        // the clips below path, levels deep
        bool findChildren(fb::GFxMovieView* movie, const std::string& path, int levels, std::vector<Child>& out)
        {
            Clip clip;
            if (!clip.resolve(movie, path.c_str()))
                return false;

            const char* names[MAX_CHILDREN];
            const int count = childNames(clip.character(), names);
            for (int i = 0; i < count; ++i)
            {
                Child child{ path + "." + names[i] };
                Clip found;
                fb::GFxDisplayInfo info{ };
                if (!found.resolve(movie, child.path.c_str()) || !found.read(&info))
                    continue;
                child.restX = info.m_x;
                child.restY = info.m_y;
                out.push_back(child);
                if (levels > 1)
                    findChildren(movie, child.path, levels - 1, out);
            }
            return true;
        }

        // the clips of a vehicle widget ActionScript has moved since they were found
        void trackChildren(fb::GFxMovieView* movie, std::vector<Child>& children)
        {
            for (Child& child : children)
            {
                Clip clip;
                fb::GFxDisplayInfo info{ };
                if (!child.moving && clip.resolve(movie, child.path.c_str()) && clip.read(&info))
                    child.moving = std::abs(info.m_x - child.restX) > MOVED || std::abs(info.m_y - child.restY) > MOVED;
            }
        }

        // the clips found now, keeping what is known of those found before
        bool rescanChildren(fb::GFxMovieView* movie, const std::string& path, std::vector<Child>& clips)
        {
            std::vector<Child> found;
            if (!findChildren(movie, path, CHILD_LEVELS, found))
                return false;
            for (Child& child : found)
            {
                const auto known = std::find_if(clips.begin(), clips.end(), [&child](const Child& k) { return k.path == child.path; });
                if (known != clips.end())
                    child = std::move(*known);
            }
            clips = std::move(found);
            return true;
        }

        // a character's 2x3 matrix (GMatrix2D, translation in twips): x' = m0 x + m1 y + m2, y' = m3 x + m4 y + m5
        struct Affine
        {
            float m[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        };

        Affine then(const Affine& parent, const float* child)
        {
            const float* p = parent.m;
            return { { p[0] * child[0] + p[1] * child[3], p[0] * child[1] + p[1] * child[4], p[0] * child[2] + p[1] * child[5] + p[2],
                p[3] * child[0] + p[4] * child[3], p[3] * child[1] + p[4] * child[4], p[3] * child[2] + p[4] * child[5] + p[5] } };
        }

        struct Outline
        {
            std::unordered_map<const unsigned char*, OutlinePart>& parts;
            double bounds[4];
            bool any = false;
        };

        // a drawn part's bounds through at, in the widget's space (pixels), into the outline while they hold still
        void addOutlinePart(unsigned char* character, const Affine& at, Outline& outline)
        {
            float rect[4] = { };
            using GetBoundsFn = float* (__thiscall*)(unsigned char* character, float* out, const float* matrix);
            reinterpret_cast<GetBoundsFn>((*reinterpret_cast<void* const* const*>(character))[GET_BOUNDS_SLOT])(character, rect, at.m);
            if (!(rect[2] > rect[0]) || !(rect[3] > rect[1]) || std::abs(rect[0]) > 1e7f || std::abs(rect[1]) > 1e7f) // empty
                return;
            const std::array<double, 4> b = { rect[0] / TWIPS, rect[1] / TWIPS, rect[2] / TWIPS, rect[3] / TWIPS };
            const auto known = g_outlineParts.find(character);
            OutlinePart part = known != g_outlineParts.end() ? known->second : OutlinePart{ b };
            for (int i = 0; i < 4; ++i)
                part.varied |= std::abs(b[i] - part.bounds[i]) > MOVED;
            outline.parts[character] = part;
            if (part.varied)
                return;
            if (!outline.any)
                std::copy(b.begin(), b.end(), outline.bounds);
            outline.bounds[0] = (std::min)(outline.bounds[0], b[0]);
            outline.bounds[1] = (std::min)(outline.bounds[1], b[1]);
            outline.bounds[2] = (std::max)(outline.bounds[2], b[2]);
            outline.bounds[3] = (std::max)(outline.bounds[3], b[3]);
            outline.any = true;
        }

        // what the sprite draws: a mask stands for what it masks (the radar's map is far bigger than the circle showing it),
        // hidden sprites draw nothing
        void walkOutline(unsigned char* sprite, const Affine& at, int levels, Outline& outline)
        {
            unsigned char* const* children = *reinterpret_cast<unsigned char* const* const*>(sprite + SPRITE_CHILDREN);
            const unsigned int count = *reinterpret_cast<const unsigned int*>(sprite + SPRITE_CHILD_COUNT);
            int maskedTo = INT_MIN; // the children up to this depth are under a timeline mask
            for (unsigned int i = 0; children && i < count && i < MAX_CHILDREN * 4; ++i)
            {
                unsigned char* child = children[i];
                if (!child)
                    continue;
                if (*reinterpret_cast<const int*>(child + CHARACTER_DEPTH) <= maskedTo)
                    continue;
                const Affine placed = then(at, reinterpret_cast<const float*>(child + CHARACTER_MATRIX));
                if (const unsigned short clipDepth = *reinterpret_cast<const unsigned short*>(child + CHARACTER_CLIP_DEPTH))
                {
                    maskedTo = (std::max)(maskedTo, static_cast<int>(clipDepth));
                    addOutlinePart(child, placed, outline);
                    continue;
                }
                if (reinterpret_cast<uintptr_t>(*reinterpret_cast<void* const*>(child)) != OFF_vt_GFxSprite)
                {
                    addOutlinePart(child, placed, outline);
                    continue;
                }
                const unsigned int flags = *reinterpret_cast<const unsigned int*>(child + CHARACTER_FLAGS);
                const auto& cxform = *reinterpret_cast<const fb::GFxCxform*>(child + CHARACTER_CXFORM);
                if (!(flags & CHARACTER_VISIBLE) || (cxform.m_rows[3][0] <= 0.0f && cxform.m_rows[3][1] <= 0.0f)
                    || *reinterpret_cast<void* const*>(child + SPRITE_MASK)) // under a setMask mask, which is drawn on its own
                    continue;
                // ActionScript turns or moves it (the jet radar's compass ring, the icons along its edge): no outline of the
                // minimap even while it holds still, as on the runway
                if (!(flags & ACCEPT_ANIM_MOVES))
                    continue;
                if (flags & CHARACTER_USED_AS_MASK)
                    addOutlinePart(child, placed, outline);
                else if (levels > 1)
                    walkOutline(child, placed, levels - 1, outline);
            }
        }

        // the bounds of what the widget draws that holds still, in its own space; false when nothing is known
        bool outlineBounds(Clip& widget, std::unordered_map<const unsigned char*, OutlinePart>& parts, double out[4])
        {
            unsigned char* character = widget.character();
            if (!character || reinterpret_cast<uintptr_t>(*reinterpret_cast<void* const*>(character)) != OFF_vt_GFxSprite)
                return false;
            Outline outline{ parts };
            walkOutline(character, Affine{ }, OUTLINE_LEVELS, outline);
            if (outline.any)
                std::copy(outline.bounds, outline.bounds + 4, out);
            return outline.any;
        }

        // after ActionScript: the bounds corner nearest to the widget's screen corner, for widgets sized apart from their layout
        void measureWidgets(fb::GFxMovieView* movie)
        {
            // other movies advance too, the root's measurements stay
            fb::UISystem* system = fb::UISystem::GetInstance();
            fb::UIScreenManager* screens = system ? system->m_screenManager : nullptr;
            if (!screens || !screens->m_rootMovie || screens->m_rootMovie->m_movieView != movie)
                return;

            g_corners.clear();
            g_minimapOutline.clear();
            g_backgroundBounds.clear();
            std::unordered_map<std::string, ChildSet> children;
            std::unordered_map<const unsigned char*, OutlinePart> outlineParts;
            const ULONGLONG now = GetTickCount64();
            forEachScaledWidget(movie, [&children, &outlineParts, now](const WidgetContext& c)
            {
                if (c.ownCenter && c.k != 1.0)
                {
                    ChildSet& own = children[c.path];
                    if (const auto known = g_children.find(c.path); known != g_children.end())
                        own = std::move(known->second);
                    if (now - own.scanned >= RESCAN_MS)
                    {
                        if (!rescanChildren(c.movie, c.path, own.clips))
                        {
                            children.erase(c.path); // not built yet, looked for again
                            return;
                        }
                        own.scanned = now;
                    }
                    trackChildren(c.movie, own.clips);
                }
                if (c.minimapEdge || c.minimapBackground)
                {
                    Clip clip;
                    fb::GFxDisplayInfo info{ };
                    double bounds[4];
                    bool measured = clip.resolve(c.movie, c.path.c_str()) && clip.read(&info);
                    if (measured && c.minimapEdge)
                        measured = outlineBounds(clip, outlineParts, bounds) || clip.localBounds(bounds);
                    else if (measured)
                        measured = clip.localBounds(bounds);
                    if (measured)
                    {
                        const double x0 = info.m_x + bounds[0] * info.m_xScale / 100.0, x1 = info.m_x + bounds[2] * info.m_xScale / 100.0;
                        const double y0 = info.m_y + bounds[1] * info.m_yScale / 100.0, y1 = info.m_y + bounds[3] * info.m_yScale / 100.0;
                        if (c.minimapBackground)
                        {
                            g_backgroundBounds[c.path] = { x0, y0, x1, y1 };
                            return;
                        }
                        const auto [entry, added] = g_minimapOutline.try_emplace(c.screen->m_instanceName, std::array<double, 4>{ x0, y0, x1, y1 });
                        if (!added)
                        {
                            std::array<double, 4>& o = entry->second;
                            o = { (std::min)(o[0], x0), (std::min)(o[1], y0), (std::max)(o[2], x1), (std::max)(o[3], y1) };
                        }
                    }
                }
                if (c.k == c.layout || c.ownCenter)
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
            g_children.swap(children);
            g_outlineParts.swap(outlineParts);
        }

        // a clip's origin and axes in its widget's space: p = (x, y) + [a c; b d] * local
        struct Placement
        {
            double x = 0.0, y = 0.0, a = 1.0, b = 0.0, c = 0.0, d = 1.0;
        };

        // the clip at local (x, y) of its parent, turned and scaled by its display info
        Placement place(const Placement& parent, const fb::GFxDisplayInfo& info, double x, double y)
        {
            const double angle = info.m_rotation * std::numbers::pi / 180.0, sx = info.m_xScale / 100.0, sy = info.m_yScale / 100.0;
            const double a = std::cos(angle) * sx, b = std::sin(angle) * sx, c = -std::sin(angle) * sy, d = std::cos(angle) * sy;
            return { parent.x + parent.a * x + parent.c * y, parent.y + parent.b * x + parent.d * y,
                parent.a * a + parent.c * b, parent.b * a + parent.d * b, parent.a * c + parent.c * d, parent.b * c + parent.d * d };
        }

        // the widget is drawn k times larger around its origin
        void holdMovingChildren(const WidgetContext& c)
        {
            const auto children = g_children.find(c.path);
            if (children == g_children.end())
                return;
            const std::vector<Child>& clips = children->second.clips;

            // path -> where ActionScript put it, where it is drawn before the widget's k; parents come before their clips
            std::unordered_map<std::string, std::pair<Placement, Placement>> placed;
            for (const Child& child : clips)
            {
                const std::string below = child.path + ".";
                const bool parentOfMoving = std::any_of(clips.begin(), clips.end(), [&below](const Child& other)
                {
                    return other.moving && other.path.compare(0, below.size(), below) == 0;
                });
                if (!child.moving && !parentOfMoving)
                    continue;

                auto scaled = std::make_unique<Scaled>();
                if (!scaled->clip.resolve(c.movie, child.path.c_str()) || !scaled->clip.read(&scaled->base))
                    continue;

                const auto parent = placed.find(child.path.substr(0, child.path.rfind('.')));
                const Placement parentGame = parent != placed.end() ? parent->second.first : Placement{ };
                const Placement parentDrawn = parent != placed.end() ? parent->second.second : Placement{ };
                fb::GFxDisplayInfo info = scaled->base;
                const Placement game = place(parentGame, info, info.m_x, info.m_y);
                if (!child.moving)
                {
                    placed[child.path] = { game, place(parentDrawn, info, info.m_x, info.m_y) };
                    continue;
                }

                // local = parent axes^-1 * (game / k - parent origin)
                const double det = parentDrawn.a * parentDrawn.d - parentDrawn.b * parentDrawn.c;
                if (std::abs(det) < 1e-9)
                    continue;
                const double tx = game.x / c.k - parentDrawn.x, ty = game.y / c.k - parentDrawn.y;
                info.m_x = (parentDrawn.d * tx - parentDrawn.c * ty) / det;
                info.m_y = (parentDrawn.a * ty - parentDrawn.b * tx) / det;
                info.m_varsSet = 0x1 | 0x2;
                scaled->keepTimeline();
                scaled->clip.write(&info);
                placed[child.path] = { game, place(parentDrawn, info, info.m_x, info.m_y) };
                g_scaled.push_back(std::move(scaled));
            }
        }

        // ActionScript never sees the scaled transforms: they only exist while the movie is drawn
        void scaleWidgets(fb::GFxMovieView* movie)
        {
            forEachScaledWidget(movie, [](const WidgetContext& c)
            {
                auto scaled = std::make_unique<Scaled>();
                if (!scaled->clip.resolve(c.movie, c.path.c_str()) || !scaled->clip.read(&scaled->base))
                    return;
                scaled->keepTimeline();
                // before the alpha below changes it
                scaled->tinted = c.color && scaled->clip.readCxform(&scaled->baseCxform);

                // the corner follows the layout factor, the widget grows from it
                fb::GFxDisplayInfo info = scaled->base;
                double cx = info.m_x, cy = info.m_y;

                if (const auto corner = g_corners.find(c.path); corner != g_corners.end())
                {
                    cx += corner->second.first;
                    cy += corner->second.second;
                }

                double tx = c.ax + (cx - c.ax) * c.layout;
                double ty = c.ay + (cy - c.ay) * c.layout;
                double kx = c.k, ky = c.k;
                // beyond the minimap's outline only the minimap's part grows with it: its edge moves by the minimap's size, the
                // stock gap past it by the widget's own, so a minimap bigger than the tickets leaves no gap between them
                const auto outline = c.followsMinimap || c.minimapBackground ? g_minimapOutline.find(c.screen->m_instanceName) : g_minimapOutline.end();
                if (outline != g_minimapOutline.end())
                {
                    const auto place = [&c](double point, double anchor, double low, double high, double k)
                    {
                        const double edge = std::abs(low - anchor) > std::abs(high - anchor) ? low : high; // the side away from the anchor
                        const bool beyond = (edge - anchor) * (point - edge) > 0.0;
                        return beyond ? anchor + (edge - anchor) * c.layout + (point - edge) * k : anchor + (point - anchor) * c.layout;
                    };
                    const std::array<double, 4>& o = outline->second;
                    const auto background = c.minimapBackground ? g_backgroundBounds.find(c.path) : g_backgroundBounds.end();
                    if (background != g_backgroundBounds.end())
                    {
                        // the panel spans the minimap and the tickets: its side past the minimap ends where the tickets do, or at the
                        // minimap without a ticket panel
                        const double beyondK = elementFactor(Objectives);
                        const std::array<double, 4>& b = background->second;
                        bool ticketsOnly = false;
                        const auto stretch = [&](double& origin, double& k, double low, double high, double anchor, double edgeLow, double edgeHigh)
                        {
                            const double inner = std::abs(low - anchor) <= std::abs(high - anchor) ? low : high, outer = inner == low ? high : low;
                            if (std::abs(outer - inner) < 1e-6)
                                return;
                            const double edge = std::abs(edgeLow - anchor) > std::abs(edgeHigh - anchor) ? edgeLow : edgeHigh;        
                            // radar break it, so needed that
                            const bool across = (edge - anchor) * (inner - edge) <= 0.0 && (edge - anchor) * (outer - edge) > 0.0
                                && std::abs(inner - edge) > std::abs(outer - edge);
                            ticketsOnly |= (edge - anchor) * (outer - edge) > 0.0 && !across;
                            const double reach = settings().removeTicketPanel && across ? edge : outer;
                            const double placedInner = place(inner, anchor, edgeLow, edgeHigh, beyondK), placedOuter = place(reach, anchor, edgeLow, edgeHigh, beyondK);
                            k = (placedOuter - placedInner) / (outer - inner);
                            origin = placedInner + (origin - inner) * k;
                        };
                        double x = info.m_x, y = info.m_y;
                        stretch(x, kx, b[0], b[2], c.ax, o[0], o[2]);
                        stretch(y, ky, b[1], b[3], c.ay, o[1], o[3]);
                        cx = info.m_x, cy = info.m_y;
                        tx = x, ty = y;
                        if (ticketsOnly && settings().removeTicketPanel)
                            info.m_alpha = 0.0;
                    }
                    else if (c.followsMinimap)
                    {
                        tx = place(cx, c.ax, o[0], o[2], c.k);
                        ty = place(cy, c.ay, o[1], o[3], c.k);
                    }
                }
                info.m_x = tx + (info.m_x - cx) * kx;
                info.m_y = ty + (info.m_y - cy) * ky;
                info.m_xScale *= kx;
                info.m_yScale *= ky;
                info.m_alpha *= c.alpha; // on top of the widget's own fades
                info.m_varsSet = VARS_TRANSFORM | VARS_ALPHA;
                scaled->clip.write(&info);
                if (scaled->tinted && c.recolored)
                {
                    const bool through = c.screen->m_renderToTexture && cockpitNeutral();
                    recolorClip(scaled->clip, c.tint, through);
                }
                else if (scaled->tinted)
                    scaled->clip.tint(c.tint, c.replaceColor);
                g_scaled.push_back(std::move(scaled));
                if (c.ownCenter && c.k != 1.0)
                    holdMovingChildren(c);
            });
        }

        // a 1st person vehicle screen; chase cam screens hold the crosshair alone
        bool isCockpitScreen(const fb::UIScreenData* screen)
        {
            if (screen->m_widgetsEnd - screen->m_widgetsBegin <= 1)
                return false;
            for (fb::WidgetNode** w = screen->m_widgetsBegin; w != screen->m_widgetsEnd; ++w)
            {
                WidgetGroup group;
                if (*w && (*w)->m_WidgetAsset && (*w)->m_WidgetAsset->m_Name && widgetGroup((*w)->m_WidgetAsset->m_Name, group)
                    && (group.element == VehicleHud || group.element == VehicleCrosshair))
                    return true;
            }
            return false;
        }

        // the 1st person vehicle screens' widgets outside the HUD groups
        void recolorVehicleScreens(fb::GFxMovieView* movie)
        {
            const Color* hud = widgetColor(VehicleHud);
            fb::UISystem* system = fb::UISystem::GetInstance();
            fb::UIScreenManager* screens = system ? system->m_screenManager : nullptr;
            if (!screens || !screens->m_rootMovie || screens->m_rootMovie->m_movieView != movie)
                return;
            if (hud || widgetColor(VehicleCrosshair))
                scanCockpitShaders();
            if (!hud)
                return;

            for (fb::UIScreenData** s = screens->m_screensBegin; s != screens->m_screensEnd; ++s)
            {
                const fb::UIScreenData* screen = *s;
                if (!screen || !isCockpitScreen(screen))
                    continue;
                for (fb::WidgetNode** w = screen->m_widgetsBegin; w != screen->m_widgetsEnd; ++w)
                {
                    const fb::WidgetNode* widget = *w;
                    WidgetGroup group;
                    if (!widget || !widget->m_InstanceName || (widget->m_WidgetAsset && widget->m_WidgetAsset->m_Name && widgetGroup(widget->m_WidgetAsset->m_Name, group)))
                        continue;
                    auto recolored = std::make_unique<Scaled>();
                    const std::string path = std::string(screen->m_instanceName) + ".instance1." + widget->m_InstanceName;
                    if (!recolored->clip.resolve(movie, path.c_str()) || !recolored->clip.read(&recolored->base))
                        continue;
                    recolored->keepTimeline();
                    recolored->tinted = recolored->clip.readCxform(&recolored->baseCxform);
                    if (!recolored->tinted)
                        continue;
                    const bool through = screen->m_renderToTexture && cockpitNeutral();
                    recolorClip(recolored->clip, *hud, through);
                    g_scaled.push_back(std::move(recolored));
                }
            }
        }

        bool isCommoRose(const fb::UIScreenData* screen)
        {
            return std::memcmp(screen->m_guid, COMMO_ROSE_SCREEN, sizeof(COMMO_ROSE_SCREEN)) == 0;
        }

        // ActionScript keeps the unscaled rose (hkSetScreenVisible), so it is only drawn at its size, around the screen center
        void scaleCommoRose(fb::GFxMovieView* movie)
        {
            fb::UISystem* system = fb::UISystem::GetInstance();
            fb::UIScreenManager* screens = system ? system->m_screenManager : nullptr;
            const double k = currentScale() * elementFactor(CommoRose);
            const double alpha = elementOpacity(CommoRose);
            if ((k == 1.0 && alpha == 1.0) || !screens || !screens->m_rootMovie || screens->m_rootMovie->m_movieView != movie)
                return;

            for (fb::UIScreenData** s = screens->m_screensBegin; s != screens->m_screensEnd; ++s)
            {
                if (!*s || !isCommoRose(*s))
                    continue;

                auto scaled = std::make_unique<Scaled>();
                if (!scaled->clip.resolve(movie, (std::string((*s)->m_instanceName) + ".instance1").c_str()) || !scaled->clip.read(&scaled->base) || !scaled->base.m_visible)
                    return;
                scaled->keepTimeline();

                fb::GRectF visible{ };
                movie->getVisibleFrameRect(&visible);
                const Frame parent = clipFrame(movie, (*s)->m_instanceName);
                const double cx = ((visible.m_left + visible.m_right) * 0.5 - parent.x) / parent.sx;
                const double cy = ((visible.m_top + visible.m_bottom) * 0.5 - parent.y) / parent.sy;

                fb::GFxDisplayInfo info = scaled->base;
                info.m_x = cx + (info.m_x - cx) * k;
                info.m_y = cy + (info.m_y - cy) * k;
                info.m_xScale *= k;
                info.m_yScale *= k;
                info.m_alpha *= alpha;
                info.m_varsSet = VARS_TRANSFORM | VARS_ALPHA;
                scaled->clip.write(&info);
                g_scaled.push_back(std::move(scaled));
                return;
            }
        }

        // a chase cam screen holds the vehicle crosshair alone
        const fb::WidgetNode* chaseCrosshair(const fb::UIScreenData* screen)
        {
            const fb::WidgetNode* widget = screen->m_widgetsEnd - screen->m_widgetsBegin == 1 ? *screen->m_widgetsBegin : nullptr;
            WidgetGroup group;
            const bool chase = widget && widget->m_InstanceName && widget->m_WidgetAsset && widget->m_WidgetAsset->m_Name
                && widgetGroup(widget->m_WidgetAsset->m_Name, group) && (group.element == VehicleCrosshair || group.element == ChaseCrosshair);
            return chase ? widget : nullptr;
        }

        // every shape below the sprite, a sprite being a child of the sprite's own class; each scaled around the center of its bounds
        void thinShapes(unsigned char* sprite, float factor, int levels)
        {

            const void* spriteVtable = *reinterpret_cast<void* const*>(sprite);
            unsigned char* const* children = *reinterpret_cast<unsigned char* const* const*>(sprite + SPRITE_CHILDREN);
            const unsigned int count = *reinterpret_cast<const unsigned int*>(sprite + SPRITE_CHILD_COUNT);
            for (unsigned int i = 0; children && i < count && i < MAX_CHILDREN * 4 && g_shapeCount < MAX_SHAPES; ++i)
            {
                unsigned char* child = children[i];
                if (!child)
                    continue;
                if (*reinterpret_cast<void* const*>(child) == spriteVtable)
                {
                    if (levels > 1)
                        thinShapes(child, factor, levels - 1);
                    continue;
                }
                float identity[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
                float rect[4] = { };
                using GetBoundsFn = float* (__thiscall*)(unsigned char* character, float* out, const float* matrix);
                reinterpret_cast<GetBoundsFn>((*reinterpret_cast<void* const* const*>(child))[GET_BOUNDS_SLOT])(child, rect, identity);
                if (!(rect[2] > rect[0]) || !(rect[3] > rect[1]))
                    continue;

                float* m = reinterpret_cast<float*>(child + CHARACTER_MATRIX);
                ShapeMatrix& shape = g_shapes[g_shapeCount++];
                shape.matrix = m;
                std::memcpy(shape.saved, m, sizeof(shape.saved));
                const float cx = (rect[0] + rect[2]) * 0.5f, cy = (rect[1] + rect[3]) * 0.5f;
                m[2] += (m[0] * cx + m[1] * cy) * (1.0f - factor);
                m[5] += (m[3] * cx + m[4] * cy) * (1.0f - factor);
                m[0] *= factor;
                m[1] *= factor;
                m[3] *= factor;
                m[4] *= factor;
            }
        }

        // below 100%, for this draw only
        void thinChaseCrosshair(fb::GFxMovieView* movie)
        {
            fb::UISystem* system = fb::UISystem::GetInstance();
            fb::UIScreenManager* screens = system ? system->m_screenManager : nullptr;
            const float k = elementFactor(ChaseCrosshair);
            if (k >= 1.0f || !screens || !screens->m_rootMovie || screens->m_rootMovie->m_movieView != movie)
                return;

            for (fb::UIScreenData** s = screens->m_screensBegin; s != screens->m_screensEnd; ++s)
            {
                const fb::WidgetNode* widget = *s ? chaseCrosshair(*s) : nullptr;
                if (!widget)
                    continue;
                Clip shown, crosshair;
                fb::GFxDisplayInfo info{ };
                const std::string path = std::string((*s)->m_instanceName) + ".instance1." + widget->m_InstanceName;
                if (!shown.resolve(movie, (std::string((*s)->m_instanceName) + ".instance1").c_str()) || !shown.read(&info) || !info.m_visible
                    || !crosshair.resolve(movie, path.c_str()))
                    continue;

                if (unsigned char* character = crosshair.character())
                    thinShapes(character, k, SHAPE_LEVELS);
            }
        }

        void restoreShapes()
        {
            for (int i = 0; i < g_shapeCount; ++i)
                std::memcpy(g_shapes[i].matrix, g_shapes[i].saved, sizeof(g_shapes[i].saved));
            g_shapeCount = 0;
            for (int i = 0; i < g_shapeCxformCount; ++i)
                *g_shapeCxforms[i].cxform = g_shapeCxforms[i].saved;
            g_shapeCxformCount = 0;
            for (int i = 0; i < g_blendSaveCount; ++i)
                *g_blendSaves[i].blend = g_blendSaves[i].saved;
            g_blendSaveCount = 0;
        }

        // the chase crosshair's brackets are unnamed shapes of one reticle clip
        // this is a special case
        void thickenChaseCrosshair(fb::GFxMovieView* movie)
        {
            fb::UISystem* system = fb::UISystem::GetInstance();
            fb::UIScreenManager* screens = system ? system->m_screenManager : nullptr;
            const double k = elementFactor(ChaseCrosshair);
            if (k <= 1.0 || !screens || !screens->m_rootMovie || screens->m_rootMovie->m_movieView != movie)
                return;

            // the shown chase screens' crosshairs, by the screen frame's scale; the other shown screens
            std::vector<std::pair<std::unique_ptr<Scaled>, double>> crosshairs;
            std::vector<std::unique_ptr<Scaled>> hidden;
            for (fb::UIScreenData** s = screens->m_screensBegin; s != screens->m_screensEnd; ++s)
            {
                const fb::UIScreenData* screen = *s;
                if (!screen)
                    continue;
                auto shown = std::make_unique<Scaled>();
                if (!shown->clip.resolve(movie, (std::string(screen->m_instanceName) + ".instance1").c_str()) || !shown->clip.read(&shown->base) || !shown->base.m_visible)
                    continue;

                const fb::WidgetNode* widget = chaseCrosshair(screen);
                if (!widget)
                {
                    shown->keepTimeline();
                    hidden.push_back(std::move(shown));
                    continue;
                }

                auto crosshair = std::make_unique<Scaled>();
                const std::string path = std::string(screen->m_instanceName) + ".instance1." + widget->m_InstanceName;
                if (crosshair->clip.resolve(movie, path.c_str()) && crosshair->clip.read(&crosshair->base) && crosshair->base.m_visible)
                {
                    crosshair->keepTimeline();
                    crosshairs.emplace_back(std::move(crosshair), screenFrame(movie, screen->m_instanceName).sx);
                }
            }
            if (crosshairs.empty())
                return;

            for (auto& screen : hidden)
            {
                fb::GFxDisplayInfo info = screen->base;
                info.m_visible = false;
                info.m_varsSet = VARS_VISIBLE;
                screen->clip.write(&info);
            }

            const double radius = THICKEN_PX * (k - 1.0);
            for (int pass = 0; pass < THICKEN_PASSES; ++pass)
            {
                const double angle = 2.0 * std::numbers::pi * pass / THICKEN_PASSES;
                for (auto& [crosshair, scale] : crosshairs)
                {
                    fb::GFxDisplayInfo info = crosshair->base;
                    info.m_x += radius * std::cos(angle) / scale;
                    info.m_y += radius * std::sin(angle) / scale;
                    info.m_varsSet = 0x1 | 0x2;
                    crosshair->clip.write(&info);
                }
                oDisplay(movie);
            }

            for (auto& [crosshair, scale] : crosshairs)
            {
                crosshair->base.m_varsSet = 0x1 | 0x2;
                crosshair->clip.write(&crosshair->base);
                if (crosshair->flags)
                    *crosshair->flags = (*crosshair->flags & ~ACCEPT_ANIM_MOVES) | crosshair->acceptAnimMoves;
            }
            for (auto& screen : hidden)
            {
                screen->base.m_varsSet = VARS_VISIBLE;
                screen->clip.write(&screen->base);
                if (screen->flags)
                    *screen->flags = (*screen->flags & ~ACCEPT_ANIM_MOVES) | screen->acceptAnimMoves;
            }
        }

        void restoreWidgets()
        {
            for (auto& scaled : g_scaled)
            {
                scaled->base.m_varsSet = VARS_TRANSFORM | VARS_ALPHA;
                scaled->clip.write(&scaled->base);
                if (scaled->tinted)
                    scaled->clip.writeCxform(&scaled->baseCxform);
                if (scaled->flags)
                    *scaled->flags = (*scaled->flags & ~ACCEPT_ANIM_MOVES) | scaled->acceptAnimMoves;
            }
            g_scaled.clear();
        }

        // the scale is set on the screen's own clip, ActionScript sees it (Get/SetDisplayInfo)
        bool __fastcall hkSetScreenVisible(fb::UIScreenManager* _this, void*, fb::UIScreenData* screen, bool visible)
        {
            const bool shown = oSetScreenVisible(_this, screen, visible);
            if (!shown || !visible || !screen || !_this->m_rootMovie || !isCommoRose(screen))
                return shown;

            Clip clip;
            fb::GFxDisplayInfo info{ };
            if (clip.resolve(_this->m_rootMovie->m_movieView, (std::string(screen->m_instanceName) + ".instance1").c_str()) && clip.read(&info))
            {
                info.m_xScale = info.m_yScale = 100.0 / currentScale();
                info.m_varsSet = 0x8 | 0x10;
                clip.write(&info);
            }
            return shown;
        }

        void __fastcall hkInitializeScreen(fb::UIScreenManager* _this, void*, const char* name)
        {
            linkHudList();
            for (fb::UIScreenData** s = _this->m_screensBegin; s != _this->m_screensEnd; ++s)
            {
                if (!*s || std::strcmp((*s)->m_instanceName, name) != 0)
                    continue;

                if (hasWidget(*s, HUD_LIST_NAMES[0]) && _this->m_rootMovie)
                    createHudListClip(_this->m_rootMovie->m_movieView, *s);
            }
            oInitializeScreen(_this, name);
        }

        float __fastcall hkAdvance(fb::GFxMovieView* _this, void*, float dt, unsigned int catchUp)
        {
            const float next = oAdvance(_this, dt, catchUp);
            fitHudTabs(_this);
            showHudList(_this);
            updatePicker(_this);
            measureWidgets(_this);
            updateScopeColor();
            return next;
        }

        long long nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // a kill wins over a headshot, a headshot is used up by the damage it confirms
        void __fastcall hkWeaponMessage(void* _this, void*, const unsigned char* message)
        {
            if (*reinterpret_cast<const int*>(message + 8) == MESSAGE_DAMAGE_GIVEN)
            {
                const long long now = nowMs();
                if (now - g_lastKillTime < KILL_WINDOW_MS)
                    g_hitMarker = KillMarkers;
                else if (now - g_lastHeadshotTime < HEADSHOT_WINDOW_MS)
                    g_hitMarker = HeadshotMarkers;
                else
                    g_hitMarker = HitMarkers;
                g_lastHeadshotTime = -HEADSHOT_WINDOW_MS;
            }
            oWeaponMessage(_this, message);
        }

        void __fastcall hkHudTextMessage(void* _this, void*, const unsigned char* message)
        {
            if (*reinterpret_cast<const int*>(message + 8) == MESSAGE_KILLED_OTHER)
            {
                g_lastKillTime = nowMs();
                g_hitMarker = KillMarkers; // the killing hit's marker may already be up
            }
            oHudTextMessage(_this, message);
        }

        void __fastcall hkBulletHit(unsigned char* _this, void*, const unsigned char* message)
        {
            if (*reinterpret_cast<const int*>(message + 8) == MESSAGE_BULLET_HIT)
            {
                const unsigned int bone = *reinterpret_cast<const unsigned int*>(message + BULLET_HIT_BONE);
                const unsigned char* context = *reinterpret_cast<unsigned char* const*>(OFF_g_clientGameContext);
                void* players = context ? *reinterpret_cast<void* const*>(context + PLAYER_MANAGER) : nullptr;
                fb::ClientPlayer* shooter = players ? reinterpret_cast<GetPlayerFn>(OFF_ClientPlayerManager_getPlayer)(players, *reinterpret_cast<const unsigned int*>(message + BULLET_HIT_SHOOTER)) : nullptr;
                fb::ClientPlayer* local = fb::ClientPlayer::GetLocal();
                if (shooter && shooter == local)
                {
                    const auto me = reinterpret_cast<const unsigned char*>(local->getSoldier());
                    const auto hit = *reinterpret_cast<const unsigned char* const*>(_this + COMPONENT_SOLDIER);
                    const bool teammate = me && hit && *reinterpret_cast<const int*>(me + SOLDIER_TEAM) == *reinterpret_cast<const int*>(hit + SOLDIER_TEAM);
                    if (bone == HEAD_BONE && !teammate)
                        g_lastHeadshotTime = nowMs();
                }
            }
            oBulletHit(_this, message);
        }

        void __fastcall hkDisplay(fb::GFxMovieView* _this, void*)
        {
            holdCockpitShaders();
            scaleWidgets(_this);
            recolorVehicleScreens(_this);
            scaleCommoRose(_this);
            thinChaseCrosshair(_this);
            oDisplay(_this);
            restoreShapes();
            thickenChaseCrosshair(_this);
            restoreWidgets();
        }
    }

    void installWidgetHooks()
    {
        hook(OFF_UIScreenManager_initializeScreen, hkInitializeScreen, &oInitializeScreen);
        hook(OFF_UIScreenManager_setScreenVisible, hkSetScreenVisible, &oSetScreenVisible);
        hook(OFF_GFxMovieRoot_advance, hkAdvance, &oAdvance);
        hook(OFF_GFxMovieRoot_display, hkDisplay, &oDisplay);
        hook(OFF_UIWeaponComp_onMessage, hkWeaponMessage, &oWeaponMessage);
        hook(OFF_UIMessageComp_onMessage, hkHudTextMessage, &oHudTextMessage);
        hook(OFF_SoldierBulletHit_onMessage, hkBulletHit, &oBulletHit);
    }
}
