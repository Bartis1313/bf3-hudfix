#pragma once

#include "sdk_ui.h"

#include <format>
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
        ElementCount
    };

    struct Settings
    {
        bool autoScale = true; // s = min(w/1280, h/720)
        int scale = 100; // percent, used when autoScale is off
        int element[ElementCount] = { 100, 100, 100, 100, 100, 100, 100, 100, 100 };
        int opacity[ElementCount] = { 100, 100, 100, 100, 100, 100, 100, 100, 100 };
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
    void patchChatQueue(fb::InternalDatabasePartition* partition);
}
