#pragma once

#include "sdk_ui.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <MinHook.h>

namespace hudfix
{
    enum Element
    {
        Minimap, // map, its background and compass
        MinimapIcons, // icons drawn on the map (UIMinimap::m_resolutionScale)
        SquadList, // squad member names used by UIMinimap
		Objectives, // tickets and objective bar used by UIMinimap
        Crosshair,
        KillLog,
        Nametags,
        AmmoHealth,
        Chat,
        NametagsEnemy,
        HealthBars,
        HealthBarsEnemy,
        ReviveIcons, // man down and revive icons
        Flags, // ClientMapMarkerEntity
        CommoRose,
        HitMarkers, // hit markers of a hit
        HeadshotMarkers, // of a headshot
        KillMarkers, // hs included
        Scope,
        VehicleCrosshair, // the 1st person vehicle crosshair, drawn in the cockpit HUD
        VehicleHud, // the other vehicle widgets, each grown around its own origin, parts ActionScript moves (horizon, velocity vector) keep their offsets
        ChaseCrosshair, // the 3rd person (chase cam) vehicle crosshair
        ScoreMessages, // the score popup in the middle like enemy killed x points...
        ElementCount
    };

    enum ColorSlot
    {
        HitColor,
        HeadshotColor,
        KillColor,
        SightColor,
        CrosshairColor,
        VehicleCrosshairColor,
        VehicleHudColor,
        ChaseCrosshairColor,
        ColorSlotCount
    };

    struct Settings
    {
        bool autoScale = true; // s = min(w/1280, h/720)
        int sightZoom = 0;
        int vehicleBrightness = 100; // percent of the stock vehicle HUD's brightness a vehicle color gets
        bool removeTicketPanel = false; // no panel behind the tickets: on foot it ends at the minimap, the air vehicles' is hidden
        int scale = 100; // percent, used when autoScale is off
        int element[ElementCount];
        int opacity[ElementCount];
        int scoped[ElementCount]; // percent of the opacity while the local soldier is zoomed
        int color[ColorSlotCount]; // 0 keeps the game's colors, CUSTOM_COLOR the picked customColor (older profiles: a COLORS index)
        int customColor[ColorSlotCount]; // 0xRRGGBB from the color picker

        Settings()
        {
            std::fill(std::begin(element), std::end(element), 100);
            std::fill(std::begin(opacity), std::end(opacity), 100);
            std::fill(std::begin(scoped), std::end(scoped), 100);
            std::fill(std::begin(color), std::end(color), 0);
            std::fill(std::begin(customColor), std::end(customColor), 0xFFFFFF);
        }
    };

    // multiplies the widget's own colors ONLY
    struct Color
    {
        const char* name;
        float r, g, b;
    };
    constexpr Color COLORS[] =
    {
        { "DEFAULT", 1.0f, 1.0f, 1.0f },
        { "RED", 1.0f, 0.2f, 0.2f },
        { "GREEN", 0.2f, 1.0f, 0.2f },
        { "BLUE", 0.25f, 0.5f, 1.0f },
        { "YELLOW", 1.0f, 1.0f, 0.2f },
        { "CYAN", 0.2f, 1.0f, 1.0f },
        { "MAGENTA", 1.0f, 0.2f, 1.0f },
        { "ORANGE", 1.0f, 0.55f, 0.1f },
        { "PINK", 1.0f, 0.5f, 0.75f },
        { "PURPLE", 0.6f, 0.3f, 1.0f },
    };
    constexpr int COLOR_COUNT = static_cast<int>(std::size(COLORS));
    constexpr int CUSTOM_COLOR = COLOR_COUNT; // past the old COLORS presets, which profiles may still hold
    // the element whose opacity the color picker also sets, ElementCount for none (the scope has no opacity)
    constexpr Element COLOR_ELEMENTS[ColorSlotCount] = { HitMarkers, HeadshotMarkers, KillMarkers, ElementCount, Crosshair, VehicleCrosshair, VehicleHud, ChaseCrosshair };

    // what aiming does to the fov
    enum SightZoom
    {
        SightZoomGame, // each zoom level's own fixed fov
        SightZoomScaled, // the zoom levels' fov times the fov slider's multiplier
        SightZoomOff, // irons, red dots and 1x sights keep the hipfire fov, scopes zoom as in the game
        SightZoomCount
    };
    constexpr const char* SIGHT_ZOOM_NAMES[SightZoomCount] = { "GAME", "SCALED", "OFF" };

    constexpr int VEHICLE_BRIGHTNESS_MIN = 50;
    constexpr int VEHICLE_BRIGHTNESS_MAX = 200;

    // hue in degrees, saturation and value of an rgb triple (any range), useed for tint
    inline void hsv(const float (&rgb)[3], float& h, float& s, float& v)
    {
        v = (std::max)({ rgb[0], rgb[1], rgb[2] });
        const float low = (std::min)({ rgb[0], rgb[1], rgb[2] });
        const float range = v - low;
        s = v > 0.0f ? range / v : 0.0f;
        h = 0.0f;
        if (range <= 0.0f)
            return;
        if (v == rgb[0])
            h = (rgb[1] - rgb[2]) / range;
        else if (v == rgb[1])
            h = 2.0f + (rgb[2] - rgb[0]) / range;
        else
            h = 4.0f + (rgb[0] - rgb[1]) / range;
        h *= 60.0f;
        if (h < 0.0f)
            h += 360.0f;
    }


    // UI job thread
    Settings& settings();
    void loadSettings();
    void saveSettings();

    float elementFactor(Element element);
    float elementOpacity(Element element);
    const Color* slotColor(ColorSlot slot); // nullptr for the game's colors

    struct WidgetGroup
    {
        Element element;
        Element layout;
    };
    bool widgetGroup(const char* assetName, WidgetGroup& out);

    constexpr const char* HUD_LIST_NAME = "HudFixList";
    constexpr const char* HUD_TABS_NAME = "HudFixTabs"; // the list's sub-tabs, a TabBar_01 copy above it
    void linkHudList();
    void updateMenu(); // UI job, every frame
    int highlightedColorSlot(int row); // the ColorSlot of the HUD list's row at this index of its page, -1 if none
    const char* colorRowName(int slot); // the label of the slot's COLOR row
    bool colorRowShown(int slot); // the slot's COLOR row is on the HUD list's current page
    void pickColor(ColorSlot slot, int rgb, bool done); // done saves it, once the mouse is released

    float autoScale();
    float currentScale(); // this UI frame's s

    void log(const std::string& line);
    template <typename... Args>
    void log(std::format_string<Args...> fmt, Args&&... args)
    {
        log(std::format(fmt, std::forward<Args>(args)...));
    }

    inline void hook(uintptr_t target, void* detour, void* original)
    {
        MH_CreateHook(reinterpret_cast<LPVOID>(target), reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(original));
    }

    void installScaleHooks();
    void installWidgetHooks();
    void installMenuHooks();
    void installSettingsHooks();
    void installPerfHooks();
    void installGameplayHooks();
    void installScopeHooks();
    void installInputHooks();
    void scanCockpitShaders();
    void holdCockpitShaders();
    std::filesystem::path documentsDir();
    bool cockpitNeutral();
    void rebuildCockpitShaders();
    void holdKeyboard();
    void releaseKeyboard();
    void patchChatQueue(fb::InternalDatabasePartition* partition);
    void patchReticles(fb::InternalDatabasePartition* partition);
    void applyScopeColor();
    void applyScopeSize();
    void updateScopeColor();
    void forgetZoomLevels(fb::InternalDatabasePartition* partition);
    void applyZoomFov();
}
