#include "hudfix.h"

namespace hudfix
{
    namespace
    {
        using ReviveEnterFn = void(__thiscall*)(fb::HealthModuleState* state);
        ReviveEnterFn oReviveEnter = nullptr;

        bool g_reviveLogged = false;

        // man down's onHealthHasChanged sub_7BC6C0 switches to revive on the health update that brings the soldier back,
        // so the revive state only sees the next one. Without another update it never leaves
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
    }

    // credits gm_fix
    void installGameplayHooks()
    {
        hook(OFF_HealthModuleStateRevive_enter, hkReviveEnter, &oReviveEnter);
    }
}
