#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
void xTaskNotifyGive(TaskHandle_t handle);
uint32_t ulTaskNotifyTake(BaseType_t clear, uint32_t wait);
