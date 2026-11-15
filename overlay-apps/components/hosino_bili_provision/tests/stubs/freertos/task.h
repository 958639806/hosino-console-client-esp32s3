#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreateWithCaps(void (*)(void *),const char *,unsigned,void *,unsigned,void *,unsigned);
void vTaskDeleteWithCaps(void *);
