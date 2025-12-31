#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace NetLock
{
    void init();

    class Guard
    {
    public:
        explicit Guard(uint32_t timeoutMs = 5000);
        ~Guard();
        bool locked() const { return m_locked; }

    private:
        bool m_locked = false;
    };
}
