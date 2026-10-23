#pragma once
#include <stdio.h>
#define ESP_LOGW(t, ...) (printf("%s: ", t), printf(__VA_ARGS__), printf("\n"))
