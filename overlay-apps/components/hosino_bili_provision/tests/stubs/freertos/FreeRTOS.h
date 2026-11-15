#pragma once
#include <stddef.h>
typedef int portMUX_TYPE;
typedef int BaseType_t;
typedef unsigned TickType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
#define pdPASS 1
