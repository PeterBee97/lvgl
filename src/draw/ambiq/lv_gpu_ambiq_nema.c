/*********************
 *      INCLUDES
 *********************/

#include "lv_gpu_ambiq_nema.h"

#if LV_USE_GPU_AMBIQ_NEMA

#include LV_GPU_AMBIQ_NEMA_INCLUDE

/*********************
 *      DEFINES
 *********************/

#define BGRA_TO_RGBA(c) (((c) & 0xFF00FF00) | ((c) >> 16 & 0xFF) | ((c) << 16 & 0xFF0000))
#define C32(c)          BGRA_TO_RGBA(lv_color32_to_int(lv_color_to32(c)))
#define C32A(c,a)       (C32(c) & 0xFFFFFF | ((a) << 24))

#define SHADOW_UPSCALE_SHIFT    6
#define SHADOW_ENHANCE          1
#define SPLIT_LIMIT             50

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

static lv_opa_t * get_mask(lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);

static void draw_shadow(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords, lv_opa_t * mask);

static void draw_bg(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords, lv_opa_t * mask);

static void draw_bg_img(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords);

static void draw_border(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords, lv_opa_t * mask);

static void draw_outline(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords, lv_opa_t * mask);

static void draw_border_gpu(lv_draw_ctx_t * draw_ctx, const lv_area_t * outer_area, const lv_area_t * clip_area,
                            int32_t rout, int32_t width, lv_color_t color, lv_opa_t opa);

static void shadow_draw_corner_buf(const lv_area_t * coords, uint16_t * sh_buf, lv_coord_t s, lv_coord_t r);

static void shadow_blur_corner(lv_coord_t size, lv_coord_t sw, uint16_t * sh_ups_buf);

static void draw_border_simple(lv_draw_ctx_t * draw_ctx, const lv_area_t * outer_area, const lv_area_t * inner_area,
                               lv_color_t color, lv_opa_t opa);

/**********************
 *  STATIC VARIABLES
 **********************/

#ifndef NEMA_CONST_TEX
static uint8_t* g_tex;
#define G_TEX g_tex
#else
#define G_TEX NEMA_CONST_TEX
#endif

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

    ambiq_draw_ctx->base_draw.draw_rect = lv_draw_ambiq_rect;
    ambiq_draw_ctx->base_draw.draw_img_decoded = lv_draw_ambiq_img_decoded;
    ambiq_draw_ctx->blend = lv_draw_ambiq_blend;
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
        uint32_t color = C32A(dsc->color, dsc->opa);
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
            nema_bind_src_tex(G_TEX, 8, 1, NEMA_L1, -1, NEMA_FILTER_PS | NEMA_TEX_REPEAT);
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
    lv_area_t map_area;
    lv_area_copy(&map_area, coords);
    lv_area_move(&map_area, -draw_ctx->buf_area->x1, -draw_ctx->buf_area->y1);
    lv_coord_t map_w = lv_area_get_width(coords);
    lv_coord_t map_h = lv_area_get_height(coords);
    lv_opa_t * mask_buf = NULL;
    if(mask_any) {
        lv_coord_t x_ofs = blend_area.x1 + draw_ctx->buf_area->x1;
        lv_coord_t y_ofs = blend_area.y1 + draw_ctx->buf_area->y1;
        mask_buf = get_mask(x_ofs, y_ofs, blend_w, blend_h);
    }

    if (!mask_any || mask_buf) {
        /* Continue iff mask operations complete successfully */
        sCL = nema_cl_create();
        nema_cl_bind(&sCL);
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
            nema_set_src_color_key(C32(lv_color_chroma_key()));
            x_op |= NEMA_BLOP_SRC_CKEY;
        }
        /* GPU modulated src color is different from LVGL recolor, so the following should not be used */
        if (dsc->recolor_opa != LV_OPA_TRANSP) {
            uint32_t color = C32A(dsc->recolor, dsc->recolor_opa);
            nema_set_const_color(color);
            x_op |= NEMA_BLOP_MODULATE_RGB;
        }

        /* Set blend factor */
        nema_set_blend_blit(nema_blending_mode(NEMA_BF_SRCALPHA, NEMA_BF_INVSRCALPHA, x_op));
        if (!transformed) {
            /* Simple blit */
            nema_blit_subrect(blend_area.x1, blend_area.y1, blend_w, blend_h,
                blend_area.x1 - map_area.x1, blend_area.y1 - map_area.y1);
        } else if (zoom == LV_ZOOM_NONE) {
            /* Rotation w/o zoom*/
            nema_blit_rotate_pivot(blend_area.x1 + pivot.x, blend_area.y1 + pivot.y, pivot.x, pivot.y, angle * 0.1f);
        } else {
            /* Arbitrary transform with zoom */
            float m[3][3];
            float scale = zoom / 256.0f;
            float x[4] = { map_area.x1, map_area.x2, map_area.x2, map_area.x1 };
            float y[4] = { map_area.y1, map_area.y1, map_area.y2, map_area.y2 };
            pivot.x += map_area.x1;
            pivot.y += map_area.y1;
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
    if (dsc->blend_mode != LV_BLEND_MODE_NORMAL) {
        lv_draw_sw_rect(draw_ctx, dsc, coords);
        return;
    }
    /* Calc mask in advance, since in NEMA blend with mask is just a simple extra tex */
    bool mask_any = lv_draw_mask_is_any(coords);
    lv_opa_t * mask_buf = NULL;
    if (mask_any) {
        mask_buf = get_mask(coords->x1, coords->y1, lv_area_get_width(coords), lv_area_get_height(coords));
    }

    if (!mask_any || mask_buf) {
        /* Continue iff mask operations complete successfully */
        draw_shadow(draw_ctx, dsc, coords, mask_buf);

        draw_bg(draw_ctx, dsc, coords, mask_buf);
        draw_bg_img(draw_ctx, dsc, coords);

        draw_border(draw_ctx, dsc, coords, mask_buf);

        draw_outline(draw_ctx, dsc, coords, mask_buf);
    } else {
        lv_draw_sw_rect(draw_ctx, dsc, coords);
    }

}

static lv_opa_t * get_mask(lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_draw_mask_res_t mask_res_line;
    lv_opa_t * mask = nema_malloc(w * h * sizeof(lv_opa_t));
    lv_opa_t * p = mask;
    if (mask) {
        for(lv_coord_t i = 0; i < h; i++) {
            mask_res_line = lv_draw_mask_apply(p, x, y + i, w);

            if(mask_res_line == LV_DRAW_MASK_RES_TRANSP) {
                lv_memzero(p, w);
            }
            p += w;
        }
    }
    return mask;
}

static void draw_bg(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords, lv_opa_t * mask)
{
    if(dsc->bg_opa <= LV_OPA_MIN) return;

    lv_coord_t w = lv_area_get_width(coords);
    lv_coord_t h = lv_area_get_height(coords);

    lv_area_t clipped_coords;
    if(!_lv_area_intersect(&clipped_coords, coords, draw_ctx->clip_area)) return;
    lv_area_move(&clipped_coords, -draw_ctx->buf_area->x1, -draw_ctx->buf_area->y1);

    lv_area_t bg_coords;
    lv_area_copy(&bg_coords, coords);
    lv_area_move(&bg_coords, -draw_ctx->buf_area->x1, -draw_ctx->buf_area->y1);
    lv_coord_t dst_w     = lv_area_get_width(draw_ctx->buf_area);
    lv_coord_t dst_h     = lv_area_get_height(draw_ctx->buf_area);
    lv_coord_t clipped_w = lv_area_get_width(&clipped_coords);
    lv_coord_t clipped_h = lv_area_get_height(&clipped_coords);

    nema_cmdlist_t sCL;
    lv_grad_dir_t grad_dir = dsc->bg_grad.dir;
    lv_color_t bg_color    = grad_dir == LV_GRAD_DIR_NONE ? dsc->bg_color : dsc->bg_grad.stops[0].color;
    uint32_t bg_color_argb = C32A(bg_color, dsc->bg_opa);
    uint32_t x_op          = NEMA_BLOP_NONE;

    sCL = nema_cl_create();
    nema_cl_bind(&sCL);
    nema_set_clip(clipped_coords.x1, clipped_coords.y1, clipped_w, clipped_h);
    nema_bind_dst_tex(draw_ctx->buf, dst_w, dst_h, NEMA_COLOR_MODE, -1);

    if(lv_color_eq(bg_color, dsc->bg_grad.stops[1].color)) grad_dir = LV_GRAD_DIR_NONE;

    if (grad_dir != LV_GRAD_DIR_NONE) {
        if (dsc->bg_grad.stops_count == 2 && dsc->bg_grad.stops[1].frac == 255) {
            lv_color32_t c0 = lv_color_to32(dsc->bg_grad.stops[0].color);
            lv_color32_t c1 = lv_color_to32(dsc->bg_grad.stops[1].color);
            color_var_t c0v = { .r = c0.red, .g = c0.green, .b = c0.blue };
            color_var_t c1v = { .r = c1.red, .g = c1.green, .b = c1.blue };
            nema_enable_gradient(1);
            if (grad_dir == LV_GRAD_DIR_HOR) {
                nema_interpolate_rect_colors(bg_coords.x1, bg_coords.y1, w, h, &c0v, &c1v, &c1v);
            } else {
                nema_interpolate_rect_colors(bg_coords.x1, bg_coords.y1, w, h, &c0v, &c0v, &c1v);
            }
        }
    }
    if (mask) {
        nema_bind_tex(NEMA_TEX3, mask, w, h, NEMA_A8, -1, NEMA_FILTER_PS);
        x_op |= NEMA_BLOP_STENCIL_TXTY;
    }
    nema_set_blend_fill(nema_blending_mode(NEMA_BF_SRCALPHA, NEMA_BF_INVSRCALPHA, x_op));
    if (dsc->radius != 0) {
        nema_fill_rounded_rect(bg_coords.x1, bg_coords.y1, w, h, dsc->radius, bg_color_argb);
    } else {
        nema_fill_rect(bg_coords.x1, bg_coords.y1, w, h, bg_color_argb);
    }
    nema_cl_submit(&sCL);
    nema_cl_wait(&sCL);
    nema_cl_rewind(&sCL);
    nema_cl_destroy(&sCL);
}


static void draw_border(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords, lv_opa_t * mask)
{
    if(dsc->border_opa <= LV_OPA_MIN) return;
    if(dsc->border_width == 0) return;
    if(dsc->border_side == LV_BORDER_SIDE_NONE) return;
    if(dsc->border_post) return;

    int32_t coords_w = lv_area_get_width(coords);
    int32_t coords_h = lv_area_get_height(coords);
    int32_t rout = dsc->radius;
    int32_t short_side = LV_MIN(coords_w, coords_h);
    int32_t width = dsc->border_width;
    if(rout > short_side >> 1) rout = short_side >> 1;
    int32_t clip_ofs = LV_MAX(rout, width);

    /*Get the inner area*/
    lv_area_t clip_area;
    lv_area_copy(&clip_area, coords);
    clip_area.x1 += (dsc->border_side & LV_BORDER_SIDE_LEFT) ? 0 : clip_ofs;
    clip_area.x2 -= (dsc->border_side & LV_BORDER_SIDE_RIGHT) ? 0 : clip_ofs;
    clip_area.y1 += (dsc->border_side & LV_BORDER_SIDE_TOP) ? 0 : clip_ofs;
    clip_area.y2 -= (dsc->border_side & LV_BORDER_SIDE_BOTTOM) ? 0 : clip_ofs;

    draw_border_gpu(draw_ctx, coords, &clip_area, rout, width, dsc->border_color, dsc->border_opa);

}

static void draw_outline(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords, lv_opa_t * mask)
{
    if(dsc->outline_opa <= LV_OPA_MIN) return;
    if(dsc->outline_width == 0) return;

    lv_opa_t opa = dsc->outline_opa;

    if(opa > LV_OPA_MAX) opa = LV_OPA_COVER;

    /*Get the inner radius*/
    lv_area_t area_inner;
    lv_area_copy(&area_inner, coords);

    /*Bring the outline closer to make sure there is no color bleeding with pad=0*/
    lv_coord_t pad = dsc->outline_pad - 1;
    area_inner.x1 -= pad;
    area_inner.y1 -= pad;
    area_inner.x2 += pad;
    area_inner.y2 += pad;

    lv_area_t area_outer;
    lv_area_copy(&area_outer, &area_inner);

    area_outer.x1 -= dsc->outline_width;
    area_outer.x2 += dsc->outline_width;
    area_outer.y1 -= dsc->outline_width;
    area_outer.y2 += dsc->outline_width;


    int32_t inner_w = lv_area_get_width(&area_inner);
    int32_t inner_h = lv_area_get_height(&area_inner);
    int32_t rin = dsc->radius;
    int32_t short_side = LV_MIN(inner_w, inner_h);
    if(rin > short_side >> 1) rin = short_side >> 1;

    lv_coord_t rout = rin + dsc->outline_width;

    draw_border_gpu(draw_ctx, &area_outer, &area_outer, rout, dsc->outline_width, dsc->outline_color, dsc->outline_opa);
}

static void draw_border_gpu(lv_draw_ctx_t * draw_ctx, const lv_area_t * outer_area, const lv_area_t * clip_area,
                            int32_t rout, int32_t width, lv_color_t color, lv_opa_t opa)
{
    lv_area_t clipped_coords;
    if(!_lv_area_intersect(&clipped_coords, clip_area, draw_ctx->clip_area)) return;

    lv_coord_t dst_w     = lv_area_get_width(draw_ctx->buf_area);
    lv_coord_t dst_h     = lv_area_get_height(draw_ctx->buf_area);
    lv_coord_t clipped_w = lv_area_get_width(&clipped_coords);
    lv_coord_t clipped_h = lv_area_get_height(&clipped_coords);
    lv_area_t draw_area;
    uint32_t c32a = C32A(color, opa);
    nema_cmdlist_t sCL;

    sCL = nema_cl_create();
    nema_cl_bind(&sCL);
    lv_area_copy(&draw_area, outer_area);
    lv_coord_t draw_w = lv_area_get_width(&draw_area);
    lv_coord_t draw_h = lv_area_get_height(&draw_area);
    lv_area_move(&draw_area, -draw_ctx->buf_area->x1, -draw_ctx->buf_area->y1);
    lv_area_move(&clipped_coords, -draw_ctx->buf_area->x1, -draw_ctx->buf_area->y1);
    nema_set_clip(clipped_coords.x1, clipped_coords.y1, clipped_w, clipped_h);
    nema_bind_dst_tex(draw_ctx->buf, dst_w, dst_h, NEMA_COLOR_MODE, -1);
    nema_set_blend_fill(NEMA_BL_SIMPLE);

    /* NemaGFX only provides API for drawing rounded rect with 1px width, uhhh */
    for (size_t i = 0; i < width; i++) {
        nema_draw_rounded_rect(draw_area.x1 + i, draw_area.y1 + i, draw_w - i * 2, draw_h - i * 2, LV_MAX(0, rout - i), c32a);
    }

    nema_cl_submit(&sCL);
    nema_cl_wait(&sCL);
    nema_cl_rewind(&sCL);
    nema_cl_destroy(&sCL);
}

LV_ATTRIBUTE_FAST_MEM static void draw_shadow(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc,
                                              const lv_area_t * coords, lv_opa_t * mask)
{
    /*Check whether the shadow is visible*/
    if(dsc->shadow_width == 0) return;
    if(dsc->shadow_opa <= LV_OPA_MIN) return;

    if(dsc->shadow_width == 1 && dsc->shadow_spread <= 0 &&
       dsc->shadow_ofs_x == 0 && dsc->shadow_ofs_y == 0) {
        return;
    }

    /*Calculate the rectangle which is blurred to get the shadow in `shadow_area`*/
    lv_area_t core_area;
    core_area.x1 = coords->x1  + dsc->shadow_ofs_x - dsc->shadow_spread;
    core_area.x2 = coords->x2  + dsc->shadow_ofs_x + dsc->shadow_spread;
    core_area.y1 = coords->y1  + dsc->shadow_ofs_y - dsc->shadow_spread;
    core_area.y2 = coords->y2  + dsc->shadow_ofs_y + dsc->shadow_spread;

    /*Calculate the bounding box of the shadow*/
    lv_area_t shadow_area;
    shadow_area.x1 = core_area.x1 - dsc->shadow_width / 2 - 1;
    shadow_area.x2 = core_area.x2 + dsc->shadow_width / 2 + 1;
    shadow_area.y1 = core_area.y1 - dsc->shadow_width / 2 - 1;
    shadow_area.y2 = core_area.y2 + dsc->shadow_width / 2 + 1;

    lv_opa_t opa = dsc->shadow_opa;
    if(opa > LV_OPA_MAX) opa = LV_OPA_COVER;

    /*Get clipped draw area which is the real draw area.
     *It is always the same or inside `shadow_area`*/
    lv_area_t draw_area;
    if(!_lv_area_intersect(&draw_area, &shadow_area, draw_ctx->clip_area)) return;

    /*Consider 1 px smaller bg to be sure the edge will be covered by the shadow*/
    lv_area_t bg_area;
    lv_area_copy(&bg_area, coords);
    lv_area_increase(&bg_area, -1, -1);

    /*Get the clamped radius*/
    int32_t r_bg = dsc->radius;
    lv_coord_t short_side = LV_MIN(lv_area_get_width(&bg_area), lv_area_get_height(&bg_area));
    if(r_bg > short_side >> 1) r_bg = short_side >> 1;

    /*Get the clamped radius*/
    int32_t r_sh = dsc->radius;
    short_side = LV_MIN(lv_area_get_width(&core_area), lv_area_get_height(&core_area));
    if(r_sh > short_side >> 1) r_sh = short_side >> 1;


    /*Get how many pixels are affected by the blur on the corners*/
    int32_t corner_size = dsc->shadow_width  + r_sh;

    lv_opa_t * sh_buf;

#if LV_DRAW_SW_SHADOW_CACHE_SIZE
    if(sh_cache_size == corner_size && sh_cache_r == r_sh) {
        /*Use the cache if available*/
        sh_buf = nema_malloc(corner_size * corner_size);
        lv_memcpy(sh_buf, sh_cache, corner_size * corner_size);
    }
    else {
        /*A larger buffer is required for calculation*/
        sh_buf = nema_malloc(corner_size * corner_size * sizeof(uint16_t));
        shadow_draw_corner_buf(&core_area, (uint16_t *)sh_buf, dsc->shadow_width, r_sh);

        /*Cache the corner if it fits into the cache size*/
        if((uint32_t)corner_size * corner_size < sizeof(sh_cache)) {
            lv_memcpy(sh_cache, sh_buf, corner_size * corner_size);
            sh_cache_size = corner_size;
            sh_cache_r = r_sh;
        }
    }
#else
    sh_buf = nema_malloc(corner_size * corner_size * sizeof(uint16_t));
    shadow_draw_corner_buf(&core_area, (uint16_t *)sh_buf, dsc->shadow_width, r_sh);
#endif

    /*Skip a lot of masking if the background will cover the shadow that would be masked out*/
    bool mask_any = lv_draw_mask_is_any(&shadow_area);
    bool simple = true;
    if(mask_any || dsc->bg_opa < LV_OPA_COVER || dsc->blend_mode != LV_BLEND_MODE_NORMAL) simple = false;

    /*Create a radius mask to clip remove shadow on the bg area*/

    lv_draw_mask_radius_param_t mask_rout_param;
    int16_t mask_rout_id = LV_MASK_ID_INV;
    if(!simple) {
        lv_draw_mask_radius_init(&mask_rout_param, &bg_area, r_bg, true);
        mask_rout_id = lv_draw_mask_add(&mask_rout_param, NULL);
    }
    lv_opa_t * mask_buf = nema_malloc(lv_area_get_width(&shadow_area));
    lv_area_t blend_area;
    lv_area_t clip_area_sub;
    lv_opa_t * sh_buf_tmp;
    lv_coord_t y;
    bool simple_sub;

    lv_draw_sw_blend_dsc_t blend_dsc;
    lv_memzero(&blend_dsc, sizeof(blend_dsc));
    blend_dsc.blend_area = &blend_area;
    blend_dsc.mask_area = &blend_area;
    blend_dsc.mask_buf = mask_buf;
    blend_dsc.color = dsc->shadow_color;
    blend_dsc.opa = dsc->shadow_opa;
    blend_dsc.blend_mode = dsc->blend_mode;

    lv_coord_t w_half = shadow_area.x1 + lv_area_get_width(&shadow_area) / 2;
    lv_coord_t h_half = shadow_area.y1 + lv_area_get_height(&shadow_area) / 2;

    /*Draw the corners if they are on the current clip area and not fully covered by the bg*/

    /*Top right corner*/
    blend_area.x2 = shadow_area.x2;
    blend_area.x1 = shadow_area.x2 - corner_size + 1;
    blend_area.y1 = shadow_area.y1;
    blend_area.y2 = shadow_area.y1 + corner_size - 1;
    /*Do not overdraw the other top corners*/
    blend_area.x1 = LV_MAX(blend_area.x1, w_half);
    blend_area.y2 = LV_MIN(blend_area.y2, h_half);

    if(_lv_area_intersect(&clip_area_sub, &blend_area, draw_ctx->clip_area) &&
       !_lv_area_is_in(&clip_area_sub, &bg_area, r_bg)) {
        lv_coord_t w = lv_area_get_width(&clip_area_sub);
        sh_buf_tmp = sh_buf;
        sh_buf_tmp += (clip_area_sub.y1 - shadow_area.y1) * corner_size;
        sh_buf_tmp += clip_area_sub.x1 - (shadow_area.x2 - corner_size + 1);

        /*Do not mask if out of the bg*/
        if(simple && _lv_area_is_out(&clip_area_sub, &bg_area, r_bg)) simple_sub = true;
        else simple_sub = simple;
        if(w > 0) {
            blend_dsc.mask_buf = mask_buf;
            blend_area.x1 = clip_area_sub.x1;
            blend_area.x2 = clip_area_sub.x2;
            blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;    /*In simple mode it won't be overwritten*/
            for(y = clip_area_sub.y1; y <= clip_area_sub.y2; y++) {
                blend_area.y1 = y;
                blend_area.y2 = y;

                if(!simple_sub) {
                    lv_memcpy(mask_buf, sh_buf_tmp, corner_size);
                    blend_dsc.mask_res = lv_draw_mask_apply(mask_buf, clip_area_sub.x1, y, w);
                    if(blend_dsc.mask_res == LV_DRAW_MASK_RES_FULL_COVER) blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;
                }
                else {
                    blend_dsc.mask_buf = sh_buf_tmp;
                }
                lv_draw_sw_blend(draw_ctx, &blend_dsc);
                sh_buf_tmp += corner_size;
            }
        }
    }

    /*Bottom right corner.
     *Almost the same as top right just read the lines of `sh_buf` from then end*/
    blend_area.x2 = shadow_area.x2;
    blend_area.x1 = shadow_area.x2 - corner_size + 1;
    blend_area.y1 = shadow_area.y2 - corner_size + 1;
    blend_area.y2 = shadow_area.y2;
    /*Do not overdraw the other corners*/
    blend_area.x1 = LV_MAX(blend_area.x1, w_half);
    blend_area.y1 = LV_MAX(blend_area.y1, h_half + 1);

    if(_lv_area_intersect(&clip_area_sub, &blend_area, draw_ctx->clip_area) &&
       !_lv_area_is_in(&clip_area_sub, &bg_area, r_bg)) {
        lv_coord_t w = lv_area_get_width(&clip_area_sub);
        sh_buf_tmp = sh_buf;
        sh_buf_tmp += (blend_area.y2 - clip_area_sub.y2) * corner_size;
        sh_buf_tmp += clip_area_sub.x1 - (shadow_area.x2 - corner_size + 1);
        /*Do not mask if out of the bg*/
        if(simple && _lv_area_is_out(&clip_area_sub, &bg_area, r_bg)) simple_sub = true;
        else simple_sub = simple;

        if(w > 0) {
            blend_dsc.mask_buf = mask_buf;
            blend_area.x1 = clip_area_sub.x1;
            blend_area.x2 = clip_area_sub.x2;
            blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;    /*In simple mode it won't be overwritten*/
            for(y = clip_area_sub.y2; y >= clip_area_sub.y1; y--) {
                blend_area.y1 = y;
                blend_area.y2 = y;

                if(!simple_sub) {
                    lv_memcpy(mask_buf, sh_buf_tmp, corner_size);
                    blend_dsc.mask_res = lv_draw_mask_apply(mask_buf, clip_area_sub.x1, y, w);
                    if(blend_dsc.mask_res == LV_DRAW_MASK_RES_FULL_COVER) blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;
                }
                else {
                    blend_dsc.mask_buf = sh_buf_tmp;
                }
                lv_draw_sw_blend(draw_ctx, &blend_dsc);
                sh_buf_tmp += corner_size;
            }
        }
    }

    /*Top side*/
    blend_area.x1 = shadow_area.x1 + corner_size;
    blend_area.x2 = shadow_area.x2 - corner_size;
    blend_area.y1 = shadow_area.y1;
    blend_area.y2 = shadow_area.y1 + corner_size - 1;
    blend_area.y2 = LV_MIN(blend_area.y2, h_half);

    if(_lv_area_intersect(&clip_area_sub, &blend_area, draw_ctx->clip_area) &&
       !_lv_area_is_in(&clip_area_sub, &bg_area, r_bg)) {
        lv_coord_t w = lv_area_get_width(&clip_area_sub);
        sh_buf_tmp = sh_buf;
        sh_buf_tmp += (clip_area_sub.y1 - blend_area.y1) * corner_size;

        /*Do not mask if out of the bg*/
        if(simple && _lv_area_is_out(&clip_area_sub, &bg_area, r_bg)) simple_sub = true;
        else simple_sub = simple;

        if(w > 0) {
            if(!simple_sub) {
                blend_dsc.mask_buf = mask_buf;
            }
            else {
                blend_dsc.mask_buf = NULL;
            }
            blend_area.x1 = clip_area_sub.x1;
            blend_area.x2 = clip_area_sub.x2;

            for(y = clip_area_sub.y1; y <= clip_area_sub.y2; y++) {
                blend_area.y1 = y;
                blend_area.y2 = y;

                if(!simple_sub) {
                    lv_memset(mask_buf, sh_buf_tmp[0], w);
                    blend_dsc.mask_res = lv_draw_mask_apply(mask_buf, clip_area_sub.x1, y, w);
                    if(blend_dsc.mask_res == LV_DRAW_MASK_RES_FULL_COVER) blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;
                    lv_draw_sw_blend(draw_ctx, &blend_dsc);
                }
                else {
                    blend_dsc.opa = opa == LV_OPA_COVER ? sh_buf_tmp[0] : (sh_buf_tmp[0] * dsc->shadow_opa) >> 8;
                    lv_draw_sw_blend(draw_ctx, &blend_dsc);
                }
                sh_buf_tmp += corner_size;
            }
        }
    }
    blend_dsc.opa = dsc->shadow_opa;    /*Restore*/

    /*Bottom side*/
    blend_area.x1 = shadow_area.x1 + corner_size;
    blend_area.x2 = shadow_area.x2 - corner_size;
    blend_area.y1 = shadow_area.y2 - corner_size + 1;
    blend_area.y2 = shadow_area.y2;
    blend_area.y1 = LV_MAX(blend_area.y1, h_half + 1);


    if(_lv_area_intersect(&clip_area_sub, &blend_area, draw_ctx->clip_area) &&
       !_lv_area_is_in(&clip_area_sub, &bg_area, r_bg)) {
        lv_coord_t w = lv_area_get_width(&clip_area_sub);
        sh_buf_tmp = sh_buf;
        sh_buf_tmp += (blend_area.y2 - clip_area_sub.y2) * corner_size;
        if(w > 0) {
            /*Do not mask if out of the bg*/
            if(simple && _lv_area_is_out(&clip_area_sub, &bg_area, r_bg)) simple_sub = true;
            else simple_sub = simple;

            if(!simple_sub) {
                blend_dsc.mask_buf = mask_buf;
            }
            else {
                blend_dsc.mask_buf = NULL;
            }
            blend_area.x1 = clip_area_sub.x1;
            blend_area.x2 = clip_area_sub.x2;

            for(y = clip_area_sub.y2; y >= clip_area_sub.y1; y--) {
                blend_area.y1 = y;
                blend_area.y2 = y;

                /*Do not mask if out of the bg*/
                if(simple && _lv_area_is_out(&clip_area_sub, &bg_area, r_bg)) simple_sub = true;
                else simple_sub = simple;

                if(!simple_sub) {
                    lv_memset(mask_buf, sh_buf_tmp[0], w);
                    blend_dsc.mask_res = lv_draw_mask_apply(mask_buf, clip_area_sub.x1, y, w);
                    if(blend_dsc.mask_res == LV_DRAW_MASK_RES_FULL_COVER) blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;
                    lv_draw_sw_blend(draw_ctx, &blend_dsc);
                }
                else {
                    blend_dsc.opa = opa == LV_OPA_COVER ? sh_buf_tmp[0] : (sh_buf_tmp[0] * dsc->shadow_opa) >> 8;
                    lv_draw_sw_blend(draw_ctx, &blend_dsc);

                }
                sh_buf_tmp += corner_size;
            }
        }
    }

    blend_dsc.opa = dsc->shadow_opa;    /*Restore*/

    /*Right side*/
    blend_area.x1 = shadow_area.x2 - corner_size + 1;
    blend_area.x2 = shadow_area.x2;
    blend_area.y1 = shadow_area.y1 + corner_size;
    blend_area.y2 = shadow_area.y2 - corner_size;
    /*Do not overdraw the other corners*/
    blend_area.y1 = LV_MIN(blend_area.y1, h_half + 1);
    blend_area.y2 = LV_MAX(blend_area.y2, h_half);
    blend_area.x1 = LV_MAX(blend_area.x1, w_half);

    if(_lv_area_intersect(&clip_area_sub, &blend_area, draw_ctx->clip_area) &&
       !_lv_area_is_in(&clip_area_sub, &bg_area, r_bg)) {
        lv_coord_t w = lv_area_get_width(&clip_area_sub);
        sh_buf_tmp = sh_buf;
        sh_buf_tmp += (corner_size - 1) * corner_size;
        sh_buf_tmp += clip_area_sub.x1 - (shadow_area.x2 - corner_size + 1);

        /*Do not mask if out of the bg*/
        if(simple && _lv_area_is_out(&clip_area_sub, &bg_area, r_bg)) simple_sub = true;
        else simple_sub = simple;
        blend_dsc.mask_buf = simple_sub ? sh_buf_tmp : mask_buf;

        if(w > 0) {
            blend_area.x1 = clip_area_sub.x1;
            blend_area.x2 = clip_area_sub.x2;
            blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;    /*In simple mode it won't be overwritten*/
            for(y = clip_area_sub.y1; y <= clip_area_sub.y2; y++) {
                blend_area.y1 = y;
                blend_area.y2 = y;

                if(!simple_sub) {
                    lv_memcpy(mask_buf, sh_buf_tmp, w);
                    blend_dsc.mask_res = lv_draw_mask_apply(mask_buf, clip_area_sub.x1, y, w);
                    if(blend_dsc.mask_res == LV_DRAW_MASK_RES_FULL_COVER) blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;
                }
                lv_draw_sw_blend(draw_ctx, &blend_dsc);
            }
        }
    }

    /*Mirror the shadow corner buffer horizontally*/
    sh_buf_tmp = sh_buf ;
    for(y = 0; y < corner_size; y++) {
        int32_t x;
        lv_opa_t * start = sh_buf_tmp;
        lv_opa_t * end = sh_buf_tmp + corner_size - 1;
        for(x = 0; x < corner_size / 2; x++) {
            lv_opa_t tmp = *start;
            *start = *end;
            *end = tmp;

            start++;
            end--;
        }
        sh_buf_tmp += corner_size;
    }

    /*Left side*/
    blend_area.x1 = shadow_area.x1;
    blend_area.x2 = shadow_area.x1 + corner_size - 1;
    blend_area.y1 = shadow_area.y1 + corner_size;
    blend_area.y2 = shadow_area.y2 - corner_size;
    /*Do not overdraw the other corners*/
    blend_area.y1 = LV_MIN(blend_area.y1, h_half + 1);
    blend_area.y2 = LV_MAX(blend_area.y2, h_half);
    blend_area.x2 = LV_MIN(blend_area.x2, w_half - 1);

    if(_lv_area_intersect(&clip_area_sub, &blend_area, draw_ctx->clip_area) &&
       !_lv_area_is_in(&clip_area_sub, &bg_area, r_bg)) {
        lv_coord_t w = lv_area_get_width(&clip_area_sub);
        sh_buf_tmp = sh_buf;
        sh_buf_tmp += (corner_size - 1) * corner_size;
        sh_buf_tmp += clip_area_sub.x1 - blend_area.x1;

        /*Do not mask if out of the bg*/
        if(simple && _lv_area_is_out(&clip_area_sub, &bg_area, r_bg)) simple_sub = true;
        else simple_sub = simple;
        blend_dsc.mask_buf = simple_sub ? sh_buf_tmp : mask_buf;
        if(w > 0) {
            blend_area.x1 = clip_area_sub.x1;
            blend_area.x2 = clip_area_sub.x2;
            blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;    /*In simple mode it won't be overwritten*/
            for(y = clip_area_sub.y1; y <= clip_area_sub.y2; y++) {
                blend_area.y1 = y;
                blend_area.y2 = y;

                if(!simple_sub) {
                    lv_memcpy(mask_buf, sh_buf_tmp, w);
                    blend_dsc.mask_res = lv_draw_mask_apply(mask_buf, clip_area_sub.x1, y, w);
                    if(blend_dsc.mask_res == LV_DRAW_MASK_RES_FULL_COVER) blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;
                }

                lv_draw_sw_blend(draw_ctx, &blend_dsc);
            }
        }
    }

    /*Top left corner*/
    blend_area.x1 = shadow_area.x1;
    blend_area.x2 = shadow_area.x1 + corner_size - 1;
    blend_area.y1 = shadow_area.y1;
    blend_area.y2 = shadow_area.y1 + corner_size - 1;
    /*Do not overdraw the other corners*/
    blend_area.x2 = LV_MIN(blend_area.x2, w_half - 1);
    blend_area.y2 = LV_MIN(blend_area.y2, h_half);

    if(_lv_area_intersect(&clip_area_sub, &blend_area, draw_ctx->clip_area) &&
       !_lv_area_is_in(&clip_area_sub, &bg_area, r_bg)) {
        lv_coord_t w = lv_area_get_width(&clip_area_sub);
        sh_buf_tmp = sh_buf;
        sh_buf_tmp += (clip_area_sub.y1 - blend_area.y1) * corner_size;
        sh_buf_tmp += clip_area_sub.x1 - blend_area.x1;

        /*Do not mask if out of the bg*/
        if(simple && _lv_area_is_out(&clip_area_sub, &bg_area, r_bg)) simple_sub = true;
        else simple_sub = simple;
        blend_dsc.mask_buf = mask_buf;

        if(w > 0) {
            blend_area.x1 = clip_area_sub.x1;
            blend_area.x2 = clip_area_sub.x2;
            blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;    /*In simple mode it won't be overwritten*/
            for(y = clip_area_sub.y1; y <= clip_area_sub.y2; y++) {
                blend_area.y1 = y;
                blend_area.y2 = y;

                if(!simple_sub) {
                    lv_memcpy(mask_buf, sh_buf_tmp, corner_size);
                    blend_dsc.mask_res = lv_draw_mask_apply(mask_buf, clip_area_sub.x1, y, w);
                    if(blend_dsc.mask_res == LV_DRAW_MASK_RES_FULL_COVER) blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;
                }
                else {
                    blend_dsc.mask_buf = sh_buf_tmp;
                }

                lv_draw_sw_blend(draw_ctx, &blend_dsc);
                sh_buf_tmp += corner_size;
            }
        }
    }

    /*Bottom left corner.
     *Almost the same as bottom right just read the lines of `sh_buf` from then end*/
    blend_area.x1 = shadow_area.x1 ;
    blend_area.x2 = shadow_area.x1 + corner_size - 1;
    blend_area.y1 = shadow_area.y2 - corner_size + 1;
    blend_area.y2 = shadow_area.y2;
    /*Do not overdraw the other corners*/
    blend_area.y1 = LV_MAX(blend_area.y1, h_half + 1);
    blend_area.x2 = LV_MIN(blend_area.x2, w_half - 1);

    if(_lv_area_intersect(&clip_area_sub, &blend_area, draw_ctx->clip_area) &&
       !_lv_area_is_in(&clip_area_sub, &bg_area, r_bg)) {
        lv_coord_t w = lv_area_get_width(&clip_area_sub);
        sh_buf_tmp = sh_buf;
        sh_buf_tmp += (blend_area.y2 - clip_area_sub.y2) * corner_size;
        sh_buf_tmp += clip_area_sub.x1 - blend_area.x1;

        /*Do not mask if out of the bg*/
        if(simple && _lv_area_is_out(&clip_area_sub, &bg_area, r_bg)) simple_sub = true;
        else simple_sub = simple;
        blend_dsc.mask_buf = mask_buf;
        if(w > 0) {
            blend_area.x1 = clip_area_sub.x1;
            blend_area.x2 = clip_area_sub.x2;
            blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;    /*In simple mode it won't be overwritten*/
            for(y = clip_area_sub.y2; y >= clip_area_sub.y1; y--) {
                blend_area.y1 = y;
                blend_area.y2 = y;

                if(!simple_sub) {
                    lv_memcpy(mask_buf, sh_buf_tmp, corner_size);
                    blend_dsc.mask_res = lv_draw_mask_apply(mask_buf, clip_area_sub.x1, y, w);
                    if(blend_dsc.mask_res == LV_DRAW_MASK_RES_FULL_COVER) blend_dsc.mask_res = LV_DRAW_MASK_RES_CHANGED;
                }
                else {
                    blend_dsc.mask_buf = sh_buf_tmp;
                }
                lv_draw_sw_blend(draw_ctx, &blend_dsc);
                sh_buf_tmp += corner_size;
            }
        }
    }

    /*Draw the center rectangle.*/
    blend_area.x1 = shadow_area.x1 + corner_size ;
    blend_area.x2 = shadow_area.x2 - corner_size;
    blend_area.y1 = shadow_area.y1 + corner_size;
    blend_area.y2 = shadow_area.y2 - corner_size;
    blend_dsc.mask_buf = mask_buf;

    if(_lv_area_intersect(&clip_area_sub, &blend_area, draw_ctx->clip_area) &&
       !_lv_area_is_in(&clip_area_sub, &bg_area, r_bg)) {
        lv_coord_t w = lv_area_get_width(&clip_area_sub);
        if(w > 0) {
            blend_area.x1 = clip_area_sub.x1;
            blend_area.x2 = clip_area_sub.x2;
            for(y = clip_area_sub.y1; y <= clip_area_sub.y2; y++) {
                blend_area.y1 = y;
                blend_area.y2 = y;

                lv_memset(mask_buf, 0xff, w);
                blend_dsc.mask_res = lv_draw_mask_apply(mask_buf, clip_area_sub.x1, y, w);
                lv_draw_sw_blend(draw_ctx, &blend_dsc);
            }
        }
    }

    if(!simple) {
        lv_draw_mask_free_param(&mask_rout_param);
        lv_draw_mask_remove_id(mask_rout_id);
    }
    nema_free(sh_buf);
    nema_free(mask_buf);
}

/**
 * Calculate a blurred corner
 * @param coords Coordinates of the shadow
 * @param sh_buf a buffer to store the result. Its size should be `(sw + r)^2 * 2`
 * @param sw shadow width
 * @param r radius
 */
LV_ATTRIBUTE_FAST_MEM static void shadow_draw_corner_buf(const lv_area_t * coords, uint16_t * sh_buf, lv_coord_t sw,
                                                         lv_coord_t r)
{
    int32_t sw_ori = sw;
    int32_t size = sw_ori  + r;

    lv_area_t sh_area;
    lv_area_copy(&sh_area, coords);
    sh_area.x2 = sw / 2 + r - 1  - ((sw & 1) ? 0 : 1);
    sh_area.y1 = sw / 2 + 1;

    sh_area.x1 = sh_area.x2 - lv_area_get_width(coords);
    sh_area.y2 = sh_area.y1 + lv_area_get_height(coords);

    lv_draw_mask_radius_param_t mask_param;
    lv_draw_mask_radius_init(&mask_param, &sh_area, r, false);

#if SHADOW_ENHANCE
    /*Set half shadow width width because blur will be repeated*/
    if(sw_ori == 1) sw = 1;
    else sw = sw_ori >> 1;
#endif

    int32_t y;
    lv_opa_t * mask_line = lv_malloc(size);
    uint16_t * sh_ups_tmp_buf = (uint16_t *)sh_buf;
    for(y = 0; y < size; y++) {
        lv_memset(mask_line, 0xff, size);
        lv_draw_mask_res_t mask_res = mask_param.dsc.cb(mask_line, 0, y, size, &mask_param);
        if(mask_res == LV_DRAW_MASK_RES_TRANSP) {
            lv_memzero(sh_ups_tmp_buf, size * sizeof(sh_ups_tmp_buf[0]));
        }
        else {
            int32_t i;
            sh_ups_tmp_buf[0] = (mask_line[0] << SHADOW_UPSCALE_SHIFT) / sw;
            for(i = 1; i < size; i++) {
                if(mask_line[i] == mask_line[i - 1]) sh_ups_tmp_buf[i] = sh_ups_tmp_buf[i - 1];
                else  sh_ups_tmp_buf[i] = (mask_line[i] << SHADOW_UPSCALE_SHIFT) / sw;
            }
        }

        sh_ups_tmp_buf += size;
    }
    lv_free(mask_line);

    lv_draw_mask_free_param(&mask_param);

    if(sw == 1) {
        int32_t i;
        lv_opa_t * res_buf = (lv_opa_t *)sh_buf;
        for(i = 0; i < size * size; i++) {
            res_buf[i] = (sh_buf[i] >> SHADOW_UPSCALE_SHIFT);
        }
        return;
    }

    shadow_blur_corner(size, sw, sh_buf);

#if SHADOW_ENHANCE == 0
    /*The result is required in lv_opa_t not uint16_t*/
    uint32_t x;
    lv_opa_t * res_buf = (lv_opa_t *)sh_buf;
    for(x = 0; x < size * size; x++) {
        res_buf[x] = sh_buf[x];
    }
#else
    sw += sw_ori & 1;
    if(sw > 1) {
        uint32_t i;
        uint32_t max_v_div = (LV_OPA_COVER << SHADOW_UPSCALE_SHIFT) / sw;
        for(i = 0; i < (uint32_t)size * size; i++) {
            if(sh_buf[i] == 0) continue;
            else if(sh_buf[i] == LV_OPA_COVER) sh_buf[i] = max_v_div;
            else  sh_buf[i] = (sh_buf[i] << SHADOW_UPSCALE_SHIFT) / sw;
        }

        shadow_blur_corner(size, sw, sh_buf);
    }
    int32_t x;
    lv_opa_t * res_buf = (lv_opa_t *)sh_buf;
    for(x = 0; x < size * size; x++) {
        res_buf[x] = sh_buf[x];
    }
#endif

}

LV_ATTRIBUTE_FAST_MEM static void shadow_blur_corner(lv_coord_t size, lv_coord_t sw, uint16_t * sh_ups_buf)
{
    int32_t s_left = sw >> 1;
    int32_t s_right = (sw >> 1);
    if((sw & 1) == 0) s_left--;

    /*Horizontal blur*/
    uint16_t * sh_ups_blur_buf = lv_malloc(size * sizeof(uint16_t));

    int32_t x;
    int32_t y;

    uint16_t * sh_ups_tmp_buf = sh_ups_buf;

    for(y = 0; y < size; y++) {
        int32_t v = sh_ups_tmp_buf[size - 1] * sw;
        for(x = size - 1; x >= 0; x--) {
            sh_ups_blur_buf[x] = v;

            /*Forget the right pixel*/
            uint32_t right_val = 0;
            if(x + s_right < size) right_val = sh_ups_tmp_buf[x + s_right];
            v -= right_val;

            /*Add the left pixel*/
            uint32_t left_val;
            if(x - s_left - 1 < 0) left_val = sh_ups_tmp_buf[0];
            else left_val = sh_ups_tmp_buf[x - s_left - 1];
            v += left_val;
        }
        lv_memcpy(sh_ups_tmp_buf, sh_ups_blur_buf, size * sizeof(uint16_t));
        sh_ups_tmp_buf += size;
    }

    /*Vertical blur*/
    uint32_t i;
    uint32_t max_v = LV_OPA_COVER << SHADOW_UPSCALE_SHIFT;
    uint32_t max_v_div = max_v / sw;
    for(i = 0; i < (uint32_t)size * size; i++) {
        if(sh_ups_buf[i] == 0) continue;
        else if(sh_ups_buf[i] == max_v) sh_ups_buf[i] = max_v_div;
        else sh_ups_buf[i] = sh_ups_buf[i] / sw;
    }

    for(x = 0; x < size; x++) {
        sh_ups_tmp_buf = &sh_ups_buf[x];
        int32_t v = sh_ups_tmp_buf[0] * sw;
        for(y = 0; y < size ; y++, sh_ups_tmp_buf += size) {
            sh_ups_blur_buf[y] = v < 0 ? 0 : (v >> SHADOW_UPSCALE_SHIFT);

            /*Forget the top pixel*/
            uint32_t top_val;
            if(y - s_right <= 0) top_val = sh_ups_tmp_buf[0];
            else top_val = sh_ups_buf[(y - s_right) * size + x];
            v -= top_val;

            /*Add the bottom pixel*/
            uint32_t bottom_val;
            if(y + s_left + 1 < size) bottom_val = sh_ups_buf[(y + s_left + 1) * size + x];
            else bottom_val = sh_ups_buf[(size - 1) * size + x];
            v += bottom_val;
        }

        /*Write back the result into `sh_ups_buf`*/
        sh_ups_tmp_buf = &sh_ups_buf[x];
        for(y = 0; y < size; y++, sh_ups_tmp_buf += size) {
            (*sh_ups_tmp_buf) = sh_ups_blur_buf[y];
        }
    }

    lv_free(sh_ups_blur_buf);
}

static void draw_bg_img(lv_draw_ctx_t * draw_ctx, const lv_draw_rect_dsc_t * dsc, const lv_area_t * coords)
{
    if(dsc->bg_img_src == NULL) return;
    if(dsc->bg_img_opa <= LV_OPA_MIN) return;

    lv_area_t clip_area;
    if(!_lv_area_intersect(&clip_area, coords, draw_ctx->clip_area)) {
        return;
    }

    const lv_area_t * clip_area_ori = draw_ctx->clip_area;
    draw_ctx->clip_area = &clip_area;

    lv_img_src_t src_type = lv_img_src_get_type(dsc->bg_img_src);
    if(src_type == LV_IMG_SRC_SYMBOL) {
        lv_point_t size;
        lv_txt_get_size(&size, dsc->bg_img_src, dsc->bg_img_symbol_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_area_t a;
        a.x1 = coords->x1 + lv_area_get_width(coords) / 2 - size.x / 2;
        a.x2 = a.x1 + size.x - 1;
        a.y1 = coords->y1 + lv_area_get_height(coords) / 2 - size.y / 2;
        a.y2 = a.y1 + size.y - 1;

        lv_draw_label_dsc_t label_draw_dsc;
        lv_draw_label_dsc_init(&label_draw_dsc);
        label_draw_dsc.font = dsc->bg_img_symbol_font;
        label_draw_dsc.color = dsc->bg_img_recolor;
        label_draw_dsc.opa = dsc->bg_img_opa;
        lv_draw_label(draw_ctx, &label_draw_dsc, &a, dsc->bg_img_src, NULL);
    }
    else {
        lv_img_header_t header;
        lv_res_t res = lv_img_decoder_get_info(dsc->bg_img_src, &header);
        if(res == LV_RES_OK) {
            lv_draw_img_dsc_t img_dsc;
            lv_draw_img_dsc_init(&img_dsc);
            img_dsc.blend_mode = dsc->blend_mode;
            img_dsc.recolor = dsc->bg_img_recolor;
            img_dsc.recolor_opa = dsc->bg_img_recolor_opa;
            img_dsc.opa = dsc->bg_img_opa;

            /*Center align*/
            if(dsc->bg_img_tiled == false) {
                lv_area_t area;
                area.x1 = coords->x1 + lv_area_get_width(coords) / 2 - header.w / 2;
                area.y1 = coords->y1 + lv_area_get_height(coords) / 2 - header.h / 2;
                area.x2 = area.x1 + header.w - 1;
                area.y2 = area.y1 + header.h - 1;

                lv_draw_img(draw_ctx, &img_dsc, &area, dsc->bg_img_src);
            }
            else {
                lv_area_t area;
                area.y1 = coords->y1;
                area.y2 = area.y1 + header.h - 1;

                for(; area.y1 <= coords->y2; area.y1 += header.h, area.y2 += header.h) {

                    area.x1 = coords->x1;
                    area.x2 = area.x1 + header.w - 1;
                    for(; area.x1 <= coords->x2; area.x1 += header.w, area.x2 += header.w) {
                        lv_draw_img(draw_ctx, &img_dsc, &area, dsc->bg_img_src);
                    }
                }
            }
        }
        else {
            LV_LOG_WARN("Couldn't read the background image");
        }
    }

    draw_ctx->clip_area = clip_area_ori;
}

void lv_draw_ambiq_nema_init(void)
{
#ifndef NEMA_CONST_TEX
    g_tex = nema_malloc(sizeof(*g_tex));
    *g_tex = ~0;
#endif
}

void lv_draw_ambiq_nema_deinit(void)
{
#ifndef NEMA_CONST_TEX
    if (g_tex) nema_free(g_tex);
#endif
}
#endif /* LV_USE_GPU_AMBIQ_NEMA */
