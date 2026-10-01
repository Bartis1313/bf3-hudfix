#pragma once

#include "sdk_ui.h"

#include <algorithm>
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
        ElementCount
    };

    struct Settings
    {
        bool autoScale = true; // s = min(w/1280, h/720)
        int scale = 100; // percent, used when autoScale is off
        int element[ElementCount];
        int opacity[ElementCount];
        int scoped[ElementCount]; // percent of the opacity while the local soldier is zoomed

        Settings()
        {
            std::fill(std::begin(element), std::end(element), 100);
            std::fill(std::begin(opacity), std::end(opacity), 100);
            std::fill(std::begin(scoped), std::end(scoped), 100);
        }
    };

    // UI job thread
    Settings& settings();
    void loadSettings();
    void saveSettings();

    float elementFactor(Element element);
    float elementOpacity(Element element);

    struct WidgetGroup
    {
        Element element;
        Element layout;
    };
    bool widgetGroup(const char* assetName, WidgetGroup& out);

    constexpr const char* HUD_LIST_NAME = "HudFixList";
    void linkHudList();

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
    void patchChatQueue(fb::InternalDatabasePartition* partition);
}
