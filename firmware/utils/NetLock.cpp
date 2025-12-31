#include "utils/NetLock.h"

namespace
{
    SemaphoreHandle_t g_netMutex = nullptr;
}

void NetLock::init()
{
    if (g_netMutex == nullptr)
    {
        g_netMutex = xSemaphoreCreateMutex();
    }
}

NetLock::Guard::Guard(uint32_t timeoutMs)
{
    if (g_netMutex == nullptr)
    {
        NetLock::init();
    }
    if (g_netMutex == nullptr)
    {
        // If creation failed, fail open so we don't stall network.
        m_locked = true;
        return;
    }
    if (xSemaphoreTake(g_netMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE)
    {
        m_locked = true;
    }
}

NetLock::Guard::~Guard()
{
    if (m_locked && g_netMutex)
    {
        xSemaphoreGive(g_netMutex);
    }
}
