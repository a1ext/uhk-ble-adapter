/* Minimal Arduino shim: lets src/ui.cpp, src/live.cpp and LVGL build on a PC (tools/ui_sim) */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef enum { GPIO_NUM_0 = 0 } gpio_num_t;
/* single-threaded here: FreeRTOS critical sections are no-ops, PSRAM is the heap */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define ps_calloc calloc
#ifdef __cplusplus
extern "C" {
#endif
uint32_t millis(void);
#ifdef __cplusplus
}
#include <algorithm>
using std::min;
using std::max;
#endif
