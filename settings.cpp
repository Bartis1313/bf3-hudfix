#include "hudfix.h"

#include <ShlObj.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace hudfix
{
    namespace
    {
        using SetFloatFn = void(__thiscall*)(fb::SettingsGroup* group, const char* name, float value);
        using GetFloatFn = float(__thiscall*)(fb::SettingsGroup* group, const char* name, float fallback);
        using GroupChangedFn = void(__thiscall*)(fb::ProfileOptions* settings, int group);
        using AssignGroupFn = int(__thiscall*)(fb::SettingsGroup* group, const fb::SettingsGroup* source);
        using LoadProfileFn = int(__thiscall*)(fb::ProfileOptions* settings, const void* data, int size, const void* text, int textSize);
        using ResetProfileFn = void(__thiscall*)(fb::ProfileOptions* settings);

        AssignGroupFn oAssignGroup = nullptr;
        LoadProfileFn oLoadProfile = nullptr;
        ResetProfileFn oResetProfile = nullptr;

        constexpr int GAMEPLAY_GROUP = 4;
        constexpr const char* SCALE_AUTO_KEY = "HudFixAuto";
        constexpr const char* SCALE_KEY = "HudFixScale";
        constexpr const char* ELEMENT_KEYS[ElementCount] = 
        { 
            "HudFixMinimap",
            "HudFixMinimapIcons",
            "HudFixSquadList",
            "HudFixObjectives",
            "HudFixCrosshair",
            "HudFixKillLog",
            "HudFixNametags",
            "HudFixAmmoHealth"
        };

        struct AssetGroup
        {
            const char* asset;
            WidgetGroup group;
        };

        constexpr AssetGroup UI_ASSETS[] = 
        {
            { "UI/Assets/Minimap", { Minimap, Minimap } },
            { "UI/Assets/HudBackgroundWidget", { Minimap, Minimap } },
            { "UI/Assets/Compass", { Minimap, Minimap } },
            { "UI/Assets/SquadList", { SquadList, Minimap } },
            { "UI/Assets/TicketCounter", { Objectives, Minimap } },
            { "UI/Assets/ObjectiveBar", { Objectives, Minimap } },
            { "UI/Assets/Crosshair", { Crosshair, Crosshair } },
            { "UI/Assets/Ammo", { AmmoHealth, AmmoHealth } },
            { "UI/Assets/Health", { AmmoHealth, AmmoHealth } },
            { "UI/Assets/VehicleHealth", { AmmoHealth, AmmoHealth } },
            { "UI/Assets/PassangerList", { AmmoHealth, AmmoHealth } },
        };

        Settings g_settings;
        std::mutex g_logLock;

        // prob settingsmanager has path, but whatever...
        std::filesystem::path documentsDir()
        {
            PWSTR path = nullptr;
            std::filesystem::path result = ".";
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path)))
                result = std::filesystem::path(path) / "Battlefield 3";
            CoTaskMemFree(path);
            return result;
        }

        bool equalsNoCase(const char* a, const char* b)
        {
            return _stricmp(a, b) == 0;
        }

        void syncProfile(fb::ProfileOptions* game)
        {
            fb::SettingsGroup* group = &game->m_groups[GAMEPLAY_GROUP];
            const auto sync = [group](const char* key, int& value)
            {
                const float stored = reinterpret_cast<GetFloatFn>(OFF_SettingsGroup_getFloat)(group, key, -1.0f);
                if (stored < 0.0f)
                    reinterpret_cast<SetFloatFn>(OFF_SettingsGroup_setFloat)(group, key, static_cast<float>(value));
                else
                    value = static_cast<int>(std::lround(stored));
            };

            int autoScale = g_settings.autoScale ? 1 : 0;
            sync(SCALE_AUTO_KEY, autoScale);
            g_settings.autoScale = autoScale != 0;
            sync(SCALE_KEY, g_settings.scale);

            if (g_settings.scale <= 0) // the slider builds stored "fit" as 0
                g_settings.scale = 100;

            for (int i = 0; i < ElementCount; ++i)
                sync(ELEMENT_KEYS[i], g_settings.element[i]);
        }

        // options menu revert / restore defaults copy a whole bank over the live group
        int __fastcall hkAssignGroup(fb::SettingsGroup* group, void*, const fb::SettingsGroup* source)
        {
            const int result = oAssignGroup(group, source);
            fb::ProfileOptions* game = fb::ProfileOptions::GetInstance();
            if (game && group == &game->m_groups[GAMEPLAY_GROUP])
                syncProfile(game);
            return result;
        }

        int __fastcall hkLoadProfile(fb::ProfileOptions* game, void*, const void* data, int size, const void* text, int textSize)
        {
            const int result = oLoadProfile(game, data, size, text, textSize);
            syncProfile(game);
            return result;
        }

        void __fastcall hkResetProfile(fb::ProfileOptions* game, void*)
        {
            oResetProfile(game);
            syncProfile(game);
        }
    }

    Settings& settings()
    {
        return g_settings;
    }

    // the profile may be up before our hooks are
    void loadSettings()
    {
        if (fb::ProfileOptions* game = fb::ProfileOptions::GetInstance())
            syncProfile(game);
    }

    void saveSettings()
    {
        fb::ProfileOptions* game = fb::ProfileOptions::GetInstance();
        if (!game)
            return;

        fb::SettingsGroup* group = &game->m_groups[GAMEPLAY_GROUP];
        const auto set = reinterpret_cast<SetFloatFn>(OFF_SettingsGroup_setFloat);
        set(group, SCALE_AUTO_KEY, g_settings.autoScale ? 1.0f : 0.0f);
        set(group, SCALE_KEY, static_cast<float>(g_settings.scale));

        for (int i = 0; i < ElementCount; ++i)
            set(group, ELEMENT_KEYS[i], static_cast<float>(g_settings.element[i]));

        reinterpret_cast<GroupChangedFn>(OFF_ProfileOptions_groupChanged)(game, GAMEPLAY_GROUP);
    }

    float elementFactor(Element element)
    {
        return g_settings.element[element] / 100.0f;
    }

    bool widgetGroup(const char* assetName, WidgetGroup& out)
    {
        for (const AssetGroup& entry : UI_ASSETS)
        {
            if (equalsNoCase(entry.asset, assetName))
            {
                out = entry.group;
                return true;
            }
        }
        return false;
    }

    void installSettingsHooks()
    {
        hook(OFF_SettingsGroup_assign, hkAssignGroup, &oAssignGroup);
        hook(OFF_ProfileOptions_loadProfile, hkLoadProfile, &oLoadProfile);
        hook(OFF_ProfileOptions_resetProfile, hkResetProfile, &oResetProfile);
    }

    void log(const std::string& line)
    {
        std::lock_guard lock(g_logLock);
        static std::ofstream file(documentsDir() / "hudfix.log", std::ios::trunc);
        file << line << std::endl;
    }
}
