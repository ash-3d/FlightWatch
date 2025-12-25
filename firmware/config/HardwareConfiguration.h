#pragma once

#include <Arduino.h>

namespace HardwareConfiguration
{
    // Trinity HUB75 panel configuration
    static const uint16_t DISPLAY_MATRIX_WIDTH = 64;
    static const uint16_t DISPLAY_MATRIX_HEIGHT = 64;
    static const uint8_t DISPLAY_CHAIN_LENGTH = 1;
    static const uint8_t DISPLAY_GPIO_E = 18; // Trinity maps E pin to GPIO18
}
