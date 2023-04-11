
#ifndef LV_GPU_AMBIQ_NEMA_H
#define LV_GPU_AMBIQ_NEMA_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../../lv_conf_internal.h"
#if LV_USE_GPU_AMBIQ_NEMA
#include "../sw/lv_draw_sw.h"

/*********************
 *      DEFINES
 *********************/

#if LV_COLOR_DEPTH == 32
#define NEMA_COLOR_MODE         NEMA_BGRX8888
#define NEMA_COLOR_MODE_ALPHA   NEMA_BGRA8888
#elif LV_COLOR_DEPTH == 24
#define NEMA_COLOR_MODE         NEMA_BGR24
#define NEMA_COLOR_MODE_ALPHA   NEMA_BGRA8888
#elif LV_COLOR_DEPTH == 16
#define NEMA_COLOR_MODE         NEMA_BGR565     /* Available if HW enabled - check HW manual */
#define NEMA_COLOR_MODE_ALPHA   NEMA_BGRA5650   /* BGRA5658 currently not supported */
#warning "16bpp support for NEMA is not fully compatible with LVGL, use for testing only"
#endif

/**********************
 *      TYPEDEFS
 **********************/
typedef lv_draw_sw_ctx_t lv_draw_ambiq_ctx_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

void lv_draw_ambiq_ctx_init(lv_disp_t * drv, lv_draw_ctx_t * draw_ctx);

void lv_draw_ambiq_ctx_deinit(lv_disp_t * drv, lv_draw_ctx_t * draw_ctx);

void lv_draw_ambiq_nema_init(void);

void lv_draw_ambiq_nema_deinit(void);

/**********************
 *      MACROS
 **********************/
#endif /* LV_USE_GPU_AMBIQ_NEMA */

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* LV_GPU_AMBIQ_NEMA_H */
