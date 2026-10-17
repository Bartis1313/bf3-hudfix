#include "hudfix.h"

#include <ShlObj.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace hudfix
{
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
        constexpr const char* SIGHT_ZOOM_KEY = "HudFixSightZoom";
        constexpr const char* UNIFORM_AIMING_KEY = "HudFixUniformAiming";
        constexpr const char* ZOOM_SENSITIVITY_KEY = "HudFixZoomSensitivity";
        constexpr const char* UNIFORM_AIMING_COEFFICIENT_KEY = "HudFixUniformAimingCoefficient";
        constexpr const char* UNIFORM_SIGHTS_KEY = "HudFixUniformSights";
        constexpr const char* VEHICLE_BRIGHTNESS_KEY = "HudFixVehicleBrightness";
        constexpr const char* TICKET_PANEL_KEY = "HudFixRemoveTicketPanel";
        constexpr const char* NEUTRAL_COLORS_KEY = "HudFixNeutralColors";
        constexpr const char* COLOR_GRADING_KEY = "HudFixNoColorGrading";
        constexpr const char* ELEMENT_KEYS[ElementCount] = 
        { 
            "HudFixMinimap",
            "HudFixMinimapIcons",
            "HudFixSquadList",
            "HudFixObjectives",
            "HudFixCrosshair",
            "HudFixKillLog",
            "HudFixNametags",
            "HudFixAmmoHealth",
            "HudFixChat",
            "HudFixNametagsEnemy",
            "HudFixHealthBars",
            "HudFixHealthBarsEnemy",
            "HudFixReviveIcons",
            "HudFixFlags",
            "HudFixCommoRose",
            "HudFixHitMarkers",
            "HudFixHeadshotMarkers",
            "HudFixKillMarkers",
            "HudFixScopeSize",
            "HudFixVehicleCrosshair",
            "HudFixVehicleHud",
            "HudFixChaseCrosshair",
            "HudFixScoreMessages",
            "HudFixAwards"
        };
        constexpr const char* OPACITY_KEYS[ElementCount] =
        {
            "HudFixMinimapBackgroundOpacity",
            "HudFixMinimapIconsOpacity",
            "HudFixSquadListOpacity",
            "HudFixObjectivesOpacity",
            "HudFixCrosshairOpacity",
            "HudFixKillLogOpacity",
            "HudFixNametagsOpacity",
            "HudFixAmmoHealthOpacity",
            "HudFixChatOpacity",
            "HudFixNametagsEnemyOpacity",
            "HudFixHealthBarsOpacity",
            "HudFixHealthBarsEnemyOpacity",
            "HudFixReviveIconsOpacity",
            "HudFixFlagsOpacity",
            "HudFixCommoRoseOpacity",
            "HudFixHitMarkersOpacity",
            "HudFixHeadshotMarkersOpacity",
            "HudFixKillMarkersOpacity",
            nullptr, // the scope has no "opacity", it's just bloom, prefer untouched
            "HudFixVehicleCrosshairOpacity",
            "HudFixVehicleHudOpacity",
            "HudFixChaseCrosshairOpacity",
            "HudFixScoreMessagesOpacity",
            "HudFixAwardsOpacity"
        };
        constexpr const char* SCOPED_KEYS[ElementCount] =
        {
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            "HudFixNametagsScoped",
            nullptr,
            nullptr,
            "HudFixNametagsEnemyScoped",
            "HudFixHealthBarsScoped",
            "HudFixHealthBarsEnemyScoped",
            "HudFixReviveIconsScoped",
            "HudFixFlagsScoped",
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr
        };
        constexpr const char* COLOR_KEYS[ColorSlotCount] =
        {
            "HudFixHitMarkersColor",
            "HudFixHeadshotMarkersColor",
            "HudFixKillMarkersColor",
            "HudFixScopeColor",
            "HudFixCrosshairColor",
            "HudFixVehicleCrosshairColor",
            "HudFixVehicleHudColor",
            "HudFixChaseCrosshairColor"
        };

        constexpr const char* CUSTOM_COLOR_KEYS[ColorSlotCount] =
        {
            "HudFixHitMarkersRGB",
            "HudFixHeadshotMarkersRGB",
            "HudFixKillMarkersRGB",
            "HudFixScopeRGB",
            "HudFixCrosshairRGB",
            "HudFixVehicleCrosshairRGB",
            "HudFixVehicleHudRGB",
            "HudFixChaseCrosshairRGB"
        };

        struct AssetGroup
        {
            const char* asset;
            WidgetGroup group;
        };

        // widgets of a shared asset grouped apart by their instance in the screen
        struct InstanceGroup
        {
            const char* asset;
            const char* instance;
            WidgetGroup group;
        };
        constexpr InstanceGroup UI_INSTANCES[] =
        {
            { "UI/Assets/HudMessage", "HudMessage_04", { Awards, Awards } }, // RewardMessage: ribbons and medals
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
            { "UI/Assets/Hitindicator", { HitMarkers, HitMarkers } },
            { "UI/Assets/Ammo", { AmmoHealth, AmmoHealth } },
            { "UI/Assets/Health", { AmmoHealth, AmmoHealth } },
            { "UI/Assets/VehicleHealth", { AmmoHealth, AmmoHealth } },
            { "UI/Assets/PassangerList", { AmmoHealth, AmmoHealth } },
            { "UI/Assets/ChatLog", { Chat, Minimap } },
            { "UI/Assets/ScoreAggregator", { ScoreMessages, ScoreMessages } },
            { "UI/Assets/HudMessage", { ScoreMessages, ScoreMessages } }, // the score events in the middle ("ENEMY KILLED 100")
            // the vehicle screens' widgets (UI/Flow/Screen/Vehicle)
            { "UI/Assets/VehicleCrosshair", { VehicleCrosshair, VehicleCrosshair } },
            { "UI/Assets/Tank3PCrosshair", { ChaseCrosshair, ChaseCrosshair } },
            { "UI/Assets/Tank3PCrosshairGrade", { ChaseCrosshair, ChaseCrosshair } },
            { "UI/Assets/CannonPredictiveSight", { VehicleCrosshair, VehicleCrosshair } },
            { "UI/Assets/VehicleHudText", { VehicleHud, VehicleHud } },
            { "UI/Assets/VehicleHudWarnings", { VehicleHud, VehicleHud } },
            { "UI/Assets/VehicleWeaponName", { VehicleHud, VehicleHud } },
            { "UI/Assets/VehicleWeaponUnlockDisplay", { VehicleHud, VehicleHud } },
            { "UI/Assets/VehicleGunnerWeaponElevation", { VehicleHud, VehicleHud } },
            { "UI/Assets/VehicleStaticArt", { VehicleHud, VehicleHud } },
            { "UI/Assets/VehicleWeaponReload", { VehicleHud, VehicleHud } },
            { "UI/Assets/VehicleMissileDisplay", { VehicleHud, VehicleHud } },
            { "UI/Assets/TurretDisplay", { VehicleHud, VehicleHud } },
            { "UI/Assets/HeliGunnerDirectionDisplay", { VehicleHud, VehicleHud } },
            { "UI/Assets/BankAngle", { VehicleHud, VehicleHud } },
            { "UI/Assets/VelocityVector", { VehicleHud, VehicleHud } },
            { "UI/Assets/TrimBall", { VehicleHud, VehicleHud } },
            { "UI/Assets/RadarAltitudeMeter", { VehicleHud, VehicleHud } },
            { "UI/Assets/RadarAltitudeDisplay", { VehicleHud, VehicleHud } },
            { "UI/Assets/ClimbRateMeter", { VehicleHud, VehicleHud } },
            { "UI/Assets/ArtificialHorizon", { VehicleHud, VehicleHud } },
            { "UI/Assets/ArtificialHorizonHelicopter", { VehicleHud, VehicleHud } },
            { "UI/Assets/JetTrackingAndLockingManager", { VehicleCrosshair, VehicleCrosshair } }, // the heat seeker's lock circle, an aim
        };

        Settings g_settings;
        std::mutex g_logLock;

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

            sync(SIGHT_ZOOM_KEY, g_settings.sightZoom);
            int uniformAiming = g_settings.uniformAiming ? 1 : 0;
            sync(UNIFORM_AIMING_KEY, uniformAiming);
            g_settings.uniformAiming = uniformAiming != 0;
            sync(UNIFORM_AIMING_COEFFICIENT_KEY, g_settings.uniformAimingCoefficient);
            g_settings.uniformAimingCoefficient = std::clamp(g_settings.uniformAimingCoefficient, UNIFORM_AIMING_COEFFICIENTS[0],
                UNIFORM_AIMING_COEFFICIENTS[UNIFORM_AIMING_COEFFICIENT_COUNT - 1]);
            int uniformSights = g_settings.uniformSights ? 1 : 0;
            sync(UNIFORM_SIGHTS_KEY, uniformSights);
            g_settings.uniformSights = uniformSights != 0;
            sync(ZOOM_SENSITIVITY_KEY, g_settings.zoomSensitivity);
            g_settings.zoomSensitivity = std::clamp(g_settings.zoomSensitivity, ZOOM_SENSITIVITY_MIN, ZOOM_SENSITIVITY_MAX);
            g_settings.sightZoom = std::clamp(g_settings.sightZoom, 0, SightZoomCount - 1);
            sync(VEHICLE_BRIGHTNESS_KEY, g_settings.vehicleBrightness);
            g_settings.vehicleBrightness = std::clamp(g_settings.vehicleBrightness, VEHICLE_BRIGHTNESS_MIN, VEHICLE_BRIGHTNESS_MAX);
            int removeTicketPanel = g_settings.removeTicketPanel ? 1 : 0;
            sync(TICKET_PANEL_KEY, removeTicketPanel);
            g_settings.removeTicketPanel = removeTicketPanel != 0;
            int neutralColors = g_settings.neutralColors ? 1 : 0;
            sync(NEUTRAL_COLORS_KEY, neutralColors);
            g_settings.neutralColors = neutralColors != 0;
            int noColorGrading = g_settings.noColorGrading ? 1 : 0;
            sync(COLOR_GRADING_KEY, noColorGrading);
            g_settings.noColorGrading = noColorGrading != 0;

            if (g_settings.scale <= 0) // the slider builds stored "fit" as 0
                g_settings.scale = 100;

            for (int i = 0; i < ElementCount; ++i)
            {
                sync(ELEMENT_KEYS[i], g_settings.element[i]);
                if (i == Scope)
                    g_settings.element[i] = std::clamp(g_settings.element[i], SCOPE_SIZE_MIN, SCOPE_SIZE_MAX);
                if (OPACITY_KEYS[i])
                    sync(OPACITY_KEYS[i], g_settings.opacity[i]);
                if (SCOPED_KEYS[i])
                    sync(SCOPED_KEYS[i], g_settings.scoped[i]);
            }
            for (int i = 0; i < ColorSlotCount; ++i)
            {
                sync(COLOR_KEYS[i], g_settings.color[i]);
                sync(CUSTOM_COLOR_KEYS[i], g_settings.customColor[i]); // 24 bits, exact in the float

                // the rows used to pick from COLORS: such a pick is now that color as CUSTOM
                const int preset = g_settings.color[i];
                if (preset > 0 && preset < COLOR_COUNT)
                {
                    const Color& c = COLORS[preset];
                    g_settings.customColor[i] = std::lround(c.r * 255.0f) << 16 | std::lround(c.g * 255.0f) << 8 | std::lround(c.b * 255.0f);
                    g_settings.color[i] = CUSTOM_COLOR;
                }
                else if (preset != 0)
                {
                    g_settings.color[i] = CUSTOM_COLOR;
                }
            }
            applyScopeColor();
            applyScopeSize();
            applyZoomFov();
            applyVisuals();
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
        set(group, SIGHT_ZOOM_KEY, static_cast<float>(g_settings.sightZoom));
        set(group, UNIFORM_AIMING_KEY, g_settings.uniformAiming ? 1.0f : 0.0f);
        set(group, UNIFORM_AIMING_COEFFICIENT_KEY, static_cast<float>(g_settings.uniformAimingCoefficient));
        set(group, UNIFORM_SIGHTS_KEY, g_settings.uniformSights ? 1.0f : 0.0f);
        set(group, ZOOM_SENSITIVITY_KEY, static_cast<float>(g_settings.zoomSensitivity));
        set(group, VEHICLE_BRIGHTNESS_KEY, static_cast<float>(g_settings.vehicleBrightness));
        set(group, TICKET_PANEL_KEY, g_settings.removeTicketPanel ? 1.0f : 0.0f);
        set(group, NEUTRAL_COLORS_KEY, g_settings.neutralColors ? 1.0f : 0.0f);
        set(group, COLOR_GRADING_KEY, g_settings.noColorGrading ? 1.0f : 0.0f);

        for (int i = 0; i < ElementCount; ++i)
        {
            set(group, ELEMENT_KEYS[i], static_cast<float>(g_settings.element[i]));
            if (OPACITY_KEYS[i])
                set(group, OPACITY_KEYS[i], static_cast<float>(g_settings.opacity[i]));
            if (SCOPED_KEYS[i])
                set(group, SCOPED_KEYS[i], static_cast<float>(g_settings.scoped[i]));
        }
        for (int i = 0; i < ColorSlotCount; ++i)
        {
            set(group, COLOR_KEYS[i], static_cast<float>(g_settings.color[i]));
            set(group, CUSTOM_COLOR_KEYS[i], static_cast<float>(g_settings.customColor[i]));
        }

        reinterpret_cast<GroupChangedFn>(OFF_ProfileOptions_groupChanged)(game, GAMEPLAY_GROUP);
    }

    float elementFactor(Element element)
    {
        return g_settings.element[element] / 100.0f;
    }

    float elementOpacity(Element element)
    {
        return g_settings.opacity[element] / 100.0f;
    }

    const Color* slotColor(ColorSlot slot)
    {
        const int index = g_settings.color[slot];
        if (index == CUSTOM_COLOR)
        {
            static Color custom[ColorSlotCount];
            const int rgb = g_settings.customColor[slot];
            custom[slot] = { "CUSTOM", ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f };
            return &custom[slot];
        }
        return index > 0 && index < COLOR_COUNT ? &COLORS[index] : nullptr;
    }

    bool widgetGroup(const char* assetName, WidgetGroup& out, const char* instanceName)
    {
        for (const InstanceGroup& entry : UI_INSTANCES)
        {
            if (instanceName && equalsNoCase(entry.asset, assetName) && equalsNoCase(entry.instance, instanceName))
            {
                out = entry.group;
                return true;
            }
        }
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
