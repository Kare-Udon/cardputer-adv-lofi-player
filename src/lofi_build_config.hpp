#pragma once

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#ifndef CONFIG_LOFI_DEBUG_AUTOMATION
#define CONFIG_LOFI_DEBUG_AUTOMATION 0
#endif

#if !defined(ESP_PLATFORM) || CONFIG_LOFI_DEBUG_AUTOMATION
#define LOFI_DEBUG_AUTOMATION_ENABLED 1
#else
#define LOFI_DEBUG_AUTOMATION_ENABLED 0
#endif
