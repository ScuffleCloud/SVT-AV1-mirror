/*
 * Copyright (c) 2024, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
 */

#ifndef AOM_AV1_COMMON_ARM_CONVOLVE_NEON_H_
#define AOM_AV1_COMMON_ARM_CONVOLVE_NEON_H_

#include <arm_neon.h>

#include "inter_prediction.h"
#include "mem_neon.h"

DECLARE_ALIGNED(16, static const uint8_t, kDotProdPermuteTbl[48]) = {
    0, 1, 2, 3, 1, 2, 3, 4,  2, 3, 4,  5,  3, 4,  5,  6,  4,  5,  6,  7,  5,  6,  7,  8,
    6, 7, 8, 9, 7, 8, 9, 10, 8, 9, 10, 11, 9, 10, 11, 12, 10, 11, 12, 13, 11, 12, 13, 14};

static INLINE Bool is_convolve_2tap(const int16_t *const filter) {
    return (const void *)filter == (const void *)bilinear_filters;
}

static INLINE Bool is_convolve_4tap(const int16_t *const filter) {
    return (const void *)filter == (const void *)sub_pel_filters_4 ||
        (const void *)filter == (const void *)sub_pel_filters_4smooth;
}

static INLINE Bool is_convolve_6tap(const int16_t *const filter) {
    return (const void *)filter == (const void *)sub_pel_filters_8 ||
        (const void *)filter == (const void *)sub_pel_filters_8smooth;
}

static INLINE int32_t get_convolve_tap(const int16_t *const filter) {
    if (is_convolve_2tap(filter))
        return 2;
    else if (is_convolve_4tap(filter))
        return 4;
    else if (is_convolve_6tap(filter))
        return 6;
    else
        return 8;
}

static INLINE int16x4_t convolve4_4_2d_v(const int16x4_t s0, const int16x4_t s1, const int16x4_t s2, const int16x4_t s3,
                                         const int16x4_t y_filter) {
    int32x4_t sum = vmull_lane_s16(s0, y_filter, 0);
    sum           = vmlal_lane_s16(sum, s1, y_filter, 1);
    sum           = vmlal_lane_s16(sum, s2, y_filter, 2);
    sum           = vmlal_lane_s16(sum, s3, y_filter, 3);

    return vqrshrn_n_s32(sum, 2 * FILTER_BITS - ROUND0_BITS);
}

static INLINE uint8x8_t convolve4_8_2d_v(const int16x8_t s0, const int16x8_t s1, const int16x8_t s2, const int16x8_t s3,
                                         const int16x4_t y_filter, const int16x8_t sub_const) {
    int32x4_t sum0 = vmull_lane_s16(vget_low_s16(s0), y_filter, 0);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s1), y_filter, 1);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s2), y_filter, 2);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s3), y_filter, 3);

    int32x4_t sum1 = vmull_lane_s16(vget_high_s16(s0), y_filter, 0);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s1), y_filter, 1);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s2), y_filter, 2);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s3), y_filter, 3);

    int16x8_t res = vcombine_s16(vqrshrn_n_s32(sum0, 2 * FILTER_BITS - ROUND0_BITS),
                                 vqrshrn_n_s32(sum1, 2 * FILTER_BITS - ROUND0_BITS));
    res           = vsubq_s16(res, sub_const);

    return vqmovun_s16(res);
}

static INLINE void convolve_2d_sr_vert_4tap_neon(int16_t *src_ptr, int src_stride, uint8_t *dst_ptr, int dst_stride,
                                                 int w, int h, const int16_t *y_filter) {
    const int       bd        = 8;
    const int16x8_t sub_const = vdupq_n_s16(1 << (bd - 1));

    const int16x4_t filter = vld1_s16(y_filter + 2);

    if (w == 4) {
        int16x4_t s0, s1, s2;
        load_s16_4x3(src_ptr, src_stride, &s0, &s1, &s2);
        src_ptr += 3 * src_stride;

        do {
            int16x4_t s3, s4, s5, s6;
            load_s16_4x4(src_ptr, src_stride, &s3, &s4, &s5, &s6);

            int16x4_t d0 = convolve4_4_2d_v(s0, s1, s2, s3, filter);
            int16x4_t d1 = convolve4_4_2d_v(s1, s2, s3, s4, filter);
            int16x4_t d2 = convolve4_4_2d_v(s2, s3, s4, s5, filter);
            int16x4_t d3 = convolve4_4_2d_v(s3, s4, s5, s6, filter);

            uint8x8_t d01 = vqmovun_s16(vsubq_s16(vcombine_s16(d0, d1), sub_const));
            uint8x8_t d23 = vqmovun_s16(vsubq_s16(vcombine_s16(d2, d3), sub_const));

            store_u8x4_strided_x2(dst_ptr + 0 * dst_stride, dst_stride, d01);
            store_u8x4_strided_x2(dst_ptr + 2 * dst_stride, dst_stride, d23);

            s0 = s4;
            s1 = s5;
            s2 = s6;

            src_ptr += 4 * src_stride;
            dst_ptr += 4 * dst_stride;
            h -= 4;
        } while (h != 0);
    } else {
        // Width is a multiple of 8 and height is a multiple of 4.
        do {
            int      height = h;
            int16_t *s      = src_ptr;
            uint8_t *d      = dst_ptr;

            int16x8_t s0, s1, s2;
            load_s16_8x3(s, src_stride, &s0, &s1, &s2);
            s += 3 * src_stride;

            do {
                int16x8_t s3, s4, s5, s6;
                load_s16_8x4(s, src_stride, &s3, &s4, &s5, &s6);

                uint8x8_t d0 = convolve4_8_2d_v(s0, s1, s2, s3, filter, sub_const);
                uint8x8_t d1 = convolve4_8_2d_v(s1, s2, s3, s4, filter, sub_const);
                uint8x8_t d2 = convolve4_8_2d_v(s2, s3, s4, s5, filter, sub_const);
                uint8x8_t d3 = convolve4_8_2d_v(s3, s4, s5, s6, filter, sub_const);

                store_u8_8x4(d, dst_stride, d0, d1, d2, d3);

                s0 = s4;
                s1 = s5;
                s2 = s6;

                s += 4 * src_stride;
                d += 4 * dst_stride;
                height -= 4;
            } while (height != 0);
            src_ptr += 8;
            dst_ptr += 8;
            w -= 8;
        } while (w != 0);
    }
}

static INLINE int16x4_t convolve6_4_2d_v(const int16x4_t s0, const int16x4_t s1, const int16x4_t s2, const int16x4_t s3,
                                         const int16x4_t s4, const int16x4_t s5, const int16x8_t y_filter) {
    const int16x4_t y_filter_lo = vget_low_s16(y_filter);
    const int16x4_t y_filter_hi = vget_high_s16(y_filter);

    int32x4_t sum = vmull_lane_s16(s0, y_filter_lo, 1);
    sum           = vmlal_lane_s16(sum, s1, y_filter_lo, 2);
    sum           = vmlal_lane_s16(sum, s2, y_filter_lo, 3);
    sum           = vmlal_lane_s16(sum, s3, y_filter_hi, 0);
    sum           = vmlal_lane_s16(sum, s4, y_filter_hi, 1);
    sum           = vmlal_lane_s16(sum, s5, y_filter_hi, 2);

    return vqrshrn_n_s32(sum, 2 * FILTER_BITS - ROUND0_BITS);
}

static INLINE uint8x8_t convolve6_8_2d_v(const int16x8_t s0, const int16x8_t s1, const int16x8_t s2, const int16x8_t s3,
                                         const int16x8_t s4, const int16x8_t s5, const int16x8_t y_filter,
                                         const int16x8_t sub_const) {
    const int16x4_t y_filter_lo = vget_low_s16(y_filter);
    const int16x4_t y_filter_hi = vget_high_s16(y_filter);

    int32x4_t sum0 = vmull_lane_s16(vget_low_s16(s0), y_filter_lo, 1);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s1), y_filter_lo, 2);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s2), y_filter_lo, 3);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s3), y_filter_hi, 0);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s4), y_filter_hi, 1);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s5), y_filter_hi, 2);

    int32x4_t sum1 = vmull_lane_s16(vget_high_s16(s0), y_filter_lo, 1);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s1), y_filter_lo, 2);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s2), y_filter_lo, 3);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s3), y_filter_hi, 0);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s4), y_filter_hi, 1);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s5), y_filter_hi, 2);

    int16x8_t res = vcombine_s16(vqrshrn_n_s32(sum0, 2 * FILTER_BITS - ROUND0_BITS),
                                 vqrshrn_n_s32(sum1, 2 * FILTER_BITS - ROUND0_BITS));
    res           = vsubq_s16(res, sub_const);

    return vqmovun_s16(res);
}

static INLINE void convolve_2d_sr_vert_6tap_neon(int16_t *src_ptr, int src_stride, uint8_t *dst_ptr, int dst_stride,
                                                 int w, int h, const int16x8_t y_filter) {
    const int       bd        = 8;
    const int16x8_t sub_const = vdupq_n_s16(1 << (bd - 1));

    if (w <= 4) {
        int16x4_t s0, s1, s2, s3, s4;
        load_s16_4x5(src_ptr, src_stride, &s0, &s1, &s2, &s3, &s4);
        src_ptr += 5 * src_stride;

        do {
            int16x4_t s5, s6, s7, s8;
            load_s16_4x4(src_ptr, src_stride, &s5, &s6, &s7, &s8);

            int16x4_t d0 = convolve6_4_2d_v(s0, s1, s2, s3, s4, s5, y_filter);
            int16x4_t d1 = convolve6_4_2d_v(s1, s2, s3, s4, s5, s6, y_filter);
            int16x4_t d2 = convolve6_4_2d_v(s2, s3, s4, s5, s6, s7, y_filter);
            int16x4_t d3 = convolve6_4_2d_v(s3, s4, s5, s6, s7, s8, y_filter);

            uint8x8_t d01 = vqmovun_s16(vsubq_s16(vcombine_s16(d0, d1), sub_const));
            uint8x8_t d23 = vqmovun_s16(vsubq_s16(vcombine_s16(d2, d3), sub_const));

            store_u8_4x1(dst_ptr + 0 * dst_stride, d01, 0);
            store_u8_4x1(dst_ptr + 1 * dst_stride, d01, 1);
            store_u8_4x1(dst_ptr + 2 * dst_stride, d23, 0);
            store_u8_4x1(dst_ptr + 3 * dst_stride, d23, 1);

            s0 = s4;
            s1 = s5;
            s2 = s6;
            s3 = s7;
            s4 = s8;
            src_ptr += 4 * src_stride;
            dst_ptr += 4 * dst_stride;
            h -= 4;
        } while (h != 0);
    } else {
        // Width is a multiple of 8 and height is a multiple of 4.
        do {
            int      height = h;
            int16_t *s      = src_ptr;
            uint8_t *d      = dst_ptr;

            int16x8_t s0, s1, s2, s3, s4;
            load_s16_8x5(s, src_stride, &s0, &s1, &s2, &s3, &s4);
            s += 5 * src_stride;

            do {
                int16x8_t s5, s6, s7, s8;
                load_s16_8x4(s, src_stride, &s5, &s6, &s7, &s8);

                uint8x8_t d0 = convolve6_8_2d_v(s0, s1, s2, s3, s4, s5, y_filter, sub_const);
                uint8x8_t d1 = convolve6_8_2d_v(s1, s2, s3, s4, s5, s6, y_filter, sub_const);
                uint8x8_t d2 = convolve6_8_2d_v(s2, s3, s4, s5, s6, s7, y_filter, sub_const);
                uint8x8_t d3 = convolve6_8_2d_v(s3, s4, s5, s6, s7, s8, y_filter, sub_const);

                store_u8_8x4(d, dst_stride, d0, d1, d2, d3);

                s0 = s4;
                s1 = s5;
                s2 = s6;
                s3 = s7;
                s4 = s8;
                s += 4 * src_stride;
                d += 4 * dst_stride;
                height -= 4;
            } while (height != 0);
            src_ptr += 8;
            dst_ptr += 8;
            w -= 8;
        } while (w != 0);
    }
}

static INLINE int16x4_t convolve8_4_2d_v(const int16x4_t s0, const int16x4_t s1, const int16x4_t s2, const int16x4_t s3,
                                         const int16x4_t s4, const int16x4_t s5, const int16x4_t s6, const int16x4_t s7,
                                         const int16x8_t y_filter) {
    const int16x4_t y_filter_lo = vget_low_s16(y_filter);
    const int16x4_t y_filter_hi = vget_high_s16(y_filter);

    int32x4_t sum = vmull_lane_s16(s0, y_filter_lo, 0);
    sum           = vmlal_lane_s16(sum, s1, y_filter_lo, 1);
    sum           = vmlal_lane_s16(sum, s2, y_filter_lo, 2);
    sum           = vmlal_lane_s16(sum, s3, y_filter_lo, 3);
    sum           = vmlal_lane_s16(sum, s4, y_filter_hi, 0);
    sum           = vmlal_lane_s16(sum, s5, y_filter_hi, 1);
    sum           = vmlal_lane_s16(sum, s6, y_filter_hi, 2);
    sum           = vmlal_lane_s16(sum, s7, y_filter_hi, 3);

    return vqrshrn_n_s32(sum, 2 * FILTER_BITS - ROUND0_BITS);
}

static INLINE uint8x8_t convolve8_8_2d_v(const int16x8_t s0, const int16x8_t s1, const int16x8_t s2, const int16x8_t s3,
                                         const int16x8_t s4, const int16x8_t s5, const int16x8_t s6, const int16x8_t s7,
                                         const int16x8_t y_filter, const int16x8_t sub_const) {
    const int16x4_t y_filter_lo = vget_low_s16(y_filter);
    const int16x4_t y_filter_hi = vget_high_s16(y_filter);

    int32x4_t sum0 = vmull_lane_s16(vget_low_s16(s0), y_filter_lo, 0);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s1), y_filter_lo, 1);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s2), y_filter_lo, 2);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s3), y_filter_lo, 3);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s4), y_filter_hi, 0);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s5), y_filter_hi, 1);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s6), y_filter_hi, 2);
    sum0           = vmlal_lane_s16(sum0, vget_low_s16(s7), y_filter_hi, 3);

    int32x4_t sum1 = vmull_lane_s16(vget_high_s16(s0), y_filter_lo, 0);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s1), y_filter_lo, 1);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s2), y_filter_lo, 2);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s3), y_filter_lo, 3);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s4), y_filter_hi, 0);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s5), y_filter_hi, 1);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s6), y_filter_hi, 2);
    sum1           = vmlal_lane_s16(sum1, vget_high_s16(s7), y_filter_hi, 3);

    int16x8_t res = vcombine_s16(vqrshrn_n_s32(sum0, 2 * FILTER_BITS - ROUND0_BITS),
                                 vqrshrn_n_s32(sum1, 2 * FILTER_BITS - ROUND0_BITS));
    res           = vsubq_s16(res, sub_const);

    return vqmovun_s16(res);
}

static INLINE void convolve_2d_sr_vert_8tap_neon(int16_t *src_ptr, int src_stride, uint8_t *dst_ptr, int dst_stride,
                                                 int w, int h, const int16x8_t y_filter) {
    const int       bd        = 8;
    const int16x8_t sub_const = vdupq_n_s16(1 << (bd - 1));

    if (w <= 4) {
        int16x4_t s0, s1, s2, s3, s4, s5, s6;
        load_s16_4x7(src_ptr, src_stride, &s0, &s1, &s2, &s3, &s4, &s5, &s6);
        src_ptr += 7 * src_stride;

        do {
            int16x4_t s7, s8, s9, s10;
            load_s16_4x4(src_ptr, src_stride, &s7, &s8, &s9, &s10);

            int16x4_t d0 = convolve8_4_2d_v(s0, s1, s2, s3, s4, s5, s6, s7, y_filter);
            int16x4_t d1 = convolve8_4_2d_v(s1, s2, s3, s4, s5, s6, s7, s8, y_filter);
            int16x4_t d2 = convolve8_4_2d_v(s2, s3, s4, s5, s6, s7, s8, s9, y_filter);
            int16x4_t d3 = convolve8_4_2d_v(s3, s4, s5, s6, s7, s8, s9, s10, y_filter);

            uint8x8_t d01 = vqmovun_s16(vsubq_s16(vcombine_s16(d0, d1), sub_const));
            uint8x8_t d23 = vqmovun_s16(vsubq_s16(vcombine_s16(d2, d3), sub_const));

            store_u8_4x1(dst_ptr + 0 * dst_stride, d01, 0);
            store_u8_4x1(dst_ptr + 1 * dst_stride, d01, 1);
            store_u8_4x1(dst_ptr + 2 * dst_stride, d23, 0);
            store_u8_4x1(dst_ptr + 3 * dst_stride, d23, 1);

            s0 = s4;
            s1 = s5;
            s2 = s6;
            s3 = s7;
            s4 = s8;
            s5 = s9;
            s6 = s10;
            src_ptr += 4 * src_stride;
            dst_ptr += 4 * dst_stride;
            h -= 4;
        } while (h != 0);
    } else {
        // Width is a multiple of 8 and height is a multiple of 4.
        do {
            int      height = h;
            int16_t *s      = src_ptr;
            uint8_t *d      = dst_ptr;

            int16x8_t s0, s1, s2, s3, s4, s5, s6;
            load_s16_8x7(s, src_stride, &s0, &s1, &s2, &s3, &s4, &s5, &s6);
            s += 7 * src_stride;

            do {
                int16x8_t s7, s8, s9, s10;
                load_s16_8x4(s, src_stride, &s7, &s8, &s9, &s10);

                uint8x8_t d0 = convolve8_8_2d_v(s0, s1, s2, s3, s4, s5, s6, s7, y_filter, sub_const);
                uint8x8_t d1 = convolve8_8_2d_v(s1, s2, s3, s4, s5, s6, s7, s8, y_filter, sub_const);
                uint8x8_t d2 = convolve8_8_2d_v(s2, s3, s4, s5, s6, s7, s8, s9, y_filter, sub_const);
                uint8x8_t d3 = convolve8_8_2d_v(s3, s4, s5, s6, s7, s8, s9, s10, y_filter, sub_const);

                store_u8_8x4(d, dst_stride, d0, d1, d2, d3);

                s0 = s4;
                s1 = s5;
                s2 = s6;
                s3 = s7;
                s4 = s8;
                s5 = s9;
                s6 = s10;
                s += 4 * src_stride;
                d += 4 * dst_stride;
                height -= 4;
            } while (height != 0);
            src_ptr += 8;
            dst_ptr += 8;
            w -= 8;
        } while (w != 0);
    }
}

#endif // AOM_AV1_COMMON_ARM_CONVOLVE_NEON_H_
