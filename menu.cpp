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
            Scale,
            Element
        };

        struct Row
        {
            const char* source; // UISettingsComp data source name
            const char* name;
            Kind kind;
            int element;
            int key; // -1 = not special
        };

        std::array<Row, 2 + ElementCount> g_rows
        {
        {
            { "HudFixAuto", "AUTO HUD SCALE", Kind::Auto, -1 },
            { "HudFixScale", "HUD SCALE", Kind::Scale, -1 },
            { "HudFixMinimap", "MINIMAP SIZE", Kind::Element, Minimap },
            { "HudFixMinimapIcons", "MINIMAP ICON SIZE", Kind::Element, MinimapIcons },
            { "HudFixSquadList", "SQUAD LIST SIZE", Kind::Element, SquadList },
            { "HudFixObjectives", "TICKETS SIZE", Kind::Element, Objectives },
            { "HudFixCrosshair", "CROSSHAIR SIZE", Kind::Element, Crosshair },
            { "HudFixKillLog", "KILL LOG SIZE", Kind::Element, KillLog },
            { "HudFixNametags", "NAMETAG SIZE", Kind::Element, Nametags },
            { "HudFixAmmoHealth", "AMMO AND HEALTH SIZE", Kind::Element, AmmoHealth },
        }
        };

        constexpr int SCALE_MIN = 100;
        constexpr int ELEMENT_MIN = 50;
        constexpr int ELEMENT_MAX = 300;
        constexpr int ELEMENT_STEP = 10;

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
            const char* names[2 + ElementCount];
        };

        SourceArray g_sources{ };
        FakeComponentData g_fakeData{ };

        struct HudList
        {
            const fb::WidgetNode* main;
            const fb::UINestedListDataBinding* mainBinding;
            const fb::NestedList* templateRow;
            fb::WidgetNode* list;
            fb::UINestedListDataBinding* binding;
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
            for (size_t i = 0; i < g_rows.size(); ++i)
            {
                g_rows[i].key = dataKey(g_rows[i].source);
                g_sources.names[i] = g_rows[i].source;
            }
            g_sources.count = static_cast<unsigned int>(g_rows.size());
        }

        int itemCount(const Row& row)
        {
            switch (row.kind)
            {
            case Kind::Auto:
                return 2;
            case Kind::Scale:
                return (std::max)(static_cast<int>(std::floor(autoScale() * 100.0f + 1e-3f)) - SCALE_MIN, 0) / ELEMENT_STEP + 1;
            default:
                return (ELEMENT_MAX - ELEMENT_MIN) / ELEMENT_STEP + 1;
            }
        }

        int percentOfItem(const Row& row, int index)
        {
            return (row.kind == Kind::Scale ? SCALE_MIN : ELEMENT_MIN) + index * ELEMENT_STEP;
        }

        int itemOfSetting(const Row& row)
        {
            switch (row.kind)
            {
            case Kind::Auto:
                return settings().autoScale ? 1 : 0;
            case Kind::Scale:
                return std::clamp((settings().scale - SCALE_MIN + ELEMENT_STEP / 2) / ELEMENT_STEP, 0, itemCount(row) - 1);
            default:
                return std::clamp((settings().element[row.element] - ELEMENT_MIN + ELEMENT_STEP / 2) / ELEMENT_STEP, 0, itemCount(row) - 1);
            }
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
        void fillRow(const Row& row, fb::UIDataValue* value)
        {
            // this is just to not get alloc issues
            thread_local std::unique_ptr<char[]> scratch(new char[0x10000]);
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
                if (row.kind == Kind::Auto)
                    std::snprintf(label, sizeof(label), "%s", i ? "ID_M_OPTIONS_YES" : "ID_M_OPTIONS_NO");
                else
                    std::snprintf(label, sizeof(label), "%d%%", percentOfItem(row, i));

                std::snprintf(index, sizeof(index), "%d", i);
                reinterpret_cast<SetEnumItemFn>(OFF_UIDataValue_setEnumItem)(&items, i, label, index);
            }

            setInt(value, "key", row.key);
            setInt(value, "Highlighted", itemOfSetting(row));
            reinterpret_cast<SetMemberFn>(OFF_UIDataValue_setMember)(value, "SubItems", &items);
            reinterpret_cast<DestroyFn>(OFF_UIDataValue_destroy)(&items);
        }

        int selectedIndex(const fb::UIDataValue* value)
        {
            if (value->m_type == 3)
                return static_cast<int>(value->m_number);
            if (value->m_type == 4)
                return value->m_int;
            return value->m_string ? std::atoi(value->m_string) : 0;
        }

        void applyRow(const Row& row, const fb::UIDataValue* value)
        {
            const int index = std::clamp(selectedIndex(value), 0, itemCount(row) - 1);
            switch (row.kind)
            {
            case Kind::Auto:
                settings().autoScale = index == 1;
                break;
            case Kind::Scale:
                settings().scale = percentOfItem(row, index);
                break;
            case Kind::Element:
                settings().element[row.element] = percentOfItem(row, index);
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
            log("registered {} option keys", g_rows.size());
        }

        bool __fastcall hkGetDataValue(void* _this, void*, int key, fb::UIDataValue* value)
        {
            if (const Row* row = findRow(key))
            {
                fillRow(*row, value);
                return true;
            }
            return oGetDataValue(_this, key, value);
        }

        bool __fastcall hkSetDataValue(void* _this, void*, int key, fb::UIDataValue* value)
        {
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
                auto* id = reinterpret_cast<unsigned int*>(block);
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
            auto* header = static_cast<fb::ArrayHeader*>(allocate(sizeof(fb::ArrayHeader) + items.size() * sizeof(T)));
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
                rows.push_back(item);
            }
            setArray(binding->m_NestedLists, rows);
            binding->m_ListIndex = 5; // hud list index
            binding->m_KeepScrollOffset = true;
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
            g_hudList = { main, mainBinding, templateRow, list, binding };
            log("gameplay options: {} with {} rows, {} connections", HUD_LIST_NAME, rows.size(), connections.size() - original);
        }

        void __fastcall hkPartitionLoaded(fb::InternalDatabasePartition* _this, void*)
        {
            oPartitionLoaded(_this);

            if (!_this->m_name)
                return;

            std::string name = _this->m_name;

            for (char& c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            if (name.find(GAMEPLAY_PAGE) == std::string::npos)
                return;

            fb::UIGraphAsset* screen = nullptr;
            fb::WidgetNode* main = nullptr;
            for (fb::DataContainer* object : _this->m_instances)
            {
                const char* type = fb::typeName(object);
                if (!type)
                    continue;

                if (std::strcmp(type, "UIScreenAsset") == 0)
                    screen = static_cast<fb::UIGraphAsset*>(object);
                else if (std::strcmp(type, "WidgetNode") == 0 && std::strcmp(static_cast<fb::WidgetNode*>(object)->m_InstanceName, MAIN_LIST) == 0)
                    main = static_cast<fb::WidgetNode*>(object);
            }
            if (screen && main)
                addHudList(screen, main);
        }
    }

    void linkHudList()
    {
        HudList& h = g_hudList;
        if (!h.list || h.list->m_WidgetAsset == h.main->m_WidgetAsset)
            return;

        h.list->m_WidgetAsset = h.main->m_WidgetAsset;
        h.binding->m_DefaultHighlightedRow = h.mainBinding->m_DefaultHighlightedRow;
        h.binding->m_Visibility = h.mainBinding->m_Visibility;
        for (unsigned int i = 0; i < h.binding->m_NestedLists.size(); ++i)
        {
            fb::NestedList& row = h.binding->m_NestedLists.m_firstElement[i];
            row.m_ListDataSource.m_DataCategory = h.templateRow->m_ListDataSource.m_DataCategory;
            row.m_DynamicShowList = h.templateRow->m_DynamicShowList;
            row.m_DefaultHighlighted = h.templateRow->m_DefaultHighlighted;
        }
        log("{} linked, widget asset {}", HUD_LIST_NAME, static_cast<const void*>(h.list->m_WidgetAsset));
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
