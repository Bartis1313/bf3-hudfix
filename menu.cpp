#include "hudfix.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace hudfix
{
    namespace
    {
        using RegisterKeysFn = void(__thiscall*)(void* manager, void* componentData, void* component);
        using DataValueFn = bool(__thiscall*)(void* component, int key, fb::UIDataValue* value);
        using PartitionLoadedFn = void(__thiscall*)(fb::InternalDatabasePartition* partition);
        using SetMemberFn = void(__thiscall*)(fb::UIDataValue* value, const char* name, const fb::UIDataValue* member);
        using DestroyFn = void(__thiscall*)(fb::UIDataValue* value);
        using MakeContainerFn = void(__thiscall*)(fb::UIDataValue* value, int count);
        using SetEnumItemFn = void(__cdecl*)(fb::UIDataValue* container, int index, const char* label, const char* indexValue);
        using SetTypeFn = void(__thiscall*)(fb::UIDataValue* value, int type);

        RegisterKeysFn oRegisterKeys = nullptr;
        DataValueFn oGetDataValue = nullptr;
        DataValueFn oSetDataValue = nullptr;
        PartitionLoadedFn oPartitionLoaded = nullptr;

        // part to trick cloning
        constexpr const char* SETTINGS_COMP = "UI/UIComponents/UISettingsComp";
        constexpr const char* GAMEPLAY_PAGE = "screen/optionsgameplayscreen";
        constexpr const char* MAIN_LIST = "List_01";

        enum class Kind
        {
            Auto,
            SightZoom,
            Scale,
            Element,
            Opacity,
            Scoped,
            Color,
            Brightness,
            TicketPanel,
            UniformAiming,
            ZoomSensitivity
        };

        enum Tab
        {
            TabHud,
            TabAim,
            TabTags,
            TabVehicle,
            TabCount
        };
        constexpr const char* TAB_NAMES[TabCount] = { "HUD", "AIM", "TAGS", "VEHICLE" };
        constexpr const char* TAB_SOURCES[TabCount] = { "HudFixShowHud", "HudFixShowAim", "HudFixShowTags", "HudFixShowVehicle" };
        constexpr const char* TAB_BAR_TABS = "HUD;AIM;TAGS;VEHICLE"; // TabBar "Tabs" property
        constexpr const char* TAB_SOURCE = "HudFixTab"; // what the tab bar's Released writes
        constexpr const char* MAIN_TAB_BAR = "TabBar_01";
        constexpr const char* SET_VALUE_NODE = "SetValue"; // List_01.SelectorChanged's DataSetNode

        struct Row
        {
            const char* source; // UISettingsComp data source name
            const char* name;
            Kind kind;
            int element;
            int tab;
            int key; // -1 = not special
        };

        Row g_rows[] =
        {
            { "HudFixAuto", "AUTO HUD SCALE", Kind::Auto, -1, TabHud },
            { "HudFixScale", "HUD SCALE", Kind::Scale, -1, TabHud },
            { "HudFixMinimap", "MINIMAP SIZE", Kind::Element, Minimap, TabHud },
            { "HudFixMinimapIcons", "MAP ICON SIZE", Kind::Element, MinimapIcons, TabHud },
            { "HudFixMinimapIconsOpacity", "MAP ICON OPACITY", Kind::Opacity, MinimapIcons, TabHud },
            { "HudFixSquadList", "SQUAD SIZE", Kind::Element, SquadList, TabHud },
            { "HudFixSquadListOpacity", "SQUAD OPACITY", Kind::Opacity, SquadList, TabHud },
            { "HudFixObjectives", "TICKETS SIZE", Kind::Element, Objectives, TabHud },
            { "HudFixObjectivesOpacity", "TICKETS OPACITY", Kind::Opacity, Objectives, TabHud },
            { "HudFixRemoveTicketPanel", "REMOVE TICKET PANEL", Kind::TicketPanel, -1, TabHud },
            { "HudFixAmmoHealth", "AMMO/HP SIZE", Kind::Element, AmmoHealth, TabHud },
            { "HudFixAmmoHealthOpacity", "AMMO/HP OPACITY", Kind::Opacity, AmmoHealth, TabHud },
            { "HudFixKillLog", "KILL LOG SIZE", Kind::Element, KillLog, TabHud },
            { "HudFixKillLogOpacity", "KILL LOG OPACITY", Kind::Opacity, KillLog, TabHud },
            { "HudFixScoreMessages", "SCORE POPUP SIZE", Kind::Element, ScoreMessages, TabHud },
            { "HudFixScoreMessagesOpacity", "SCORE POPUP OPACITY", Kind::Opacity, ScoreMessages, TabHud },
            { "HudFixAwards", "AWARD SIZE", Kind::Element, Awards, TabHud },
            { "HudFixAwardsOpacity", "AWARD OPACITY", Kind::Opacity, Awards, TabHud },
            { "HudFixChat", "CHAT SIZE", Kind::Element, Chat, TabHud },
            { "HudFixChatOpacity", "CHAT OPACITY", Kind::Opacity, Chat, TabHud },
            { "HudFixCommoRose", "COMMO ROSE SIZE", Kind::Element, CommoRose, TabHud },
            { "HudFixCommoRoseOpacity", "COMMO ROSE OPACITY", Kind::Opacity, CommoRose, TabHud },

            { "HudFixCrosshair", "CROSSHAIR SIZE", Kind::Element, Crosshair, TabAim },
            { "HudFixCrosshairColor", "CROSSHAIR COLOR", Kind::Color, CrosshairColor, TabAim },
            { "HudFixScopeColor", "SIGHT COLOR", Kind::Color, SightColor, TabAim },
            { "HudFixScopeSize", "SCOPE SIZE", Kind::Element, Scope, TabAim },
            { "HudFixSightZoom", "SIGHT ZOOM", Kind::SightZoom, -1, TabAim },
            { "HudFixUniformAiming", "UNIFORM AIMING", Kind::UniformAiming, -1, TabAim },
            { "HudFixZoomSensitivity", "ZOOM SENSITIVITY", Kind::ZoomSensitivity, -1, TabAim },
            { "HudFixHitMarkers", "HIT MARKER SIZE", Kind::Element, HitMarkers, TabAim },
            { "HudFixHitMarkersColor", "HIT MARKER COLOR", Kind::Color, HitColor, TabAim },
            { "HudFixHeadshotMarkers", "HEADSHOT SIZE", Kind::Element, HeadshotMarkers, TabAim },
            { "HudFixHeadshotMarkersColor", "HEADSHOT COLOR", Kind::Color, HeadshotColor, TabAim },
            { "HudFixKillMarkers", "KILL MARKER SIZE", Kind::Element, KillMarkers, TabAim },
            { "HudFixKillMarkersColor", "KILL MARKER COLOR", Kind::Color, KillColor, TabAim },

            { "HudFixNametags", "TEAM TAG SIZE", Kind::Element, Nametags, TabTags },
            { "HudFixNametagsOpacity", "TEAM TAG OPACITY", Kind::Opacity, Nametags, TabTags },
            { "HudFixNametagsScoped", "TEAM TAG SCOPED", Kind::Scoped, Nametags, TabTags },
            { "HudFixNametagsEnemy", "ENEMY TAG SIZE", Kind::Element, NametagsEnemy, TabTags },
            { "HudFixNametagsEnemyOpacity", "ENEMY TAG OPACITY", Kind::Opacity, NametagsEnemy, TabTags },
            { "HudFixNametagsEnemyScoped", "ENEMY TAG SCOPED", Kind::Scoped, NametagsEnemy, TabTags },
            { "HudFixHealthBars", "TEAM HP BAR SIZE", Kind::Element, HealthBars, TabTags },
            { "HudFixHealthBarsOpacity", "TEAM HP BAR OPACITY", Kind::Opacity, HealthBars, TabTags },
            { "HudFixHealthBarsScoped", "TEAM HP BAR SCOPED", Kind::Scoped, HealthBars, TabTags },
            { "HudFixHealthBarsEnemy", "ENEMY HP BAR SIZE", Kind::Element, HealthBarsEnemy, TabTags },
            { "HudFixHealthBarsEnemyOpacity", "ENEMY HP BAR OPACITY", Kind::Opacity, HealthBarsEnemy, TabTags },
            { "HudFixHealthBarsEnemyScoped", "ENEMY HP BAR SCOPED", Kind::Scoped, HealthBarsEnemy, TabTags },
            { "HudFixReviveIcons", "REVIVE SIZE", Kind::Element, ReviveIcons, TabTags },
            { "HudFixReviveIconsOpacity", "REVIVE OPACITY", Kind::Opacity, ReviveIcons, TabTags },
            { "HudFixReviveIconsScoped", "REVIVE SCOPED", Kind::Scoped, ReviveIcons, TabTags },
            { "HudFixFlags", "FLAG SIZE", Kind::Element, Flags, TabTags },
            { "HudFixFlagsOpacity", "FLAG OPACITY", Kind::Opacity, Flags, TabTags },
            { "HudFixFlagsScoped", "FLAG SCOPED", Kind::Scoped, Flags, TabTags },

            { "HudFixVehicleCrosshair", "VEHICLE AIM SIZE", Kind::Element, VehicleCrosshair, TabVehicle },
            { "HudFixVehicleCrosshairColor", "VEHICLE AIM COLOR", Kind::Color, VehicleCrosshairColor, TabVehicle },
            { "HudFixChaseCrosshair", "CHASE AIM WIDTH", Kind::Element, ChaseCrosshair, TabVehicle },
            { "HudFixChaseCrosshairColor", "CHASE AIM COLOR", Kind::Color, ChaseCrosshairColor, TabVehicle },
            { "HudFixVehicleHud", "VEHICLE HUD SIZE", Kind::Element, VehicleHud, TabVehicle },
            { "HudFixVehicleHudColor", "VEHICLE HUD COLOR", Kind::Color, VehicleHudColor, TabVehicle },
            { "HudFixVehicleBrightness", "VEHICLE BRIGHTNESS", Kind::Brightness, -1, TabVehicle },
        };
        constexpr size_t ROW_COUNT = std::size(g_rows);

        constexpr int SCALE_MIN = 100;
        constexpr int ELEMENT_MIN = 50;
        constexpr int ELEMENT_MAX = 300;
        constexpr int ELEMENT_STEP = 10;
        constexpr int OPACITY_MIN = 10;
        constexpr int OPACITY_MAX = 100;
        constexpr int SCOPED_MIN = 0;

        // UIComponentManager::registerComponentDataKeys reads name +0x8, DataSources +0x10 (count at -4), vtable slot 2
        struct FakeComponentData
        {
            void* const* vtable;
            int _4;
            const char* name;
            int _c;
            const char* const* sources;
        };

        struct SourceArray
        {
            unsigned int unk;
            unsigned int count;
            const char* names[ROW_COUNT + TabCount + 1];
        };

        SourceArray g_sources{ };
        FakeComponentData g_fakeData{ };

        int g_tab = TabHud;
        int g_previousTab = TabHud; // its rows are hidden on the tab change
        int g_tabKeys[TabCount];
        int g_tabKey = 0;
        bool g_tabChanged = false;
        int g_pickedRowKey = 0;
        char g_tabSelected[8] = "0";

        struct HudList
        {
            const fb::WidgetNode* main;
            const fb::UINestedListDataBinding* mainBinding;
            const fb::NestedList* templateRow;
            fb::WidgetNode* list;
            fb::UINestedListDataBinding* binding;
            const fb::WidgetNode* mainTabs;
            fb::WidgetNode* tabs;
            const fb::DataSetNode* setValue;
            fb::DataSetNode* setTab;
        };
        HudList g_hudList{ };

        bool __fastcall requiresPlayer(void*, void*)
        {
            return false;
        }
        void* const g_fakeVtable[4] = { nullptr, nullptr, reinterpret_cast<void*>(&requiresPlayer), nullptr };

        std::vector<std::unique_ptr<char[]>> g_blocks; // cloned objects and arrays, trick to avoid alloc issues

        int dataKey(const char* source)
        {
            const std::string full = std::string("UI_UISettingsComp_") + source;
            unsigned int hash = 5381;
            for (const char c : full)
                hash = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(c))) ^ (33 * hash);
            return static_cast<int>(hash);
        }

        const Row* findRow(int key)
        {
            for (const Row& row : g_rows)
            {
                if (row.key == key)
                    return &row;
            }
            return nullptr;
        }

        void buildKeys()
        {
            for (size_t i = 0; i < ROW_COUNT; ++i)
            {
                g_rows[i].key = dataKey(g_rows[i].source);
                g_sources.names[i] = g_rows[i].source;
            }
            for (int i = 0; i < TabCount; ++i)
            {
                g_tabKeys[i] = dataKey(TAB_SOURCES[i]);
                g_sources.names[ROW_COUNT + i] = TAB_SOURCES[i];
            }
            g_tabKey = dataKey(TAB_SOURCE);
            g_sources.names[ROW_COUNT + TabCount] = TAB_SOURCE;
            g_sources.count = static_cast<unsigned int>(ROW_COUNT + TabCount + 1);
        }

        int itemCount(const Row& row)
        {
            switch (row.kind)
            {
            case Kind::Auto:
            case Kind::TicketPanel:
            case Kind::UniformAiming:
                return 2;
            case Kind::ZoomSensitivity:
                return (ZOOM_SENSITIVITY_MAX - ZOOM_SENSITIVITY_MIN) / ZOOM_SENSITIVITY_STEP + 1;
            case Kind::SightZoom:
                return SightZoomCount;
            case Kind::Scale:
                return (std::max)(static_cast<int>(std::floor(autoScale() * 100.0f + 1e-3f)) - SCALE_MIN, 0) / ELEMENT_STEP + 1;
            case Kind::Opacity:
                return (OPACITY_MAX - OPACITY_MIN) / ELEMENT_STEP + 1;
            case Kind::Scoped:
                return (OPACITY_MAX - SCOPED_MIN) / ELEMENT_STEP + 1;
            case Kind::Color:
                return 2; // DEFAULT, CUSTOM
            case Kind::Brightness:
                return (VEHICLE_BRIGHTNESS_MAX - VEHICLE_BRIGHTNESS_MIN) / ELEMENT_STEP + 1;
            default:
                return (ELEMENT_MAX - ELEMENT_MIN) / ELEMENT_STEP + 1;
            }
        }

        int percentOfItem(const Row& row, int index)
        {
            switch (row.kind)
            {
            case Kind::Scale:
                return SCALE_MIN + index * ELEMENT_STEP;
            case Kind::Opacity:
                return OPACITY_MIN + index * ELEMENT_STEP;
            case Kind::Scoped:
                return SCOPED_MIN + index * ELEMENT_STEP;
            case Kind::Brightness:
                return VEHICLE_BRIGHTNESS_MIN + index * ELEMENT_STEP;
            case Kind::ZoomSensitivity:
                return ZOOM_SENSITIVITY_MIN + index * ZOOM_SENSITIVITY_STEP;
            default:
                return ELEMENT_MIN + index * ELEMENT_STEP;
            }
        }

        int itemOfSetting(const Row& row)
        {
            switch (row.kind)
            {
            case Kind::Auto:
                return settings().autoScale ? 1 : 0;
            case Kind::TicketPanel:
                return settings().removeTicketPanel ? 1 : 0;
            case Kind::UniformAiming:
                return settings().uniformAiming ? 1 : 0;
            case Kind::ZoomSensitivity:
                return std::clamp((settings().zoomSensitivity - ZOOM_SENSITIVITY_MIN + ZOOM_SENSITIVITY_STEP / 2) / ZOOM_SENSITIVITY_STEP, 0, itemCount(row) - 1);
            case Kind::SightZoom:
                return std::clamp(settings().sightZoom, 0, itemCount(row) - 1);
            case Kind::Scale:
                return std::clamp((settings().scale - SCALE_MIN + ELEMENT_STEP / 2) / ELEMENT_STEP, 0, itemCount(row) - 1);
            case Kind::Opacity:
                return std::clamp((settings().opacity[row.element] - OPACITY_MIN + ELEMENT_STEP / 2) / ELEMENT_STEP, 0, itemCount(row) - 1);
            case Kind::Scoped:
                return std::clamp((settings().scoped[row.element] - SCOPED_MIN + ELEMENT_STEP / 2) / ELEMENT_STEP, 0, itemCount(row) - 1);
            case Kind::Color:
                return settings().color[row.element] == CUSTOM_COLOR ? 1 : 0;
            case Kind::Brightness:
                return std::clamp((settings().vehicleBrightness - VEHICLE_BRIGHTNESS_MIN + ELEMENT_STEP / 2) / ELEMENT_STEP, 0, itemCount(row) - 1);
            default:
                return std::clamp((settings().element[row.element] - ELEMENT_MIN + ELEMENT_STEP / 2) / ELEMENT_STEP, 0, itemCount(row) - 1);
            }
        }

        // as UISettingsComp::getDataValue writes its bools
        void setBool(fb::UIDataValue* target, bool value)
        {
            if (target->m_type != 2)
                reinterpret_cast<SetTypeFn>(OFF_UIDataValue_setType)(target, 2);
            else if (static_cast<bool>(target->m_int & 0xFF) != value)
                target->m_flags |= 1;
            target->m_int = value ? 1 : 0;
        }

        void setInt(fb::UIDataValue* target, const char* name, int value)
        {
            fb::UIDataValue scalar{ };
            scalar.m_arena = reinterpret_cast<void*>(OFF_g_uiDataArena);
            scalar.m_int = value;
            scalar.m_type = 4;
            scalar.m_flags = 3;
            reinterpret_cast<SetMemberFn>(OFF_UIDataValue_setMember)(target, name, &scalar);
            reinterpret_cast<DestroyFn>(OFF_UIDataValue_destroy)(&scalar);
        }

        // sub_906A70: key, Highlighted, SubItems {Index, Label}
        constexpr size_t SCRATCH_SIZE = 0x100000; // test, if crash at alloc, then this is the cause

        void fillRow(const Row& row, fb::UIDataValue* value)
        {
            thread_local std::unique_ptr<char[]> scratch(new char[SCRATCH_SIZE]);
            fb::UIStackArena arena{ };
            arena.m_vtable = reinterpret_cast<void*>(OFF_vt_UIStackArena);
            arena.m_refCount = 1;
            arena.m_cursor = scratch.get();

            const int count = itemCount(row);
            fb::UIDataValue items{ };
            items.m_arena = &arena;
            items.m_flags = 1;
            reinterpret_cast<MakeContainerFn>(OFF_UIDataValue_makeContainer)(&items, count);
            for (int i = 0; i < count; ++i)
            {
                char label[32], index[8];
                if (row.kind == Kind::Auto || row.kind == Kind::TicketPanel || row.kind == Kind::UniformAiming)
                    std::snprintf(label, sizeof(label), "%s", i ? "ID_M_OPTIONS_YES" : "ID_M_OPTIONS_NO");
                else if (row.kind == Kind::Color)
                    std::snprintf(label, sizeof(label), "%s", i ? "CUSTOM" : "DEFAULT");
                else if (row.kind == Kind::SightZoom)
                    std::snprintf(label, sizeof(label), "%s", SIGHT_ZOOM_NAMES[i]);
                else
                    std::snprintf(label, sizeof(label), "%d%%", percentOfItem(row, i));

                std::snprintf(index, sizeof(index), "%d", i);
                reinterpret_cast<SetEnumItemFn>(OFF_UIDataValue_setEnumItem)(&items, i, label, index);
            }

            setInt(value, "key", row.key);
            setInt(value, "Highlighted", itemOfSetting(row));
            reinterpret_cast<SetMemberFn>(OFF_UIDataValue_setMember)(value, "SubItems", &items);
            reinterpret_cast<DestroyFn>(OFF_UIDataValue_destroy)(&items);

            static size_t mostUsed = 0;
            const size_t used = static_cast<size_t>(arena.m_cursor - scratch.get());
            if (used > mostUsed)
            {
                mostUsed = used;
                log("{}: {} items took {} of {} arena bytes", row.source, count, used, SCRATCH_SIZE);
            }
        }

        int selectedIndex(const fb::UIDataValue* value)
        {
            if (value->m_type == 3)
                return static_cast<int>(value->m_number);
            if (value->m_type == 4)
                return value->m_int;
            // past a prefix like the key bindings tabs' "ID0"
            const char* text = value->m_string;
            while (text && *text && !std::isdigit(static_cast<unsigned char>(*text)))
                ++text;
            return text ? std::atoi(text) : 0;
        }

        void setTab(const fb::UIDataValue* value)
        {
            const int index = std::clamp(selectedIndex(value), 0, TabCount - 1);
            log("{} tab {} (value type {})", HUD_TABS_NAME, TAB_NAMES[index], value->m_type);
            g_tabChanged = index != g_tab;
            if (g_tabChanged)
                g_previousTab = g_tab;
            g_tab = index;
            std::snprintf(g_tabSelected, sizeof(g_tabSelected), "%d", index);
        }

        void applyRow(const Row& row, const fb::UIDataValue* value)
        {
            const int index = std::clamp(selectedIndex(value), 0, itemCount(row) - 1);
            switch (row.kind)
            {
            case Kind::Auto:
                settings().autoScale = index == 1;
                break;
            case Kind::TicketPanel:
                settings().removeTicketPanel = index == 1;
                break;
            case Kind::UniformAiming:
                settings().uniformAiming = index == 1;
                applyZoomFov();
                break;
            case Kind::ZoomSensitivity:
                settings().zoomSensitivity = percentOfItem(row, index);
                applyZoomFov();
                break;
            case Kind::SightZoom:
                settings().sightZoom = index;
                applyZoomFov();
                break;
            case Kind::Scale:
                settings().scale = percentOfItem(row, index);
                break;
            case Kind::Element:
                settings().element[row.element] = percentOfItem(row, index);
                if (row.element == Scope)
                    applyScopeSize();
                break;
            case Kind::Opacity:
                settings().opacity[row.element] = percentOfItem(row, index);
                break;
            case Kind::Scoped:
                settings().scoped[row.element] = percentOfItem(row, index);
                break;
            case Kind::Color:
                settings().color[row.element] = index ? CUSTOM_COLOR : 0;
                if (row.element == SightColor)
                    applyScopeColor();
                break;
            case Kind::Brightness:
                settings().vehicleBrightness = percentOfItem(row, index);
                rebuildCockpitShaders();
                break;
            }
            saveSettings();
            log("{} -> item {}", row.source, index);
        }

        void __fastcall hkRegisterKeys(void* _this, void*, void* componentData, void* component)
        {
            oRegisterKeys(_this, componentData, component);

            const char* name = *reinterpret_cast<const char* const*>(static_cast<char*>(componentData) + 8);
            if (!name || _stricmp(name, SETTINGS_COMP) != 0)
                return;

            g_fakeData.vtable = g_fakeVtable;
            g_fakeData.name = name;
            g_fakeData.sources = g_sources.names;
            oRegisterKeys(_this, &g_fakeData, component);
            log("registered {} option keys", ROW_COUNT);
        }

        bool __fastcall hkGetDataValue(void* _this, void*, int key, fb::UIDataValue* value)
        {
            for (int i = 0; i < TabCount; ++i)
            {
                if (key == g_tabKeys[i])
                {
                    setBool(value, i == g_tab);
                    return true;
                }
            }
            if (const Row* row = findRow(key))
            {
                fillRow(*row, value);
                return true;
            }
            return oGetDataValue(_this, key, value);
        }

        bool __fastcall hkSetDataValue(void* _this, void*, int key, fb::UIDataValue* value)
        {
            if (key == g_tabKey)
            {
                setTab(value);
                return true;
            }
            if (const Row* row = findRow(key))
            {
                applyRow(*row, value);
                return true;
            }
            return oSetDataValue(_this, key, value);
        }

        void* allocate(size_t size)
        {
            g_blocks.push_back(std::make_unique<char[]>(size));
            std::memset(g_blocks.back().get(), 0, size);
            return g_blocks.back().get();
        }

        // never released for simplicity, the game will free it on exit
        template <typename T>
        T* clone(const T* source)
        {
            const bool guid = (source->m_flags & 0x100) != 0;
            const size_t prefix = guid ? 0x10 : 0;
            char* block = static_cast<char*>(allocate(prefix + sizeof(T)));
            std::memcpy(block, reinterpret_cast<const char*>(source) - prefix, prefix + sizeof(T));
            T* copy = reinterpret_cast<T*>(block + prefix);
            copy->m_refCnt = 0x4000;
            if (guid)
            {
                static unsigned int counter = 0;
                auto id = reinterpret_cast<unsigned int*>(block);
                id[2] ^= 0x48554446; // "HUDF"
                id[3] ^= ++counter;
            }
            return copy;
        }

        template <typename T>
        std::vector<T> itemsOf(fb::Array<T>& array)
        {
            return std::vector<T>(array.m_firstElement, array.m_firstElement + array.size());
        }

        template <typename T>
        void setArray(fb::Array<T>& array, const std::vector<T>& items)
        {
            fb::ArrayHeader* header = static_cast<fb::ArrayHeader*>(allocate(sizeof(fb::ArrayHeader) + items.size() * sizeof(T)));
            header->unk = array.GetHeader() ? array.GetHeader()->unk : 0;
            header->size = static_cast<uint32_t>(items.size());
            T* elements = reinterpret_cast<T*>(header + 1);
            std::copy(items.begin(), items.end(), elements);
            array.m_firstElement = elements;
        }

        fb::UINodePort* portOf(fb::Array<fb::UINodePort*>& ports, fb::UIWidgetEventID query)
        {
            for (fb::UINodePort* port : itemsOf(ports))
            {
                if (port->m_Query == query)
                    return port;
            }
            return nullptr;
        }

        // a page's rows read its page key as DynamicShowList, sub_88A350 leaves a row out of the list while it reads false
        void showOnTab(fb::NestedList& item, const Row& row, const fb::NestedList& templateRow)
        {
            if (row.tab < 0)
            {
                item.m_DynamicShowList = templateRow.m_DynamicShowList;
                return;
            }
            item.m_DynamicShowList = templateRow.m_ListDataSource;
            item.m_DynamicShowList.m_DataName = const_cast<char*>(TAB_SOURCES[row.tab]);
            item.m_DynamicShowList.m_DataKey = g_tabKeys[row.tab];
        }

        void addHudList(fb::UIGraphAsset* screen, fb::WidgetNode* main)
        {
            fb::UINestedListDataBinding* mainBinding = static_cast<fb::UINestedListDataBinding*>(main->m_DataBinding);
            const fb::NestedList* templateRow = nullptr;
            for (unsigned int i = 0; i < mainBinding->m_NestedLists.size(); ++i)
            {
                if (mainBinding->m_NestedLists.m_firstElement[i].m_ListDataSource.m_DataKey == 0x10D5B2E6) //templatekey
                    templateRow = &mainBinding->m_NestedLists.m_firstElement[i];
            }
            if (!templateRow)
                return;

            fb::UINestedListDataBinding* binding = clone(mainBinding);
            std::vector<fb::NestedList> rows;
            for (const Row& row : g_rows)
            {
                fb::NestedList item = *templateRow;
                item.m_Label = const_cast<char*>(row.name);
                item.m_ListDataSource.m_DataKey = row.key;
                showOnTab(item, row, *templateRow);
                rows.push_back(item);
            }
            setArray(binding->m_NestedLists, rows);
            binding->m_ListIndex = 5; // hud list index
            binding->m_KeepScrollOffset = true;
            // too many rows for the List_01 mask, sent as "UseScrollBar" by sub_88A350 for mouse scrolling
            binding->m_UseScrollBar = true;
            // for arrow keys specifically
            binding->m_NavigationType = fb::sendEventTopBottom;
            mainBinding->m_NavigationType = fb::sendEventTopBottom;

            fb::WidgetNode* list = clone(main);
            list->m_Name = const_cast<char*>("HudFix List");
            list->m_InstanceName = const_cast<char*>(HUD_LIST_NAME);
            list->m_DataBinding = binding;
            list->m_FocusIndex = -1;
            std::vector<std::pair<fb::UINodePort*, fb::UINodePort*>> ports;
            for (fb::Array<fb::UINodePort*>* array : { &list->m_Inputs, &list->m_Outputs })
            {
                std::vector<fb::UINodePort*> items = itemsOf(*array);
                for (fb::UINodePort*& port : items)
                {
                    fb::UINodePort* copy = clone(port);
                    copy->m_InstanceName = const_cast<char*>(HUD_LIST_NAME);
                    ports.emplace_back(port, copy);
                    port = copy;
                }
                setArray(*array, items);
            }
            const auto mapPort = [&](fb::UINodePort* port)
                {
                    for (const auto& [from, to] : ports)
                    {
                        if (from == port)
                            return to;
                    }
                    return port;
                };

            std::vector<fb::UINodeConnection*> connections = itemsOf(screen->m_Connections);
            const size_t original = connections.size();
            for (size_t i = 0; i < original; ++i)
            {
                fb::UINodeConnection* source = connections[i];
                if (source->m_SourceNode != main && source->m_TargetNode != main)
                    continue;

                fb::UINodeConnection* copy = clone(source);
                if (copy->m_SourceNode == main)
                {
                    copy->m_SourceNode = list;
                    copy->m_SourcePort = mapPort(copy->m_SourcePort);
                }
                if (copy->m_TargetNode == main)
                {
                    copy->m_TargetNode = list;
                    copy->m_TargetPort = mapPort(copy->m_TargetPort);
                }
                connections.push_back(copy);
            }

            // down past the last main row enters the new list, up past its first row returns (Video List_01/List_02)
            const auto link = [&](fb::WidgetNode* from, fb::UIWidgetEventID out, fb::WidgetNode* to, fb::UIWidgetEventID in)
                {
                    fb::UINodePort* sourcePort = portOf(from->m_Outputs, out);
                    fb::UINodePort* targetPort = portOf(to->m_Inputs, in);
                    if (!sourcePort || !targetPort || original == 0)
                        return;

                    fb::UINodeConnection* copy = clone(connections.front());
                    copy->m_SourceNode = from;
                    copy->m_SourcePort = sourcePort;
                    copy->m_TargetNode = to;
                    copy->m_TargetPort = targetPort;
                    copy->m_NumScreensToPop = 0;
                    connections.push_back(copy);
                };
            link(main, fb::UIWidgetEventID_OnReachedBottom, list, fb::UIWidgetEventID_EnterTop);
            link(list, fb::UIWidgetEventID_OnReachedTop, main, fb::UIWidgetEventID_EnterBottom);

            setArray(screen->m_Connections, connections);

            std::vector<fb::UINodeData*> nodes = itemsOf(screen->m_Nodes);
            nodes.push_back(list);
            setArray(screen->m_Nodes, nodes);
            g_hudList = { main, mainBinding, templateRow, list, binding, nullptr, nullptr, nullptr, nullptr };
            log("gameplay options: {} with {} rows, {} connections", HUD_LIST_NAME, rows.size(), connections.size() - original);
        }

        // written to TAB_SOURCE by a copy of List_01's SetValue DataSetNode
        void addHudTabs(fb::UIGraphAsset* screen, const fb::WidgetNode* mainTabs, const fb::DataSetNode* setValue)
        {
            fb::WidgetNode* tabs = clone(mainTabs);
            tabs->m_Name = const_cast<char*>("HudFix Tabs");
            tabs->m_InstanceName = const_cast<char*>(HUD_TABS_NAME);
            tabs->m_DataBinding = nullptr;
            tabs->m_FocusIndex = -1;
            const std::vector<fb::UIWidgetProperty> properties =
            {
                { const_cast<char*>("HideOnConsole"), const_cast<char*>("false") },
                { const_cast<char*>("HideOnPC"), const_cast<char*>("false") },
                { const_cast<char*>("Tabs"), const_cast<char*>(TAB_BAR_TABS) },
                { const_cast<char*>("DefaultSelected"), g_tabSelected },
                { const_cast<char*>("TabSpacing"), const_cast<char*>("2") },
            };
            setArray(tabs->m_WidgetProperties, properties);
            for (fb::Array<fb::UINodePort*>* array : { &tabs->m_Inputs, &tabs->m_Outputs })
            {
                std::vector<fb::UINodePort*> items = itemsOf(*array);
                for (fb::UINodePort*& port : items)
                {
                    port = clone(port);
                    port->m_InstanceName = const_cast<char*>(HUD_TABS_NAME);
                }
                setArray(*array, items);
            }

            fb::DataSetNode* setTab = clone(setValue);
            setTab->m_Name = const_cast<char*>("SetHudFixTab");
            setTab->m_In = clone(setValue->m_In);
            setTab->m_Out = clone(setValue->m_Out);
            setTab->m_DataSource.m_DataName = const_cast<char*>(TAB_SOURCE);
            setTab->m_DataSource.m_DataKey = g_tabKey;

            std::vector<fb::UINodeConnection*> connections = itemsOf(screen->m_Connections);
            fb::UINodePort* released = portOf(tabs->m_Outputs, fb::UIWidgetEventID_Released);
            if (!released || connections.empty())
            {
                log("{}: no Released port", HUD_TABS_NAME);
                return;
            }
            fb::UINodeConnection* connection = clone(connections.front());
            connection->m_SourceNode = tabs;
            connection->m_SourcePort = released;
            connection->m_TargetNode = setTab;
            connection->m_TargetPort = setTab->m_In;
            connection->m_NumScreensToPop = 0;
            connections.push_back(connection);
            setArray(screen->m_Connections, connections);

            std::vector<fb::UINodeData*> nodes = itemsOf(screen->m_Nodes);
            nodes.push_back(tabs);
            nodes.push_back(setTab);
            setArray(screen->m_Nodes, nodes);
            g_hudList.mainTabs = mainTabs;
            g_hudList.tabs = tabs;
            g_hudList.setValue = setValue;
            g_hudList.setTab = setTab;
            log("gameplay options: {} with tabs {}", HUD_TABS_NAME, TAB_BAR_TABS);
        }

        void __fastcall hkPartitionLoaded(fb::InternalDatabasePartition* _this, void*)
        {
            oPartitionLoaded(_this);
            patchChatQueue(_this);
            patchReticles(_this);

            if (!_this->m_name)
                return;

            std::string name = _this->m_name;

            for (char& c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (name.find(GAMEPLAY_PAGE) == std::string::npos)
                return;

            fb::UIGraphAsset* screen = nullptr;
            fb::WidgetNode* main = nullptr;
            fb::WidgetNode* mainTabs = nullptr;
            fb::DataSetNode* setValue = nullptr;
            for (fb::DataContainer* object : _this->m_instances)
            {
                const char* type = fb::typeName(object);
                if (!type)
                    continue;

                if (std::strcmp(type, "UIScreenAsset") == 0)
                    screen = static_cast<fb::UIGraphAsset*>(object);
                else if (std::strcmp(type, "WidgetNode") == 0 && std::strcmp(static_cast<fb::WidgetNode*>(object)->m_InstanceName, MAIN_LIST) == 0)
                    main = static_cast<fb::WidgetNode*>(object);
                else if (std::strcmp(type, "WidgetNode") == 0 && std::strcmp(static_cast<fb::WidgetNode*>(object)->m_InstanceName, MAIN_TAB_BAR) == 0)
                    mainTabs = static_cast<fb::WidgetNode*>(object);
                else if (std::strcmp(type, "DataSetNode") == 0 && std::strcmp(static_cast<fb::DataSetNode*>(object)->m_Name, SET_VALUE_NODE) == 0)
                    setValue = static_cast<fb::DataSetNode*>(object);
            }
            if (screen && main)
                addHudList(screen, main);
            if (screen && main && mainTabs && setValue)
                addHudTabs(screen, mainTabs, setValue);
        }
    }

    void linkHudList()
    {
        HudList& h = g_hudList;
        if (!h.list || h.list->m_WidgetAsset == h.main->m_WidgetAsset)
            return;

        h.list->m_WidgetAsset = h.main->m_WidgetAsset;
        if (h.tabs)
        {
            h.tabs->m_WidgetAsset = h.mainTabs->m_WidgetAsset;
            h.setTab->m_DataSource.m_DataCategory = h.setValue->m_DataSource.m_DataCategory;
        }
        h.binding->m_DefaultHighlightedRow = h.mainBinding->m_DefaultHighlightedRow;
        h.binding->m_Visibility = h.mainBinding->m_Visibility;
        for (unsigned int i = 0; i < h.binding->m_NestedLists.size(); ++i)
        {
            fb::NestedList& row = h.binding->m_NestedLists.m_firstElement[i];
            row.m_ListDataSource.m_DataCategory = h.templateRow->m_ListDataSource.m_DataCategory;
            showOnTab(row, g_rows[i], *h.templateRow);
            row.m_DefaultHighlighted = h.templateRow->m_DefaultHighlighted;
        }
        log("{} linked, widget asset {}", HUD_LIST_NAME, static_cast<const void*>(h.list->m_WidgetAsset));
    }

    int highlightedColorSlot(int row)
    {
        for (const Row& r : g_rows)
        {
            if (r.tab != g_tab)
                continue;
            if (row-- == 0)
                return r.kind == Kind::Color ? r.element : -1;
        }
        return -1;
    }

    const char* colorRowName(int slot)
    {
        for (const Row& r : g_rows)
        {
            if (r.kind == Kind::Color && r.element == slot)
                return r.name;
        }
        return "";
    }

    bool colorRowShown(int slot)
    {
        for (const Row& r : g_rows)
        {
            if (r.kind == Kind::Color && r.element == slot)
                return r.tab == g_tab;
        }
        return false;
    }

    void pickColor(ColorSlot slot, int rgb, bool done)
    {
        static bool switched = false; // a pick turned the row to CUSTOM
        switched |= settings().color[slot] != CUSTOM_COLOR;
        settings().customColor[slot] = rgb;
        settings().color[slot] = CUSTOM_COLOR;
        if (!done)
            return;

        if (slot == SightColor)
            applyScopeColor();
        saveSettings();
        // refreshing a row rebuilds the list, its highlight back on the first row: only when the row has to show CUSTOM
        for (const Row& r : g_rows)
        {
            if (switched && r.kind == Kind::Color && r.element == slot)
                g_pickedRowKey = r.key;
        }
        switched = false;
        log("color {} picked {:06X}", static_cast<int>(slot), rgb);
    }

    void updateMenu()
    {
        void* system = *reinterpret_cast<void**>(OFF_g_uiSystem);
        if (g_pickedRowKey && system)
            vfunc::callVFunc<void, OFF_UISystem_refreshData_SLOT>(system, g_pickedRowKey, false);
        g_pickedRowKey = 0;

        if (!g_tabChanged)
            return;

        g_tabChanged = false;
        if (!system)
            return;

        vfunc::callVFunc<void, OFF_UISystem_refreshData_SLOT>(system, g_tabKeys[g_previousTab], false);
        vfunc::callVFunc<void, OFF_UISystem_refreshData_SLOT>(system, g_tabKeys[g_tab], false);
        for (const Row& row : g_rows)
        {
            if (row.tab == g_previousTab || row.tab == g_tab)
                vfunc::callVFunc<void, OFF_UISystem_refreshData_SLOT>(system, row.key, false);
        }
        log("{} page {}", HUD_LIST_NAME, TAB_NAMES[g_tab]);
    }

    void installMenuHooks()
    {
        buildKeys();

        hook(OFF_UIComponentManager_registerComponentDataKeys, hkRegisterKeys, &oRegisterKeys);
        hook(OFF_UISettingsComp_getDataValue, hkGetDataValue, &oGetDataValue);
        hook(OFF_UISettingsComp_setDataValue, hkSetDataValue, &oSetDataValue);
        hook(OFF_InternalDatabasePartition_onPartitionLoaded, hkPartitionLoaded, &oPartitionLoaded);
    }
}
