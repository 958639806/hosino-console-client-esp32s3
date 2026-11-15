#pragma once
#include "task.h"
BaseType_t xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack,
    void *arg, unsigned priority, TaskHandle_t *out, unsigned caps);
