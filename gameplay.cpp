#include "hudfix.h"

#include <mutex>
#include <unordered_map>

namespace hudfix
{
    namespace
    {
        using ReviveEnterFn = void(__thiscall*)(fb::HealthModuleState* state);
        ReviveEnterFn oReviveEnter = nullptr;

        bool g_reviveLogged = false;

        using UpdateZoomFn = void(__thiscall*)(fb::ClientSoldierAimingSimulation* aiming, int a, int b, int c, int d);
        UpdateZoomFn oUpdateZoom = nullptr;

        // the unzoomed level's fov when a weapon has none
        constexpr float DEFAULT_FOV = 55.0f;
        // zoom levels this wide are the unmagnified sights (irons, red dots, IRNV), the 2x holo is 32
        constexpr float SIGHT_FOV = 40.0f;

        // a zoom level's data as loaded, and its weapon's unzoomed level
        struct ZoomOriginal
        {
            float fov;
            bool scaling;
            float hipFov;
            bool hipScaling;
        };
        std::unordered_map<fb::ZoomLevelData*, ZoomOriginal> g_zoomOriginals;
        const fb::SoldierAimingSimulationData* g_zoomPatched = nullptr; // the weapon data last patched
        std::mutex g_zoomLock;

        const double CAPTURE_PROGRESS_EPSILON = 0.0;

        // man down's onHealthHasChanged sub_7BC6C0 switches to revive on the health update that brings the soldier back,
        // so the revive state only sees the next one. Without another update it never leaves
        // credits gm_fix
        void __fastcall hkReviveEnter(fb::HealthModuleState* _this, void*)
        {
            oReviveEnter(_this);

            fb::ClientSoldierEntity* soldier = _this->m_module->m_soldier;
            if (!soldier || soldier->m_health <= 0.0f)
                return;

            if (!g_reviveLogged)
            {
                g_reviveLogged = true;
                log("revive state: replaying health {}", soldier->m_health);
            }
            _this->onHealthHasChanged(soldier->m_health);
        }

        void applyZoomLevel(fb::ZoomLevelData* zoom, const ZoomOriginal& original)
        {
            const int mode = settings().sightZoom;
            if (mode == SightZoomOff && original.fov >= SIGHT_FOV)
            {
                zoom->m_fieldOfView = original.hipFov;
                zoom->m_allowFieldOfViewScaling = original.hipScaling;
                return;
            }
            zoom->m_fieldOfView = original.fov;
            zoom->m_allowFieldOfViewScaling = original.scaling || mode == SightZoomScaled;
        }

        void __fastcall hkUpdateZoom(fb::ClientSoldierAimingSimulation* _this, void*, int a, int b, int c, int d)
        {
            const fb::SoldierAimingSimulationData* data = _this->m_data;
            if (data && data != g_zoomPatched && data->m_zoomLevels.size())
            {
                std::lock_guard lock(g_zoomLock);
                g_zoomPatched = data;
                const fb::ZoomLevelData* hip = data->m_zoomLevels.m_firstElement[0];
                for (uint32_t i = 1; i < data->m_zoomLevels.size(); ++i)
                {
                    fb::ZoomLevelData* zoom = data->m_zoomLevels.m_firstElement[i];
                    if (!zoom)
                        continue;

                    const ZoomOriginal original{ zoom->m_fieldOfView, zoom->m_allowFieldOfViewScaling,
                        hip ? hip->m_fieldOfView : DEFAULT_FOV, hip ? hip->m_allowFieldOfViewScaling : true };
                    const auto [entry, added] = g_zoomOriginals.try_emplace(zoom, original);
                    if (added)
                        log("zoom level {} fov {:.1f} scaling {}, hipfire {:.1f} scaling {}", i, original.fov, original.scaling, original.hipFov, original.hipScaling);
                    applyZoomLevel(zoom, entry->second);
                }
            }
            oUpdateZoom(_this, a, b, c, d);
        }

        // credits FlashHit
        void patchCaptureProgress()
        {
            auto operand = reinterpret_cast<const double**>(OFF_UICapturePoint_progressEpsilon);
            DWORD oldProtect;
            if (!VirtualProtect(operand, sizeof(*operand), PAGE_EXECUTE_READWRITE, &oldProtect))
            {
                log("capture progress: VirtualProtect failed {}", GetLastError());
                return;
            }
            *operand = &CAPTURE_PROGRESS_EPSILON;
            VirtualProtect(operand, sizeof(*operand), oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), operand, sizeof(*operand));
            log("capture progress epsilon {}", CAPTURE_PROGRESS_EPSILON);
        }
    }

    void forgetZoomLevels(fb::InternalDatabasePartition* partition)
    {
        std::lock_guard lock(g_zoomLock);
        for (fb::DataContainer* object : partition->m_instances)
            g_zoomOriginals.erase(static_cast<fb::ZoomLevelData*>(object));
        g_zoomPatched = nullptr;
    }

    // updateZoom reads the zoom level on every zoom change, so the next one uses it
    void applyZoomFov()
    {
        std::lock_guard lock(g_zoomLock);
        for (const auto& [zoom, original] : g_zoomOriginals)
            applyZoomLevel(zoom, original);
        g_zoomPatched = nullptr;
        log("sight zoom {} on {} zoom levels", SIGHT_ZOOM_NAMES[settings().sightZoom], g_zoomOriginals.size());
    }

    void installGameplayHooks()
    {
        hook(OFF_HealthModuleStateRevive_enter, hkReviveEnter, &oReviveEnter);
        hook(OFF_ClientSoldierAimingSimulation_updateZoom, hkUpdateZoom, &oUpdateZoom);
        patchCaptureProgress();
    }
}
