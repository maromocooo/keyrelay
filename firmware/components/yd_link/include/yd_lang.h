/*
 * Log message language (menuconfig → KeyRelay log language).
 * YD_L("English", "日本語") picks one string literal at compile time, so it can be used anywhere a
 * literal can, including ESP_LOGx format strings and adjacent-literal concatenation.
 */
#pragma once

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#if defined(CONFIG_YD_LOG_LANG_JA)
#define YD_L(en, ja) ja
#else
#define YD_L(en, ja) en
#endif
