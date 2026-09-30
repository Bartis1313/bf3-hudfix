#include "hudfix.h"

#include <MinHook.h>

#include <cstring>

namespace hudfix
{
    namespace
    {
        using GetMemoryInfoFn = void(__cdecl*)(unsigned int* total, unsigned int* available);
        GetMemoryInfoFn oGetMemoryInfo = nullptr;

        // GetPerformanceInfo counts every process, thread and handle on the system, so it stalls the frame
        constexpr ULONGLONG MEMORY_INFO_INTERVAL = 1000; // ms

        // chat rows never expire (createChatMessage sub_92FD70 gives them duration -1), so the queue size is how many
        // rows UIMessageComp::update sub_933800 rebuilds and sends to flash on every new line
        constexpr uint32_t CHAT_QUEUE_SIZE = 20;

        ULONGLONG g_memoryInfoTime = 0;
        unsigned int g_memoryTotal = 0;
        unsigned int g_memoryAvailable = 0;

        // PerfOverlay::update sub_66F4E0 asks every frame for PerformanceClientMessage's freeCpuMemory
        void __cdecl hkGetMemoryInfo(unsigned int* total, unsigned int* available)
        {
            const ULONGLONG now = GetTickCount64();
            if (!g_memoryInfoTime || now - g_memoryInfoTime >= MEMORY_INFO_INTERVAL)
            {
                if (!g_memoryInfoTime)
                    log("memory info throttled to {} ms", MEMORY_INFO_INTERVAL);
                oGetMemoryInfo(&g_memoryTotal, &g_memoryAvailable);
                g_memoryInfoTime = now;
            }
            *total = g_memoryTotal;
            *available = g_memoryAvailable;
        }
    }

    // credits FlashHit
    void patchChatQueue(fb::InternalDatabasePartition* partition)
    {
        for (fb::DataContainer* object : partition->m_instances)
        {
            const char* type = fb::typeName(object);
            if (!type || std::strcmp(type, "UIMessageCompData") != 0)
                continue;

            fb::MessageInfo& chat = static_cast<fb::UIMessageCompData*>(object)->m_ChatMessageInfo;
            if (chat.m_MessageQueueSize <= CHAT_QUEUE_SIZE)
                continue;

            log("chat queue {} -> {}", chat.m_MessageQueueSize, CHAT_QUEUE_SIZE);
            chat.m_MessageQueueSize = CHAT_QUEUE_SIZE;
        }
    }

    void installPerfHooks()
    {
        hook(OFF_Environment_getMemoryInfo, hkGetMemoryInfo, &oGetMemoryInfo);
    }
}
