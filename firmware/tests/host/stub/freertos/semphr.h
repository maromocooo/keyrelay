#pragma once
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
#define xSemaphoreTake(a,b) ((void)(a), (void)(b))
#define xSemaphoreGive(a) ((void)(a))
