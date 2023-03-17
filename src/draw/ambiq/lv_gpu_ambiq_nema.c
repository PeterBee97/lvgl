/*********************
 *      INCLUDES
 *********************/

#include "lv_gpu_ambiq_nema.h"

#if LV_USE_GPU_AMBIQ_NEMA

#include LV_GPU_AMBIQ_NEMA_INCLUDE

/*********************
 *      DEFINES
 *********************/

#define BGRA_TO_RGBA(c) (((c)&0xFF00FF00) | ((c) >> 16 & 0xFF) | ((c) << 16 & 0xFF0000))

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static void lv_draw_ambiq_img_decoded(struct _lv_draw_ctx_t * draw_ctx, const lv_draw_img_dsc_t * dsc, const lv_area_t * coords,
                             const uint8_t * map_p, const lv_draw_img_sup_t * sup, lv_color_format_t color_format);

static void lv_draw_ambiq_blend(lv_draw_ctx_t * draw_ctx, const lv_draw_sw_blend_dsc_t * dsc);

static void lv_draw_ambiq_rect(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords);

static lv_res_t draw_ambiq_bg(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords);

static void lv_draw_ambiq_arc(lv_draw_ctx_t * draw_ctx, const lv_draw_arc_dsc_t * dsc, const lv_point_t * center,
                              uint16_t radius, uint16_t start_angle, uint16_t end_angle);

/**********************
 *  STATIC VARIABLES
 **********************/

static uint8_t* g_tex;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_ambiq_ctx_init(lv_disp_t * drv, lv_draw_ctx_t * draw_ctx)
{
    lv_draw_sw_init_ctx(drv, draw_ctx);

    lv_draw_ambiq_ctx_t * ambiq_draw_ctx = (lv_draw_sw_ctx_t *)draw_ctx;

    // ambiq_draw_ctx->base_draw.draw_arc = lv_draw_ambiq_arc;
    // ambiq_draw_ctx->base_draw.draw_rect = lv_draw_ambiq_rect;
    ambiq_draw_ctx->base_draw.draw_img_decoded = lv_draw_ambiq_img_decoded;
    ambiq_draw_ctx->blend = lv_draw_ambiq_blend;
    //ambiq_draw_ctx->base_draw.wait_for_finish = lv_draw_ambiq_wait_cb;
}

void lv_draw_ambiq_ctx_deinit(lv_disp_t * drv, lv_draw_ctx_t * draw_ctx)
{
    lv_draw_sw_deinit_ctx(drv, draw_ctx);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void lv_draw_ambiq_blend(lv_draw_ctx_t * draw_ctx, const lv_draw_sw_blend_dsc_t * dsc)
{
    lv_area_t blend_area;

    /*Let's get the blend area which is the intersection of the area to fill and the clip area.*/
    if(!_lv_area_intersect(&blend_area, dsc->blend_area, draw_ctx->clip_area))
        return; /*Fully clipped, nothing to do*/

    /*Make the blend area relative to the buffer*/
    lv_area_move(&blend_area, -draw_ctx->buf_area->x1, -draw_ctx->buf_area->y1);

    nema_cmdlist_t sCL;

    /* Fill/Blend only normal blended */
    if(dsc->blend_mode == LV_BLEND_MODE_NORMAL) {
        const lv_color_t * src_buf = dsc->src_buf;
        lv_coord_t dst_w = lv_area_get_width(draw_ctx->buf_area);
        lv_coord_t dst_h = lv_area_get_height(draw_ctx->buf_area);
        lv_coord_t blend_w = lv_area_get_width(&blend_area);
        lv_coord_t blend_h = lv_area_get_height(&blend_area);
        uint32_t color = BGRA_TO_RGBA(lv_color32_to_int(lv_color_to32(dsc->color))) & 0xFFFFFF | dsc->opa << 24;
        uint32_t x_op = NEMA_BLOP_MODULATE_A;
        sCL = nema_cl_create();
        nema_cl_bind(&sCL);
        nema_set_clip(blend_area.x1, blend_area.y1, blend_w, blend_h);
        nema_bind_dst_tex(draw_ctx->buf, dst_w, dst_h, NEMA_COLOR_MODE, -1);
        if (dsc->mask_buf != NULL) {
            lv_coord_t mask_w = lv_area_get_width(dsc->mask_area);
            lv_coord_t mask_h = lv_area_get_height(dsc->mask_area);
            nema_bind_tex(NEMA_TEX3, dsc->mask_buf, mask_w, mask_h, NEMA_A8, -1, NEMA_FILTER_PS);
            x_op |= NEMA_BLOP_STENCIL_TXTY;
        }
        nema_set_const_color(color);
        if(src_buf == NULL) {
            x_op |= NEMA_BLOP_MODULATE_RGB;
            nema_bind_src_tex(g_tex, 8, 1, NEMA_L1, -1, NEMA_FILTER_PS | NEMA_TEX_REPEAT);
        } else {
            nema_bind_src_tex(src_buf, blend_w, blend_h, NEMA_COLOR_MODE, -1, NEMA_FILTER_PS);
        }
        nema_set_blend_blit(nema_blending_mode(NEMA_BF_SRCALPHA, NEMA_BF_INVSRCALPHA, x_op));
        nema_blit_rect(blend_area.x1, blend_area.y1, blend_w, blend_h);
        nema_cl_submit(&sCL);
        nema_cl_wait(&sCL);
        nema_cl_rewind(&sCL);
        nema_cl_destroy(&sCL);
    } else {
        LV_LOG_ERROR("Blend mode %d not supported", dsc->blend_mode);
        lv_draw_sw_blend_basic(draw_ctx, dsc);
    }
}

static void lv_draw_ambiq_img_decoded(struct _lv_draw_ctx_t * draw_ctx, const lv_draw_img_dsc_t * dsc, const lv_area_t * coords,
                                      const uint8_t * map_p, const lv_draw_img_sup_t * sup, lv_color_format_t color_format)
{
    /*Use the clip area as draw area*/
    lv_area_t blend_area;
    lv_area_copy(&blend_area, draw_ctx->clip_area);

    bool mask_any = lv_draw_mask_is_any(&blend_area);
    bool done = false;

    lv_coord_t dst_w = lv_area_get_width(draw_ctx->buf_area);
    lv_coord_t dst_h = lv_area_get_height(draw_ctx->buf_area);
    lv_coord_t blend_w = lv_area_get_width(&blend_area);
    lv_coord_t blend_h = lv_area_get_height(&blend_area);

    /*Make the blend area relative to the buffer*/
    lv_area_move(&blend_area, -draw_ctx->buf_area->x1, -draw_ctx->buf_area->y1);

    const lv_color_t * src_buf = (const lv_color_t *)map_p;
    if(!src_buf) {
        lv_draw_sw_img_decoded(draw_ctx, dsc, coords, map_p, sup, color_format);
        return;
    }

    size_t img_size;
    nema_cmdlist_t sCL;
    nema_tex_format_t src_cf;
    if (color_format == LV_COLOR_FORMAT_NATIVE_ALPHA) {
        img_size = lv_area_get_size(coords) * LV_COLOR_FORMAT_NATIVE_ALPHA_SIZE;
#if LV_COLOR_DEPTH == 16
        LV_LOG_INFO("BGRA5658 not supported by NEMA GPU");
        lv_draw_sw_img_decoded(draw_ctx, dsc, coords, map_p, sup, color_format);
        return;
#else
        src_cf = NEMA_COLOR_MODE_ALPHA;
#endif
    } else {
        img_size = lv_area_get_size(coords) * sizeof(lv_color_t);
        src_cf = NEMA_COLOR_MODE;
    }

    uint16_t angle = dsc->angle;
    uint16_t zoom = dsc->zoom;
    lv_point_t pivot = dsc->pivot;
    lv_blend_mode_t blend_mode = dsc->blend_mode;
    bool transformed = (angle != 0) || (zoom != LV_ZOOM_NONE);
    lv_coord_t map_w = lv_area_get_width(coords);
    lv_coord_t map_h = lv_area_get_height(coords);
    lv_opa_t * mask_buf = NULL;
    if(mask_any) {
        mask_buf = nema_malloc(blend_w * blend_h * sizeof(lv_opa_t));
        if (mask_buf) {
            lv_opa_t * mask_buf_tmp = mask_buf;
            lv_coord_t x_ofs = blend_area.x1 + draw_ctx->buf_area->x1;
            lv_coord_t y_ofs = blend_area.y1 + draw_ctx->buf_area->y1;
            for(lv_coord_t y = 0; y < blend_h; y++) {
                lv_draw_mask_res_t mask_res_line;
                mask_res_line = lv_draw_mask_apply(mask_buf_tmp, x_ofs, y + y_ofs, blend_w);

                if(mask_res_line == LV_DRAW_MASK_RES_TRANSP) {
                    lv_memzero(mask_buf_tmp, blend_w);
                }
                mask_buf_tmp += blend_w;
            }
        }
    }

    if (!mask_any || mask_buf) {
        /* Continue iff mask operations complete successfully */
        sCL = nema_cl_create();
        nema_cl_bind(&sCL);
        // nema_set_clip(0, 0, lv_area_get_width(draw_ctx->buf_area), lv_area_get_height(draw_ctx->buf_area));
        nema_set_clip(blend_area.x1, blend_area.y1, blend_w, blend_h);
        uint32_t x_op = NEMA_BLOP_NONE;

        /* WARNING: src_buf should be in SSRAM or PSRAM */
        nema_bind_src_tex(src_buf, map_w, map_h, src_cf, -1, NEMA_FILTER_PS);
        nema_bind_dst_tex((uintptr_t)draw_ctx->buf, dst_w, dst_h, NEMA_COLOR_MODE, -1);
        if (mask_buf) {
            /* Use TEX3 for mask */
            nema_bind_tex(NEMA_TEX3, mask_buf, blend_w, blend_h, NEMA_A8, -1, NEMA_FILTER_PS);
            x_op |= NEMA_BLOP_STENCIL_TXTY;
        }
        if (color_format == LV_COLOR_FORMAT_NATIVE_CHROMA_KEYED) {
            nema_set_src_color_key(BGRA_TO_RGBA(lv_color32_to_int(lv_color_to32(lv_color_chroma_key()))));
            x_op |= NEMA_BLOP_SRC_CKEY;
        }
        /* GPU modulated src color is different from LVGL recolor, so the following should not be used */
        if (dsc->recolor_opa != LV_OPA_TRANSP) {
            lv_color32_t color32 = lv_color_to32(dsc->recolor);
            color32.alpha = dsc->recolor_opa;
            nema_set_const_color(BGRA_TO_RGBA(lv_color32_to_int(color32)));
            x_op |= NEMA_BLOP_MODULATE_RGB;
        }

        /* Set blend factor */
        nema_set_blend_blit(nema_blending_mode(NEMA_BF_SRCALPHA, NEMA_BF_INVSRCALPHA, x_op));
        if (!transformed) {
            /* Simple blit */
            nema_blit_subrect(blend_area.x1, blend_area.y1, blend_w, blend_h,
                draw_ctx->clip_area->x1 - coords->x1, draw_ctx->clip_area->y1 - coords->y1);
        } else if (zoom == LV_ZOOM_NONE) {
            /* Rotation w/o zoom*/
            nema_blit_rotate_pivot(blend_area.x1 + pivot.x, blend_area.y1 + pivot.y, pivot.x, pivot.y, angle * 0.1f);
        } else {
            /* Arbitrary transform with zoom */
            float m[3][3];
            float scale = zoom / 256.0f;
            float x[4] = { coords->x1, coords->x2, coords->x2, coords->x1 };
            float y[4] = { coords->y1, coords->y1, coords->y2, coords->y2 };
            pivot.x += coords->x1;
            pivot.y += coords->y1;
            nema_mat3x3_load_identity(m);
            nema_mat3x3_translate(m, -pivot.x, -pivot.y);
            nema_mat3x3_scale(m, scale, scale);
            if (angle != 0) {
                nema_mat3x3_rotate(m, angle * 0.1f);
            }
            nema_mat3x3_translate(m, pivot.x, pivot.y);
            nema_mat3x3_mul_vec(m, &x[0], &y[0]);
            nema_mat3x3_mul_vec(m, &x[1], &y[1]);
            nema_mat3x3_mul_vec(m, &x[2], &y[2]);
            nema_mat3x3_mul_vec(m, &x[3], &y[3]);
            nema_blit_quad_fit(x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3]);
        }

        done = true;
    }

    if (done) {
        nema_cl_submit(&sCL);
        nema_cl_wait(&sCL);
        nema_cl_rewind(&sCL);
        nema_cl_destroy(&sCL);
        if (mask_buf) {
            nema_free(mask_buf);
        }
    } else {
        lv_draw_sw_img_decoded(draw_ctx, dsc, coords, map_p, sup, color_format);
    }
}

static void lv_draw_ambiq_rect(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords)
{
    bool done = false;
    lv_draw_rect_dsc_t ambiq_dsc;

    lv_memcpy(&ambiq_dsc, dsc, sizeof(ambiq_dsc));

    /* Draw only the shadow */
    ambiq_dsc.bg_opa = 0;
    ambiq_dsc.bg_img_opa = 0;
    ambiq_dsc.border_opa = 0;
    ambiq_dsc.outline_opa = 0;

    lv_draw_sw_rect(draw_ctx, &ambiq_dsc, coords);

    /* Draw the background */
    ambiq_dsc.shadow_opa = 0;
    ambiq_dsc.bg_opa = dsc->bg_opa;
    done = (draw_ambiq_bg(draw_ctx, &ambiq_dsc, coords) == LV_RES_OK);

    /* Draw the remaining parts */
    ambiq_dsc.shadow_opa = 0;
    if(done)
        ambiq_dsc.bg_opa = 0;
    ambiq_dsc.bg_img_opa = dsc->bg_img_opa;
    ambiq_dsc.border_opa = dsc->border_opa;
    ambiq_dsc.outline_opa = dsc->outline_opa;

    lv_draw_sw_rect(draw_ctx, &ambiq_dsc, coords);
}

static lv_res_t draw_ambiq_bg(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords)
{
    if(dsc->bg_opa <= LV_OPA_MIN)
        return LV_RES_INV;

    lv_area_t bg_coords;
    lv_area_copy(&bg_coords, coords);

    /*If the border fully covers make the bg area 1px smaller to avoid artifacts on the corners*/
    if(dsc->border_width > 1 && dsc->border_opa >= (lv_opa_t)LV_OPA_MAX && dsc->radius != 0) {
        bg_coords.x1 += (dsc->border_side & LV_BORDER_SIDE_LEFT) ? 1 : 0;
        bg_coords.y1 += (dsc->border_side & LV_BORDER_SIDE_TOP) ? 1 : 0;
        bg_coords.x2 -= (dsc->border_side & LV_BORDER_SIDE_RIGHT) ? 1 : 0;
        bg_coords.y2 -= (dsc->border_side & LV_BORDER_SIDE_BOTTOM) ? 1 : 0;
    }

    lv_area_t clipped_coords;
    if(!_lv_area_intersect(&clipped_coords, &bg_coords, draw_ctx->clip_area))
        return LV_RES_INV;

    lv_grad_dir_t grad_dir = dsc->bg_grad.dir;
    lv_color_t bg_color    = grad_dir == LV_GRAD_DIR_NONE ? dsc->bg_color : dsc->bg_grad.stops[0].color;
    if(lv_color_to_int(bg_color) == lv_color_to_int(dsc->bg_grad.stops[1].color)) grad_dir = LV_GRAD_DIR_NONE;

    bool mask_any = lv_draw_mask_is_any(&bg_coords);

    return LV_RES_INV;
}

static void lv_draw_ambiq_arc(lv_draw_ctx_t * draw_ctx, const lv_draw_arc_dsc_t * dsc, const lv_point_t * center,
                            uint16_t radius, uint16_t start_angle, uint16_t end_angle)
{
    bool done = false;

    if(dsc->opa <= LV_OPA_MIN)
        return;
    if(dsc->width == 0)
        return;
    if(start_angle == end_angle)
        return;

    if(!done)
        lv_draw_sw_arc(draw_ctx, dsc, center, radius, start_angle, end_angle);
}

void lv_draw_ambiq_nema_init()
{
    g_tex = nema_malloc(1);
    *g_tex = 0xFF;
}

#endif /* LV_USE_GPU_AMBIQ_NEMA */
