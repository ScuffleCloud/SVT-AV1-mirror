/*
 * Copyright (c) 2024, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <arm_neon.h>
#include <assert.h>
#include <stdbool.h>
#include <arm_neon_sve_bridge.h>

#include "common_dsp_rtcd.h"
#include "highbd_warp_plane_neon.h"
#include "mem_neon.h"
#include "neon_sve_bridge.h"
#include "transpose_neon.h"
#include "utility.h"
#include "warped_motion.h"

static AOM_FORCE_INLINE int16x8_t highbd_horizontal_filter_4x1_f4(int16x8_t rv0, int16x8_t rv1, int16x8_t rv2,
                                                                  int16x8_t rv3, int bd, int sx, int alpha) {
    int16x8_t f[4];
    load_filters_4(f, sx, alpha);

    int64x2_t m0 = svt_sdotq_s16(vdupq_n_s64(0), rv0, f[0]);
    int64x2_t m1 = svt_sdotq_s16(vdupq_n_s64(0), rv1, f[1]);
    int64x2_t m2 = svt_sdotq_s16(vdupq_n_s64(0), rv2, f[2]);
    int64x2_t m3 = svt_sdotq_s16(vdupq_n_s64(0), rv3, f[3]);

    int64x2_t m01 = vpaddq_s64(m0, m1);
    int64x2_t m23 = vpaddq_s64(m2, m3);

    const int offset_const = (1 << (bd + FILTER_BITS - 1)) + (1 << (ROUND0_BITS - 1));

    int32x4_t res = vcombine_s32(vmovn_s64(m01), vmovn_s64(m23));
    res           = vaddq_s32(res, vdupq_n_s32(offset_const));
    return vcombine_s16(vshrn_n_s32(res, ROUND0_BITS), vdup_n_s16(0));
}

static AOM_FORCE_INLINE int16x8_t highbd_horizontal_filter_8x1_f8(int16x8_t rv0, int16x8_t rv1, int16x8_t rv2,
                                                                  int16x8_t rv3, int16x8_t rv4, int16x8_t rv5,
                                                                  int16x8_t rv6, int16x8_t rv7, int bd, int sx,
                                                                  int alpha) {
    int16x8_t f[8];
    load_filters_8(f, sx, alpha);

    int64x2_t m0 = svt_sdotq_s16(vdupq_n_s64(0), rv0, f[0]);
    int64x2_t m1 = svt_sdotq_s16(vdupq_n_s64(0), rv1, f[1]);
    int64x2_t m2 = svt_sdotq_s16(vdupq_n_s64(0), rv2, f[2]);
    int64x2_t m3 = svt_sdotq_s16(vdupq_n_s64(0), rv3, f[3]);
    int64x2_t m4 = svt_sdotq_s16(vdupq_n_s64(0), rv4, f[4]);
    int64x2_t m5 = svt_sdotq_s16(vdupq_n_s64(0), rv5, f[5]);
    int64x2_t m6 = svt_sdotq_s16(vdupq_n_s64(0), rv6, f[6]);
    int64x2_t m7 = svt_sdotq_s16(vdupq_n_s64(0), rv7, f[7]);

    int64x2_t m01 = vpaddq_s64(m0, m1);
    int64x2_t m23 = vpaddq_s64(m2, m3);
    int64x2_t m45 = vpaddq_s64(m4, m5);
    int64x2_t m67 = vpaddq_s64(m6, m7);

    const int offset_const = (1 << (bd + FILTER_BITS - 1)) + (1 << (ROUND0_BITS - 1));

    int32x4_t res0 = vcombine_s32(vmovn_s64(m01), vmovn_s64(m23));
    int32x4_t res1 = vcombine_s32(vmovn_s64(m45), vmovn_s64(m67));
    res0           = vaddq_s32(res0, vdupq_n_s32(offset_const));
    res1           = vaddq_s32(res1, vdupq_n_s32(offset_const));
    return vcombine_s16(vshrn_n_s32(res0, ROUND0_BITS), vshrn_n_s32(res1, ROUND0_BITS));
}

static AOM_FORCE_INLINE int16x8_t highbd_horizontal_filter_4x1_f1(int16x8_t rv0, int16x8_t rv1, int16x8_t rv2,
                                                                  int16x8_t rv3, int bd, int sx) {
    int16x8_t f = load_filters_1(sx);

    int64x2_t m0 = svt_sdotq_s16(vdupq_n_s64(0), rv0, f);
    int64x2_t m1 = svt_sdotq_s16(vdupq_n_s64(0), rv1, f);
    int64x2_t m2 = svt_sdotq_s16(vdupq_n_s64(0), rv2, f);
    int64x2_t m3 = svt_sdotq_s16(vdupq_n_s64(0), rv3, f);

    int64x2_t m01 = vpaddq_s64(m0, m1);
    int64x2_t m23 = vpaddq_s64(m2, m3);

    const int offset_const = (1 << (bd + FILTER_BITS - 1)) + (1 << (ROUND0_BITS - 1));

    int32x4_t res = vcombine_s32(vmovn_s64(m01), vmovn_s64(m23));
    res           = vaddq_s32(res, vdupq_n_s32(offset_const));
    return vcombine_s16(vshrn_n_s32(res, ROUND0_BITS), vdup_n_s16(0));
}

static AOM_FORCE_INLINE int16x8_t highbd_horizontal_filter_8x1_f1(int16x8_t rv0, int16x8_t rv1, int16x8_t rv2,
                                                                  int16x8_t rv3, int16x8_t rv4, int16x8_t rv5,
                                                                  int16x8_t rv6, int16x8_t rv7, int bd, int sx) {
    int16x8_t f = load_filters_1(sx);

    int64x2_t m0 = svt_sdotq_s16(vdupq_n_s64(0), rv0, f);
    int64x2_t m1 = svt_sdotq_s16(vdupq_n_s64(0), rv1, f);
    int64x2_t m2 = svt_sdotq_s16(vdupq_n_s64(0), rv2, f);
    int64x2_t m3 = svt_sdotq_s16(vdupq_n_s64(0), rv3, f);
    int64x2_t m4 = svt_sdotq_s16(vdupq_n_s64(0), rv4, f);
    int64x2_t m5 = svt_sdotq_s16(vdupq_n_s64(0), rv5, f);
    int64x2_t m6 = svt_sdotq_s16(vdupq_n_s64(0), rv6, f);
    int64x2_t m7 = svt_sdotq_s16(vdupq_n_s64(0), rv7, f);

    int64x2_t m01 = vpaddq_s64(m0, m1);
    int64x2_t m23 = vpaddq_s64(m2, m3);
    int64x2_t m45 = vpaddq_s64(m4, m5);
    int64x2_t m67 = vpaddq_s64(m6, m7);

    const int offset_const = (1 << (bd + FILTER_BITS - 1)) + (1 << (ROUND0_BITS - 1));

    int32x4_t res0 = vcombine_s32(vmovn_s64(m01), vmovn_s64(m23));
    int32x4_t res1 = vcombine_s32(vmovn_s64(m45), vmovn_s64(m67));
    res0           = vaddq_s32(res0, vdupq_n_s32(offset_const));
    res1           = vaddq_s32(res1, vdupq_n_s32(offset_const));
    return vcombine_s16(vshrn_n_s32(res0, ROUND0_BITS), vshrn_n_s32(res1, ROUND0_BITS));
}

static AOM_FORCE_INLINE int32x4_t vertical_filter_4x1_f1(const int16x8_t *tmp, int sy) {
    const int16x8_t f     = load_filters_1(sy);
    const int16x4_t f0123 = vget_low_s16(f);
    const int16x4_t f4567 = vget_high_s16(f);

    // No benefit to using SDOT here, the cost of rearrangement is too high.
    int32x4_t m0123 = vmull_lane_s16(vget_low_s16(tmp[0]), f0123, 0);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[1]), f0123, 1);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[2]), f0123, 2);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[3]), f0123, 3);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[4]), f4567, 0);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[5]), f4567, 1);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[6]), f4567, 2);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[7]), f4567, 3);
    return m0123;
}

static AOM_FORCE_INLINE int32x4x2_t vertical_filter_8x1_f1(const int16x8_t *tmp, int sy) {
    const int16x8_t f     = load_filters_1(sy);
    const int16x4_t f0123 = vget_low_s16(f);
    const int16x4_t f4567 = vget_high_s16(f);

    // No benefit to using SDOT here, the cost of rearrangement is too high.
    int32x4_t m0123 = vmull_lane_s16(vget_low_s16(tmp[0]), f0123, 0);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[1]), f0123, 1);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[2]), f0123, 2);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[3]), f0123, 3);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[4]), f4567, 0);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[5]), f4567, 1);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[6]), f4567, 2);
    m0123           = vmlal_lane_s16(m0123, vget_low_s16(tmp[7]), f4567, 3);

    int32x4_t m4567 = vmull_lane_s16(vget_high_s16(tmp[0]), f0123, 0);
    m4567           = vmlal_lane_s16(m4567, vget_high_s16(tmp[1]), f0123, 1);
    m4567           = vmlal_lane_s16(m4567, vget_high_s16(tmp[2]), f0123, 2);
    m4567           = vmlal_lane_s16(m4567, vget_high_s16(tmp[3]), f0123, 3);
    m4567           = vmlal_lane_s16(m4567, vget_high_s16(tmp[4]), f4567, 0);
    m4567           = vmlal_lane_s16(m4567, vget_high_s16(tmp[5]), f4567, 1);
    m4567           = vmlal_lane_s16(m4567, vget_high_s16(tmp[6]), f4567, 2);
    m4567           = vmlal_lane_s16(m4567, vget_high_s16(tmp[7]), f4567, 3);
    return (int32x4x2_t){{m0123, m4567}};
}

static AOM_FORCE_INLINE int32x4_t vertical_filter_4x1_f4(const int16x8_t *tmp, int sy, int gamma) {
    int16x8_t s0, s1, s2, s3;
    transpose_elems_s16_4x8(vget_low_s16(tmp[0]),
                            vget_low_s16(tmp[1]),
                            vget_low_s16(tmp[2]),
                            vget_low_s16(tmp[3]),
                            vget_low_s16(tmp[4]),
                            vget_low_s16(tmp[5]),
                            vget_low_s16(tmp[6]),
                            vget_low_s16(tmp[7]),
                            &s0,
                            &s1,
                            &s2,
                            &s3);

    int16x8_t f[4];
    load_filters_4(f, sy, gamma);

    int64x2_t m0 = svt_sdotq_s16(vdupq_n_s64(0), s0, f[0]);
    int64x2_t m1 = svt_sdotq_s16(vdupq_n_s64(0), s1, f[1]);
    int64x2_t m2 = svt_sdotq_s16(vdupq_n_s64(0), s2, f[2]);
    int64x2_t m3 = svt_sdotq_s16(vdupq_n_s64(0), s3, f[3]);

    int64x2_t m01 = vpaddq_s64(m0, m1);
    int64x2_t m23 = vpaddq_s64(m2, m3);
    return vcombine_s32(vmovn_s64(m01), vmovn_s64(m23));
}

static AOM_FORCE_INLINE int32x4x2_t vertical_filter_8x1_f8(const int16x8_t *tmp, int sy, int gamma) {
    int16x8_t s0 = tmp[0];
    int16x8_t s1 = tmp[1];
    int16x8_t s2 = tmp[2];
    int16x8_t s3 = tmp[3];
    int16x8_t s4 = tmp[4];
    int16x8_t s5 = tmp[5];
    int16x8_t s6 = tmp[6];
    int16x8_t s7 = tmp[7];
    transpose_elems_inplace_s16_8x8(&s0, &s1, &s2, &s3, &s4, &s5, &s6, &s7);

    int16x8_t f[8];
    load_filters_8(f, sy, gamma);

    int64x2_t m0 = svt_sdotq_s16(vdupq_n_s64(0), s0, f[0]);
    int64x2_t m1 = svt_sdotq_s16(vdupq_n_s64(0), s1, f[1]);
    int64x2_t m2 = svt_sdotq_s16(vdupq_n_s64(0), s2, f[2]);
    int64x2_t m3 = svt_sdotq_s16(vdupq_n_s64(0), s3, f[3]);
    int64x2_t m4 = svt_sdotq_s16(vdupq_n_s64(0), s4, f[4]);
    int64x2_t m5 = svt_sdotq_s16(vdupq_n_s64(0), s5, f[5]);
    int64x2_t m6 = svt_sdotq_s16(vdupq_n_s64(0), s6, f[6]);
    int64x2_t m7 = svt_sdotq_s16(vdupq_n_s64(0), s7, f[7]);

    int64x2_t m01 = vpaddq_s64(m0, m1);
    int64x2_t m23 = vpaddq_s64(m2, m3);
    int64x2_t m45 = vpaddq_s64(m4, m5);
    int64x2_t m67 = vpaddq_s64(m6, m7);

    int32x4x2_t ret;
    ret.val[0] = vcombine_s32(vmovn_s64(m01), vmovn_s64(m23));
    ret.val[1] = vcombine_s32(vmovn_s64(m45), vmovn_s64(m67));
    return ret;
}

void svt_av1_highbd_warp_affine_sve(const int32_t *mat, const uint8_t *ref8b, const uint8_t *ref2b, int width,
                                    int height, int stride8b, int stride2b, uint16_t *pred, int p_col, int p_row,
                                    int p_width, int p_height, int p_stride, int subsampling_x, int subsampling_y,
                                    int bd, ConvolveParams *conv_params, int16_t alpha, int16_t beta, int16_t gamma,
                                    int16_t delta) {
    highbd_warp_affine_common(mat,
                              ref8b,
                              ref2b,
                              width,
                              height,
                              stride8b,
                              stride2b,
                              pred,
                              p_col,
                              p_row,
                              p_width,
                              p_height,
                              p_stride,
                              subsampling_x,
                              subsampling_y,
                              bd,
                              conv_params,
                              alpha,
                              beta,
                              gamma,
                              delta);
}
