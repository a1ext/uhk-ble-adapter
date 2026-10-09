/* Minimal Arduino shim: lets src/ui.cpp and LVGL build on a PC (tools/ui_sim) */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef enum { GPIO_NUM_0 = 0 } gpio_num_t;
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
