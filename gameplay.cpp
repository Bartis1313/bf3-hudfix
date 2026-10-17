#include "hudfix.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <numbers>
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

        using FindOptionFn = fb::ProfileOptionDataFloat*(__thiscall*)(fb::SettingsGroup* group, const char* name);
        constexpr int RENDER_GROUP = 1;
        constexpr float MAX_FIELD_OF_VIEW = 120.0f;

        // the unzoomed level's fov when a weapon has none
        constexpr float DEFAULT_FOV = 55.0f;
        // zoom levels this wide are the unmagnified sights (irons, red dots, IRNV), the 2x holo is 32
        constexpr float SIGHT_FOV = 40.0f;

        // a zoom level's data as loaded, and its weapon's unzoomed level
        struct ZoomOriginal
        {
            float fov;
            bool scaling;
            float lookSpeed;
            float hipFov;
            bool hipScaling;
            float hipLookSpeed;
        };

        using UpdateAimerFn = void(__thiscall*)(void* aimer, fb::AimingInput* input);
        UpdateAimerFn oUpdateAimer = nullptr;

        float g_aimerFov = 0.0f;
        float g_aimerLoggedFov = 0.0f;

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

        // the fov slider's multiplier
        float sliderFovMultiplier()
        {
            const fb::ClientGameContext* context = fb::ClientGameContext::GetInstance();
            fb::Level* level = context ? context->m_level : nullptr;
            return level ? level->fovMultiplier() : 1.0f;
        }

        std::atomic<bool> g_fieldOfViewChanged{ false };

        // the camera takes a new slider value only on a zoom change, so the current zoom level's fov is asked for again the
        // way updateZoom asks for it
        void requestZoomFov(const fb::ClientSoldierAimingSimulation* aiming)
        {
            const fb::SoldierAimingSimulationData* data = aiming->m_data;
            fb::ClientCameraManager* camera = fb::ClientCameraManager::GetInstance();
            if (!data || !camera || aiming->m_zoomLevel < 0 || static_cast<uint32_t>(aiming->m_zoomLevel) >= data->m_zoomLevels.size())
                return;
            const fb::ZoomLevelData* zoom = data->m_zoomLevels.m_firstElement[aiming->m_zoomLevel];
            if (!zoom)
                return;

            fb::FovRequest request{ };
            request.m_fov = aiming->m_fovMultiplier * zoom->m_fieldOfView * (zoom->m_allowFieldOfViewScaling ? sliderFovMultiplier() : 1.0f);
            request.m_transitionTime = aiming->m_fovTransitionTime;
            request.m_active = true;
            camera->requestFov(&request);
            log("field of view: zoom level {} camera fov {:.2f}", aiming->m_zoomLevel, request.m_fov);
        }

        // copied from bf4 bin sub_140FA1E80: 0 is the zoom ratio, the limit of atan(c * x) / c, 1 the fov ratio
        double uniformRatio(double hipFov, double fov)
        {
            constexpr double HALF_ANGLE = std::numbers::pi / 360.0;
            const double coefficient = settings().uniformAimingCoefficient / 100.0;
            const auto angle = [coefficient](double fov)
            {
                const double half = std::tan(fov * HALF_ANGLE);
                return coefficient > 0.0 ? std::atan(coefficient * half) : half;
            };
            return angle(fov) / angle(hipFov);
        }

        void applyZoomLevel(fb::ZoomLevelData* zoom, const ZoomOriginal& original)
        {
            const int mode = settings().sightZoom;
            const bool hipfire = mode == SightZoomOff && original.fov >= SIGHT_FOV;
            zoom->m_fieldOfView = hipfire ? original.hipFov : original.fov;
            zoom->m_allowFieldOfViewScaling = hipfire ? original.hipScaling : original.scaling || mode == SightZoomScaled;
            const float lookSpeed = settings().uniformAiming ? original.hipLookSpeed : original.lookSpeed;
            zoom->m_lookSpeedMultiplier = lookSpeed * settings().zoomSensitivity / 100.0f;
        }

        // copied from bf4 bin sub_140FA2660 replaces the look speed every frame by the ratio of the camera's fov of the moment to the
        // hipfire fov, so it follows the fov through the zoom transitions, unzoomed included
        void __fastcall hkUpdateAimer(void* _this, void*, fb::AimingInput* input)
        {
            const fb::ClientSoldierAimingSimulation* aiming = fb::ClientSoldierAimingSimulation::fromInput(input);
            const fb::SoldierAimingSimulationData* data = aiming->m_data;
            const fb::ZoomLevelData* hip = data && data->m_zoomLevels.size() ? data->m_zoomLevels.m_firstElement[0] : nullptr;
            fb::ClientCameraManager* camera = fb::ClientCameraManager::GetInstance();
            if (hip && camera)
            {
                const float lookSpeed = input->m_lookSpeed;
                const double hipFov = hip->m_fieldOfView * (hip->m_allowFieldOfViewScaling ? sliderFovMultiplier() : 1.0f);
                const float fov = camera->getFov();
                const double ratio = hipFov > 0.0 && fov > 0.0 ? uniformRatio(hipFov, fov) : 1.0;
                if (settings().uniformAiming)
                    input->m_lookSpeed *= static_cast<float>(ratio);

                // only on new values
                if (fov == g_aimerFov && std::abs(fov - g_aimerLoggedFov) > 0.5f)
                {
                    g_aimerLoggedFov = fov;
                    log("aimer: zoom level {} camera fov {:.2f}, hipfire fov {:.2f} (level {:.1f} scaling {}, slider {:.3f}, aiming {:.3f}), ratio {:.3f}, look speed {:.3f} -> {:.3f}",
                        aiming->m_zoomLevel, fov, hipFov, hip->m_fieldOfView, hip->m_allowFieldOfViewScaling, sliderFovMultiplier(), aiming->m_fovMultiplier,
                        ratio, lookSpeed, input->m_lookSpeed);
                }
                g_aimerFov = fov;
            }
            oUpdateAimer(_this, input);
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

                    const ZoomOriginal original{ zoom->m_fieldOfView, zoom->m_allowFieldOfViewScaling, zoom->m_lookSpeedMultiplier,
                        hip ? hip->m_fieldOfView : DEFAULT_FOV, hip ? hip->m_allowFieldOfViewScaling : true, hip ? hip->m_lookSpeedMultiplier : 1.0f };
                    const auto [entry, added] = g_zoomOriginals.try_emplace(zoom, original);
                    if (added)
                        log("zoom level {} fov {:.1f} scaling {} look speed {:.3f}, hipfire {:.1f} scaling {} look speed {:.3f}", i, original.fov,
                            original.scaling, original.lookSpeed, original.hipFov, original.hipScaling, original.hipLookSpeed);
                    applyZoomLevel(zoom, entry->second);
                }
            }
            oUpdateZoom(_this, a, b, c, d);
            if (g_fieldOfViewChanged.exchange(false))
                requestZoomFov(_this);
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

    void raiseFieldOfViewMax()
    {
        fb::ProfileOptions* game = fb::ProfileOptions::GetInstance();
        auto option = game ? reinterpret_cast<FindOptionFn>(OFF_SettingsGroup_findOption)(&game->m_groups[RENDER_GROUP], "FieldOfView") : nullptr;
        if (!option)
        {
            static bool warned = false;
            if (!warned)
                log("field of view option not found");
            warned = true;
            return;
        }
        if (option->m_max >= MAX_FIELD_OF_VIEW)
            return;

        log("field of view max {} -> {}", option->m_max, MAX_FIELD_OF_VIEW);
        option->m_max = MAX_FIELD_OF_VIEW;
    }

    void refreshFieldOfView()
    {
        g_fieldOfViewChanged = true;
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
        log("sight zoom {}, uniform aiming {} coefficient {}%, zoom sensitivity {}% on {} zoom levels", SIGHT_ZOOM_NAMES[settings().sightZoom],
            settings().uniformAiming, settings().uniformAimingCoefficient, settings().zoomSensitivity, g_zoomOriginals.size());
    }

    void installGameplayHooks()
    {
        hook(OFF_HealthModuleStateRevive_enter, hkReviveEnter, &oReviveEnter);
        hook(OFF_ClientSoldierAimingSimulation_updateZoom, hkUpdateZoom, &oUpdateZoom);
        hook(OFF_SoldierAimer_update, hkUpdateAimer, &oUpdateAimer);
        patchCaptureProgress();
    }
}
