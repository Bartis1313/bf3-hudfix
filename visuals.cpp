#include "hudfix.h"

namespace hudfix
{
    // this is compact, for proper custom editing please use bf-visual-editor
    namespace
    {
        using UpdateFn = int(__thiscall*)(fb::VisualEnvironmentManager* manager, const void* params);
        UpdateFn oUpdate = nullptr;

        void applyColorCorrection(fb::ColorCorrectionState* state)
        {
            fb::ColorCorrectionComponent* component = fb::ColorCorrectionComponent::fromState(state);
            if (!component->isValid())
                return;

            component->refresh();

            if (settings().neutralColors)
            {
				// all data colors
                for (int i = 0; i < 3; ++i)
                {
                    state->m_contrast[i] = 1.0f;
                    state->m_saturation[i] = 1.0f;
                    state->m_brightness[i] = 1.0f;
                }
            }
            if (settings().noColorGrading)
                state->m_colorGradingEnable = false;
        }

        int __fastcall hkUpdate(fb::VisualEnvironmentManager* _this, void*, const void* params)
        {
            if (_this->m_dirty)
            {
                for (fb::VisualEnvironmentState* state : _this->m_states)
                {
                    if (state && state->m_colorCorrection)
                        applyColorCorrection(state->m_colorCorrection);
                }
            }
            return oUpdate(_this, params);
        }
    }

    void applyVisuals()
    {
        if (fb::VisualEnvironmentManager* manager = fb::VisualEnvironmentManager::GetInstance())
            manager->m_dirty = true;

        log("neutral color correction {}, no color grading {}", settings().neutralColors, settings().noColorGrading);
    }

    void installVisualHooks()
    {
        hook(OFF_VisualEnvironmentManager_update, hkUpdate, &oUpdate);
    }
}
