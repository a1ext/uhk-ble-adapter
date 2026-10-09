/* The firmware's LVGL config, adapted for the PC renderer (tools/ui_sim) */
#pragma once
#include "../../../lib/LilyGo-AMOLED-Series/src/lv_conf.h"

#undef LV_COLOR_16_SWAP      /* frames are read back as plain RGB565 */
#define LV_COLOR_16_SWAP 0
#undef LV_MEM_CUSTOM         /* no PSRAM allocator: LVGL's own heap */
#define LV_MEM_CUSTOM 0
#undef LV_MEM_SIZE
#define LV_MEM_SIZE (1024U * 1024U)
#undef LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_FAST_MEM
