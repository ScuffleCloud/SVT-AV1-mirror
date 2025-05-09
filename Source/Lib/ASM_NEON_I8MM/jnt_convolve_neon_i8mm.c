/*
 * Copyright (c) 2023, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <arm_neon.h>

#include "common_dsp_rtcd.h"
#include "compound_convolve_neon.h"
#include "convolve.h"
#include "definitions.h"
#include "mem_neon.h"

DECLARE_ALIGNED(16, static const uint8_t, kDotProdPermuteTbl[48]) = {
    // clang-format off
    0, 1,  2,  3, 1,  2,  3,  4,  2,  3,  4,  5,  3,  4,  5,  6,
    4, 5,  6,  7, 5,  6,  7,  8,  6,  7,  8,  9,  7,  8,  9, 10,
    8, 9, 10, 11, 9, 10, 11, 12, 10, 11, 12, 13, 11, 12, 13, 14
    // clang-format on
};

DECLARE_ALIGNED(16, static const uint8_t, kMatMulPermuteTbl[32]) = {
    // clang-format off
    0,  1,  2,  3,  4,  5,  6,  7,  2,  3,  4,  5,  6,  7,  8,  9,
    4,  5,  6,  7,  8,  9, 10, 11,  6,  7,  8,  9, 10, 11, 12, 13
    // clang-format on
};

static inline uint16x4_t convolve6_4_x(uint8x16_t samples, const int8x16_t x_filter, const uint8x16_t permute_tbl,
                                       const int32x4_t round_offset) {
    // Permute samples ready for matrix multiply.
    // { 0,  1,  2,  3,  4,  5,  6,  7,  2,  3,  4,  5,  6,  7,  8,  9 }
    uint8x16_t permuted_samples = vqtbl1q_u8(samples, permute_tbl);

    // These instructions multiply a 2x8 matrix (samples) by an 8x2 matrix
    // (filter), destructively accumulating into the destination register.
    int32x4_t sum = vusmmlaq_s32(round_offset, permuted_samples, x_filter);

    // We halved the convolution filter values so -1 from the right shift.
    return vreinterpret_u16_s16(vshrn_n_s32(sum, ROUND0_BITS - 1));
}

static inline uint16x8_t convolve6_8_x(uint8x16_t samples, const int8x16_t x_filter, const uint8x16x2_t permute_tbl,
                                       const int32x4_t round_offset) {
    // Permute samples ready for matrix multiply.
    // { 0,  1,  2,  3,  4,  5,  6,  7,  2,  3,  4,  5,  6,  7,  8,  9 }
    // { 4,  5,  6,  7,  8,  9, 10, 11,  6,  7,  8,  9, 10, 11, 12, 13 }
    uint8x16_t permuted_samples[2] = {vqtbl1q_u8(samples, permute_tbl.val[0]), vqtbl1q_u8(samples, permute_tbl.val[1])};

    // These instructions multiply a 2x8 matrix (samples) by an 8x2 matrix
    // (filter), destructively accumulating into the destination register.
    int32x4_t sum0123 = vusmmlaq_s32(round_offset, permuted_samples[0], x_filter);
    int32x4_t sum4567 = vusmmlaq_s32(round_offset, permuted_samples[1], x_filter);

    // Narrow and re-pack.
    // We halved the convolution filter values so -1 from the right shift.
    int16x8_t res = vcombine_s16(vshrn_n_s32(sum0123, ROUND0_BITS - 1), vshrn_n_s32(sum4567, ROUND0_BITS - 1));
    return vreinterpretq_u16_s16(res);
}

static inline uint16x8_t convolve8_8_x(uint8x16_t samples, const int8x8_t x_filter, const uint8x16x3_t permute_tbl,
                                       const int32x4_t round_offset) {
    uint8x16_t permuted_samples[3];
    int32x4_t  sum[2];

    // Permute samples ready for dot product.
    // { 0,  1,  2,  3,  1,  2,  3,  4,  2,  3,  4,  5,  3,  4,  5,  6 }
    permuted_samples[0] = vqtbl1q_u8(samples, permute_tbl.val[0]);
    // { 4,  5,  6,  7,  5,  6,  7,  8,  6,  7,  8,  9,  7,  8,  9, 10 }
    permuted_samples[1] = vqtbl1q_u8(samples, permute_tbl.val[1]);
    // { 8,  9, 10, 11,  9, 10, 11, 12, 10, 11, 12, 13, 11, 12, 13, 14 }
    permuted_samples[2] = vqtbl1q_u8(samples, permute_tbl.val[2]);

    // First 4 output values.
    sum[0] = vusdotq_lane_s32(round_offset, permuted_samples[0], x_filter, 0);
    sum[0] = vusdotq_lane_s32(sum[0], permuted_samples[1], x_filter, 1);
    // Second 4 output values.
    sum[1] = vusdotq_lane_s32(round_offset, permuted_samples[1], x_filter, 0);
    sum[1] = vusdotq_lane_s32(sum[1], permuted_samples[2], x_filter, 1);

    // Narrow and re-pack.
    // We halved the convolution filter values so -1 from the right shift.
    int16x8_t res = vcombine_s16(vshrn_n_s32(sum[0], ROUND0_BITS - 1), vshrn_n_s32(sum[1], ROUND0_BITS - 1));
    return vreinterpretq_u16_s16(res);
}

static inline void dist_wtd_convolve_x_dist_wtd_avg_6tap_neon_i8mm(const uint8_t *src, int src_stride, uint16_t *dst,
                                                                   int dst_stride, uint8_t *dst8, int dst8_stride,
                                                                   int w, int h, const int16_t *x_filter_ptr,
                                                                   const uint16_t fwd_offset,
                                                                   const uint16_t bck_offset) {
    assert(w % 4 == 0);
    assert(h % 4 == 0);

    const int     bd           = 8;
    const int     offset_bits  = bd + 2 * FILTER_BITS - ROUND0_BITS;
    const int16_t round_offset = (1 << (offset_bits - COMPOUND_ROUND1_BITS)) +
        (1 << (offset_bits - COMPOUND_ROUND1_BITS - 1));
    const int16x8_t round_offset_vec = vdupq_n_s16(round_offset);
    // A shim of 1 << ((ROUND0_BITS - 1) - 1) enables us to use non-rounding
    // shifts - which are generally faster than rounding shifts on modern CPUs.
    // (The extra -1 is needed because we halved the filter values.)
    const int32x4_t round_offset_shim = vdupq_n_s32((round_offset << (ROUND0_BITS - 1)) +
                                                    (1 << ((ROUND0_BITS - 1) - 1)));

    // Filter values are even, so halve to reduce intermediate precision reqs.
    const int8x8_t x_filter_s8 = vshrn_n_s16(vld1q_s16(x_filter_ptr), 1);
    // Stagger the filter for use with the matrix multiply instructions.
    // { f0, f1, f2, f3, f4, f5,  0,  0,  0, f0, f1, f2, f3, f4, f5,  0 }
    const int8x16_t x_filter = vcombine_s8(vext_s8(x_filter_s8, x_filter_s8, 1), x_filter_s8);

    if (w == 4) {
        const uint8x16_t permute_tbl = vld1q_u8(kMatMulPermuteTbl);
        do {
            uint8x16_t s0, s1, s2, s3;
            load_u8_16x4(src, src_stride, &s0, &s1, &s2, &s3);

            uint16x4_t d0 = convolve6_4_x(s0, x_filter, permute_tbl, round_offset_shim);
            uint16x4_t d1 = convolve6_4_x(s1, x_filter, permute_tbl, round_offset_shim);
            uint16x4_t d2 = convolve6_4_x(s2, x_filter, permute_tbl, round_offset_shim);
            uint16x4_t d3 = convolve6_4_x(s3, x_filter, permute_tbl, round_offset_shim);

            uint16x4_t dd0, dd1, dd2, dd3;
            load_u16_4x4(dst, dst_stride, &dd0, &dd1, &dd2, &dd3);

            uint8x8_t d01_u8, d23_u8;
            compute_dist_wtd_avg_4x4(
                dd0, dd1, dd2, dd3, d0, d1, d2, d3, fwd_offset, bck_offset, round_offset_vec, &d01_u8, &d23_u8);

            store_u8x4_strided_x2(dst8 + 0 * dst8_stride, dst8_stride, d01_u8);
            store_u8x4_strided_x2(dst8 + 2 * dst8_stride, dst8_stride, d23_u8);

            src += 4 * src_stride;
            dst += 4 * dst_stride;
            dst8 += 4 * dst8_stride;
            h -= 4;
        } while (h != 0);
    } else {
        const uint8x16x2_t permute_tbl = vld1q_u8_x2(kMatMulPermuteTbl);
        do {
            const uint8_t *s     = src;
            uint16_t      *d     = dst;
            uint8_t       *d_u8  = dst8;
            int            width = w;

            do {
                uint8x16_t s0, s1, s2, s3;
                load_u8_16x4(s, src_stride, &s0, &s1, &s2, &s3);

                uint16x8_t d0 = convolve6_8_x(s0, x_filter, permute_tbl, round_offset_shim);
                uint16x8_t d1 = convolve6_8_x(s1, x_filter, permute_tbl, round_offset_shim);
                uint16x8_t d2 = convolve6_8_x(s2, x_filter, permute_tbl, round_offset_shim);
                uint16x8_t d3 = convolve6_8_x(s3, x_filter, permute_tbl, round_offset_shim);

                uint16x8_t dd0, dd1, dd2, dd3;
                load_u16_8x4(d, dst_stride, &dd0, &dd1, &dd2, &dd3);

                uint8x8_t d0_u8, d1_u8, d2_u8, d3_u8;
                compute_dist_wtd_avg_8x4(dd0,
                                         dd1,
                                         dd2,
                                         dd3,
                                         d0,
                                         d1,
                                         d2,
                                         d3,
                                         fwd_offset,
                                         bck_offset,
                                         round_offset_vec,
                                         &d0_u8,
                                         &d1_u8,
                                         &d2_u8,
                                         &d3_u8);

                store_u8_8x4(d_u8, dst8_stride, d0_u8, d1_u8, d2_u8, d3_u8);

                s += 8;
                d += 8;
                d_u8 += 8;
                width -= 8;
            } while (width != 0);
            src += 4 * src_stride;
            dst += 4 * dst_stride;
            dst8 += 4 * dst8_stride;
            h -= 4;
        } while (h != 0);
    }
}

static inline void dist_wtd_convolve_x_dist_wtd_avg_8tap_neon_i8mm(const uint8_t *src, int src_stride, uint16_t *dst,
                                                                   int dst_stride, uint8_t *dst8, int dst8_stride,
                                                                   int w, int h, const int16_t *x_filter_ptr,
                                                                   const uint16_t fwd_offset,
                                                                   const uint16_t bck_offset) {
    assert(w % 4 == 0);
    assert(h % 4 == 0);

    const int     bd           = 8;
    const int     offset_bits  = bd + 2 * FILTER_BITS - ROUND0_BITS;
    const int16_t round_offset = (1 << (offset_bits - COMPOUND_ROUND1_BITS)) +
        (1 << (offset_bits - COMPOUND_ROUND1_BITS - 1));
    const int16x8_t round_offset_vec = vdupq_n_s16(round_offset);
    // A shim of 1 << ((ROUND0_BITS - 1) - 1) enables us to use non-rounding
    // shifts - which are generally faster than rounding shifts on modern CPUs.
    // (The extra -1 is needed because we halved the filter values.)
    const int32x4_t round_offset_shim = vdupq_n_s32((round_offset << (ROUND0_BITS - 1)) +
                                                    (1 << ((ROUND0_BITS - 1) - 1)));

    const uint8x16x3_t permute_tbl = vld1q_u8_x3(kDotProdPermuteTbl);
    // Filter values are even, so halve to reduce intermediate precision reqs.
    const int8x8_t x_filter = vshrn_n_s16(vld1q_s16(x_filter_ptr), 1);

    do {
        const uint8_t *s     = src;
        uint16_t      *d     = dst;
        uint8_t       *d_u8  = dst8;
        int            width = w;

        do {
            uint8x16_t s0, s1, s2, s3;
            load_u8_16x4(s, src_stride, &s0, &s1, &s2, &s3);

            uint16x8_t d0 = convolve8_8_x(s0, x_filter, permute_tbl, round_offset_shim);
            uint16x8_t d1 = convolve8_8_x(s1, x_filter, permute_tbl, round_offset_shim);
            uint16x8_t d2 = convolve8_8_x(s2, x_filter, permute_tbl, round_offset_shim);
            uint16x8_t d3 = convolve8_8_x(s3, x_filter, permute_tbl, round_offset_shim);

            uint16x8_t dd0, dd1, dd2, dd3;
            load_u16_8x4(d, dst_stride, &dd0, &dd1, &dd2, &dd3);

            uint8x8_t d0_u8, d1_u8, d2_u8, d3_u8;
            compute_dist_wtd_avg_8x4(dd0,
                                     dd1,
                                     dd2,
                                     dd3,
                                     d0,
                                     d1,
                                     d2,
                                     d3,
                                     fwd_offset,
                                     bck_offset,
                                     round_offset_vec,
                                     &d0_u8,
                                     &d1_u8,
                                     &d2_u8,
                                     &d3_u8);

            store_u8_8x4(d_u8, dst8_stride, d0_u8, d1_u8, d2_u8, d3_u8);

            s += 8;
            d += 8;
            d_u8 += 8;
            width -= 8;
        } while (width != 0);
        src += 4 * src_stride;
        dst += 4 * dst_stride;
        dst8 += 4 * dst8_stride;
        h -= 4;
    } while (h != 0);
}

static inline void dist_wtd_convolve_x_avg_6tap_neon_i8mm(const uint8_t *src, int src_stride, uint16_t *dst,
                                                          int dst_stride, uint8_t *dst8, int dst8_stride, int w, int h,
                                                          const int16_t *x_filter_ptr) {
    assert(w % 4 == 0);
    assert(h % 4 == 0);

    const int     bd           = 8;
    const int     offset_bits  = bd + 2 * FILTER_BITS - ROUND0_BITS;
    const int16_t round_offset = (1 << (offset_bits - COMPOUND_ROUND1_BITS)) +
        (1 << (offset_bits - COMPOUND_ROUND1_BITS - 1));
    const int16x8_t round_offset_vec = vdupq_n_s16(round_offset);
    // A shim of 1 << ((ROUND0_BITS - 1) - 1) enables us to use non-rounding
    // shifts - which are generally faster than rounding shifts on modern CPUs.
    // (The extra -1 is needed because we halved the filter values.)
    const int32x4_t round_offset_shim = vdupq_n_s32((round_offset << (ROUND0_BITS - 1)) +
                                                    (1 << ((ROUND0_BITS - 1) - 1)));

    // Filter values are even, so halve to reduce intermediate precision reqs.
    const int8x8_t x_filter_s8 = vshrn_n_s16(vld1q_s16(x_filter_ptr), 1);
    // Stagger the filter for use with the matrix multiply instructions.
    // { f0, f1, f2, f3, f4, f5,  0,  0,  0, f0, f1, f2, f3, f4, f5,  0 }
    const int8x16_t x_filter = vcombine_s8(vext_s8(x_filter_s8, x_filter_s8, 1), x_filter_s8);

    if (w == 4) {
        const uint8x16_t permute_tbl = vld1q_u8(kMatMulPermuteTbl);
        do {
            uint8x16_t s0, s1, s2, s3;
            load_u8_16x4(src, src_stride, &s0, &s1, &s2, &s3);

            uint16x4_t d0 = convolve6_4_x(s0, x_filter, permute_tbl, round_offset_shim);
            uint16x4_t d1 = convolve6_4_x(s1, x_filter, permute_tbl, round_offset_shim);
            uint16x4_t d2 = convolve6_4_x(s2, x_filter, permute_tbl, round_offset_shim);
            uint16x4_t d3 = convolve6_4_x(s3, x_filter, permute_tbl, round_offset_shim);

            uint16x4_t dd0, dd1, dd2, dd3;
            load_u16_4x4(dst, dst_stride, &dd0, &dd1, &dd2, &dd3);

            uint8x8_t d01_u8, d23_u8;
            compute_basic_avg_4x4(dd0, dd1, dd2, dd3, d0, d1, d2, d3, round_offset_vec, &d01_u8, &d23_u8);

            store_u8x4_strided_x2(dst8 + 0 * dst8_stride, dst8_stride, d01_u8);
            store_u8x4_strided_x2(dst8 + 2 * dst8_stride, dst8_stride, d23_u8);

            src += 4 * src_stride;
            dst += 4 * dst_stride;
            dst8 += 4 * dst8_stride;
            h -= 4;
        } while (h != 0);
    } else {
        const uint8x16x2_t permute_tbl = vld1q_u8_x2(kMatMulPermuteTbl);
        do {
            const uint8_t *s     = src;
            uint16_t      *d     = dst;
            uint8_t       *d_u8  = dst8;
            int            width = w;

            do {
                uint8x16_t s0, s1, s2, s3;
                load_u8_16x4(s, src_stride, &s0, &s1, &s2, &s3);

                uint16x8_t d0 = convolve6_8_x(s0, x_filter, permute_tbl, round_offset_shim);
                uint16x8_t d1 = convolve6_8_x(s1, x_filter, permute_tbl, round_offset_shim);
                uint16x8_t d2 = convolve6_8_x(s2, x_filter, permute_tbl, round_offset_shim);
                uint16x8_t d3 = convolve6_8_x(s3, x_filter, permute_tbl, round_offset_shim);

                uint16x8_t dd0, dd1, dd2, dd3;
                load_u16_8x4(d, dst_stride, &dd0, &dd1, &dd2, &dd3);

                uint8x8_t d0_u8, d1_u8, d2_u8, d3_u8;
                compute_basic_avg_8x4(
                    dd0, dd1, dd2, dd3, d0, d1, d2, d3, round_offset_vec, &d0_u8, &d1_u8, &d2_u8, &d3_u8);

                store_u8_8x4(d_u8, dst8_stride, d0_u8, d1_u8, d2_u8, d3_u8);

                s += 8;
                d += 8;
                d_u8 += 8;
                width -= 8;
            } while (width != 0);
            src += 4 * src_stride;
            dst += 4 * dst_stride;
            dst8 += 4 * dst8_stride;
            h -= 4;
        } while (h != 0);
    }
}

static inline void dist_wtd_convolve_x_avg_8tap_neon_i8mm(const uint8_t *src, int src_stride, uint16_t *dst,
                                                          int dst_stride, uint8_t *dst8, int dst8_stride, int w, int h,
                                                          const int16_t *x_filter_ptr) {
    assert(w % 4 == 0);
    assert(h % 4 == 0);

    const int     bd           = 8;
    const int     offset_bits  = bd + 2 * FILTER_BITS - ROUND0_BITS;
    const int16_t round_offset = (1 << (offset_bits - COMPOUND_ROUND1_BITS)) +
        (1 << (offset_bits - COMPOUND_ROUND1_BITS - 1));
    const int16x8_t round_offset_vec = vdupq_n_s16(round_offset);
    // A shim of 1 << ((ROUND0_BITS - 1) - 1) enables us to use non-rounding
    // shifts - which are generally faster than rounding shifts on modern CPUs.
    // (The extra -1 is needed because we halved the filter values.)
    const int32x4_t round_offset_shim = vdupq_n_s32((round_offset << (ROUND0_BITS - 1)) +
                                                    (1 << ((ROUND0_BITS - 1) - 1)));

    const uint8x16x3_t permute_tbl = vld1q_u8_x3(kDotProdPermuteTbl);
    // Filter values are even, so halve to reduce intermediate precision reqs.
    const int8x8_t x_filter = vshrn_n_s16(vld1q_s16(x_filter_ptr), 1);

    do {
        const uint8_t *s     = src;
        uint16_t      *d     = dst;
        uint8_t       *d_u8  = dst8;
        int            width = w;

        do {
            uint8x16_t s0, s1, s2, s3;
            load_u8_16x4(s, src_stride, &s0, &s1, &s2, &s3);

            uint16x8_t d0 = convolve8_8_x(s0, x_filter, permute_tbl, round_offset_shim);
            uint16x8_t d1 = convolve8_8_x(s1, x_filter, permute_tbl, round_offset_shim);
            uint16x8_t d2 = convolve8_8_x(s2, x_filter, permute_tbl, round_offset_shim);
            uint16x8_t d3 = convolve8_8_x(s3, x_filter, permute_tbl, round_offset_shim);

            uint16x8_t dd0, dd1, dd2, dd3;
            load_u16_8x4(d, dst_stride, &dd0, &dd1, &dd2, &dd3);

            uint8x8_t d0_u8, d1_u8, d2_u8, d3_u8;
            compute_basic_avg_8x4(dd0, dd1, dd2, dd3, d0, d1, d2, d3, round_offset_vec, &d0_u8, &d1_u8, &d2_u8, &d3_u8);

            store_u8_8x4(d_u8, dst8_stride, d0_u8, d1_u8, d2_u8, d3_u8);

            s += 8;
            d += 8;
            d_u8 += 8;
            width -= 8;
        } while (width != 0);
        src += 4 * src_stride;
        dst += 4 * dst_stride;
        dst8 += 4 * dst8_stride;
        h -= 4;
    } while (h != 0);
}

static inline void dist_wtd_convolve_x_6tap_neon_i8mm(const uint8_t *src, int src_stride, uint16_t *dst, int dst_stride,
                                                      int w, int h, const int16_t *x_filter_ptr) {
    assert(w % 4 == 0);
    assert(h % 4 == 0);

    const int     bd           = 8;
    const int     offset_bits  = bd + 2 * FILTER_BITS - ROUND0_BITS;
    const int16_t round_offset = (1 << (offset_bits - COMPOUND_ROUND1_BITS)) +
        (1 << (offset_bits - COMPOUND_ROUND1_BITS - 1));
    // A shim of 1 << ((ROUND0_BITS - 1) - 1) enables us to use non-rounding
    // shifts - which are generally faster than rounding shifts on modern CPUs.
    // (The extra -1 is needed because we halved the filter values.)
    const int32x4_t round_offset_shim = vdupq_n_s32((round_offset << (ROUND0_BITS - 1)) +
                                                    (1 << ((ROUND0_BITS - 1) - 1)));

    // Filter values are even, so halve to reduce intermediate precision reqs.
    const int8x8_t x_filter_s8 = vshrn_n_s16(vld1q_s16(x_filter_ptr), 1);
    // Stagger the filter for use with the matrix multiply instructions.
    // { f0, f1, f2, f3, f4, f5,  0,  0,  0, f0, f1, f2, f3, f4, f5,  0 }
    const int8x16_t x_filter = vcombine_s8(vext_s8(x_filter_s8, x_filter_s8, 1), x_filter_s8);

    if (w == 4) {
        const uint8x16_t permute_tbl = vld1q_u8(kMatMulPermuteTbl);
        do {
            uint8x16_t s0, s1, s2, s3;
            load_u8_16x4(src, src_stride, &s0, &s1, &s2, &s3);

            uint16x4_t d0 = convolve6_4_x(s0, x_filter, permute_tbl, round_offset_shim);
            uint16x4_t d1 = convolve6_4_x(s1, x_filter, permute_tbl, round_offset_shim);
            uint16x4_t d2 = convolve6_4_x(s2, x_filter, permute_tbl, round_offset_shim);
            uint16x4_t d3 = convolve6_4_x(s3, x_filter, permute_tbl, round_offset_shim);

            store_u16_4x4(dst, dst_stride, d0, d1, d2, d3);

            src += 4 * src_stride;
            dst += 4 * dst_stride;
            h -= 4;
        } while (h != 0);
    } else {
        const uint8x16x2_t permute_tbl = vld1q_u8_x2(kMatMulPermuteTbl);
        do {
            const uint8_t *s     = src;
            uint16_t      *d     = dst;
            int            width = w;

            do {
                uint8x16_t s0, s1, s2, s3;
                load_u8_16x4(s, src_stride, &s0, &s1, &s2, &s3);

                uint16x8_t d0 = convolve6_8_x(s0, x_filter, permute_tbl, round_offset_shim);
                uint16x8_t d1 = convolve6_8_x(s1, x_filter, permute_tbl, round_offset_shim);
                uint16x8_t d2 = convolve6_8_x(s2, x_filter, permute_tbl, round_offset_shim);
                uint16x8_t d3 = convolve6_8_x(s3, x_filter, permute_tbl, round_offset_shim);

                store_u16_8x4(d, dst_stride, d0, d1, d2, d3);

                s += 8;
                d += 8;
                width -= 8;
            } while (width != 0);
            src += 4 * src_stride;
            dst += 4 * dst_stride;
            h -= 4;
        } while (h != 0);
    }
}

static inline void dist_wtd_convolve_x_8tap_neon_i8mm(const uint8_t *src, int src_stride, uint16_t *dst, int dst_stride,
                                                      int w, int h, const int16_t *x_filter_ptr) {
    assert(w % 4 == 0);
    assert(h % 4 == 0);

    const int     bd           = 8;
    const int     offset_bits  = bd + 2 * FILTER_BITS - ROUND0_BITS;
    const int16_t round_offset = (1 << (offset_bits - COMPOUND_ROUND1_BITS)) +
        (1 << (offset_bits - COMPOUND_ROUND1_BITS - 1));
    // A shim of 1 << ((ROUND0_BITS - 1) - 1) enables us to use non-rounding
    // shifts - which are generally faster than rounding shifts on modern CPUs.
    // (The extra -1 is needed because we halved the filter values.)
    const int32x4_t round_offset_shim = vdupq_n_s32((round_offset << (ROUND0_BITS - 1)) +
                                                    (1 << ((ROUND0_BITS - 1) - 1)));

    const uint8x16x3_t permute_tbl = vld1q_u8_x3(kDotProdPermuteTbl);
    // Filter values are even, so halve to reduce intermediate precision reqs.
    const int8x8_t x_filter = vshrn_n_s16(vld1q_s16(x_filter_ptr), 1);

    do {
        const uint8_t *s     = src;
        uint16_t      *d     = dst;
        int            width = w;

        do {
            uint8x16_t s0, s1, s2, s3;
            load_u8_16x4(s, src_stride, &s0, &s1, &s2, &s3);

            uint16x8_t d0 = convolve8_8_x(s0, x_filter, permute_tbl, round_offset_shim);
            uint16x8_t d1 = convolve8_8_x(s1, x_filter, permute_tbl, round_offset_shim);
            uint16x8_t d2 = convolve8_8_x(s2, x_filter, permute_tbl, round_offset_shim);
            uint16x8_t d3 = convolve8_8_x(s3, x_filter, permute_tbl, round_offset_shim);

            store_u16_8x4(d, dst_stride, d0, d1, d2, d3);

            s += 8;
            d += 8;
            width -= 8;
        } while (width != 0);
        src += 4 * src_stride;
        dst += 4 * dst_stride;
        h -= 4;
    } while (h != 0);
}

void svt_av1_jnt_convolve_x_neon_i8mm(const uint8_t *src, int src_stride, uint8_t *dst8, int dst8_stride, int w, int h,
                                      InterpFilterParams *filter_params_x, InterpFilterParams *filter_params_y,
                                      const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params) {
    assert(w % 4 == 0);
    assert(h % 4 == 0);

    if (w == 2 || h == 2) {
        svt_av1_jnt_convolve_x_c(src,
                                 src_stride,
                                 dst8,
                                 dst8_stride,
                                 w,
                                 h,
                                 filter_params_x,
                                 filter_params_y,
                                 subpel_x_qn,
                                 subpel_y_qn,
                                 conv_params);
        return;
    }

    const int16_t *x_filter_ptr = av1_get_interp_filter_subpel_kernel(*filter_params_x, subpel_x_qn & SUBPEL_MASK);
    const int      filter_taps  = get_filter_tap(filter_params_x, subpel_x_qn & SUBPEL_MASK);

    src -= (SUBPEL_TAPS / 2 - 1);

    if (conv_params->do_average) {
        if (UNLIKELY(conv_params->use_jnt_comp_avg)) {
            if (filter_taps < 8) {
                dist_wtd_convolve_x_dist_wtd_avg_6tap_neon_i8mm(src + 1,
                                                                src_stride,
                                                                conv_params->dst,
                                                                conv_params->dst_stride,
                                                                dst8,
                                                                dst8_stride,
                                                                w,
                                                                h,
                                                                x_filter_ptr,
                                                                conv_params->fwd_offset,
                                                                conv_params->bck_offset);
                return;
            }

            dist_wtd_convolve_x_dist_wtd_avg_8tap_neon_i8mm(src,
                                                            src_stride,
                                                            conv_params->dst,
                                                            conv_params->dst_stride,
                                                            dst8,
                                                            dst8_stride,
                                                            w,
                                                            h,
                                                            x_filter_ptr,
                                                            conv_params->fwd_offset,
                                                            conv_params->bck_offset);
        } else {
            if (filter_taps < 8) {
                dist_wtd_convolve_x_avg_6tap_neon_i8mm(src + 1,
                                                       src_stride,
                                                       conv_params->dst,
                                                       conv_params->dst_stride,
                                                       dst8,
                                                       dst8_stride,
                                                       w,
                                                       h,
                                                       x_filter_ptr);
                return;
            }

            dist_wtd_convolve_x_avg_8tap_neon_i8mm(
                src, src_stride, conv_params->dst, conv_params->dst_stride, dst8, dst8_stride, w, h, x_filter_ptr);
        }
    } else {
        if (filter_taps < 8) {
            dist_wtd_convolve_x_6tap_neon_i8mm(
                src + 1, src_stride, conv_params->dst, conv_params->dst_stride, w, h, x_filter_ptr);
            return;
        }

        dist_wtd_convolve_x_8tap_neon_i8mm(
            src, src_stride, conv_params->dst, conv_params->dst_stride, w, h, x_filter_ptr);
    }
}

static inline int16x4_t convolve6_4_2d_h(uint8x16_t samples, const int8x16_t x_filter, const uint8x16_t permute_tbl,
                                         const int32x4_t horiz_const) {
    // Permute samples ready for matrix multiply.
    // { 0,  1,  2,  3,  4,  5,  6,  7,  2,  3,  4,  5,  6,  7,  8,  9 }
    uint8x16_t permuted_samples = vqtbl1q_u8(samples, permute_tbl);

    // These instructions multiply a 2x8 matrix (samples) by an 8x2 matrix
    // (filter), destructively accumulating into the destination register.
    int32x4_t sum = vusmmlaq_s32(horiz_const, permuted_samples, x_filter);

    // We halved the convolution filter values so -1 from the right shift.
    return vshrn_n_s32(sum, ROUND0_BITS - 1);
}

static inline int16x8_t convolve6_8_2d_h(uint8x16_t samples, const int8x16_t x_filter, const uint8x16x2_t permute_tbl,
                                         const int32x4_t horiz_const) {
    // Permute samples ready for matrix multiply.
    // { 0,  1,  2,  3,  4,  5,  6,  7,  2,  3,  4,  5,  6,  7,  8,  9 }
    // { 4,  5,  6,  7,  8,  9, 10, 11,  6,  7,  8,  9, 10, 11, 12, 13 }
    uint8x16_t permuted_samples[2] = {vqtbl1q_u8(samples, permute_tbl.val[0]), vqtbl1q_u8(samples, permute_tbl.val[1])};

    // These instructions multiply a 2x8 matrix (samples) by an 8x2 matrix
    // (filter), destructively accumulating into the destination register.
    int32x4_t sum0123 = vusmmlaq_s32(horiz_const, permuted_samples[0], x_filter);
    int32x4_t sum4567 = vusmmlaq_s32(horiz_const, permuted_samples[1], x_filter);

    // Narrow and re-pack.
    // We halved the convolution filter values so -1 from the right shift.
    return vcombine_s16(vshrn_n_s32(sum0123, ROUND0_BITS - 1), vshrn_n_s32(sum4567, ROUND0_BITS - 1));
}

static inline void dist_wtd_convolve_2d_horiz_6tap_neon_i8mm(const uint8_t *src, int src_stride, int16_t *im_block,
                                                             const int im_stride, const int16_t *x_filter_ptr,
                                                             const int im_h, int w) {
    const int bd = 8;
    // A shim of 1 << ((ROUND0_BITS - 1) - 1) enables us to use non-rounding
    // shifts - which are generally faster than rounding shifts on modern CPUs.
    // (The extra -1 is needed because we halved the filter values.)
    const int32x4_t horiz_const = vdupq_n_s32((1 << (bd + FILTER_BITS - 2)) + (1 << ((ROUND0_BITS - 1) - 1)));

    // Filter values are even, so halve to reduce intermediate precision reqs.
    const int8x8_t x_filter_s8 = vshrn_n_s16(vld1q_s16(x_filter_ptr), 1);
    // Stagger the filter for use with the matrix multiply instructions.
    // { f0, f1, f2, f3, f4, f5,  0,  0,  0, f0, f1, f2, f3, f4, f5,  0 }
    const int8x16_t x_filter = vcombine_s8(vext_s8(x_filter_s8, x_filter_s8, 1), x_filter_s8);

    const uint8_t *src_ptr    = src;
    int16_t       *dst_ptr    = im_block;
    int            dst_stride = im_stride;
    int            height     = im_h;

    if (w == 4) {
        const uint8x16_t permute_tbl = vld1q_u8(kMatMulPermuteTbl);
        do {
            uint8x16_t s0, s1, s2, s3;
            load_u8_16x4(src_ptr, src_stride, &s0, &s1, &s2, &s3);

            int16x4_t d0 = convolve6_4_2d_h(s0, x_filter, permute_tbl, horiz_const);
            int16x4_t d1 = convolve6_4_2d_h(s1, x_filter, permute_tbl, horiz_const);
            int16x4_t d2 = convolve6_4_2d_h(s2, x_filter, permute_tbl, horiz_const);
            int16x4_t d3 = convolve6_4_2d_h(s3, x_filter, permute_tbl, horiz_const);

            store_s16_4x4(dst_ptr, dst_stride, d0, d1, d2, d3);

            src_ptr += 4 * src_stride;
            dst_ptr += 4 * dst_stride;
            height -= 4;
        } while (height > 4);

        do {
            uint8x16_t s0 = vld1q_u8(src_ptr);

            int16x4_t d0 = convolve6_4_2d_h(s0, x_filter, permute_tbl, horiz_const);

            vst1_s16(dst_ptr, d0);

            src_ptr += src_stride;
            dst_ptr += dst_stride;
        } while (--height != 0);
    } else {
        const uint8x16x2_t permute_tbl = vld1q_u8_x2(kMatMulPermuteTbl);
        do {
            const uint8_t *s     = src_ptr;
            int16_t       *d     = dst_ptr;
            int            width = w;

            do {
                uint8x16_t s0, s1, s2, s3;
                load_u8_16x4(s, src_stride, &s0, &s1, &s2, &s3);

                int16x8_t d0 = convolve6_8_2d_h(s0, x_filter, permute_tbl, horiz_const);
                int16x8_t d1 = convolve6_8_2d_h(s1, x_filter, permute_tbl, horiz_const);
                int16x8_t d2 = convolve6_8_2d_h(s2, x_filter, permute_tbl, horiz_const);
                int16x8_t d3 = convolve6_8_2d_h(s3, x_filter, permute_tbl, horiz_const);

                store_s16_8x4(d, dst_stride, d0, d1, d2, d3);

                s += 8;
                d += 8;
                width -= 8;
            } while (width > 0);
            src_ptr += 4 * src_stride;
            dst_ptr += 4 * dst_stride;
            height -= 4;
        } while (height > 4);

        do {
            const uint8_t *s     = src_ptr;
            int16_t       *d     = dst_ptr;
            int            width = w;

            do {
                uint8x16_t s0 = vld1q_u8(s);

                int16x8_t d0 = convolve6_8_2d_h(s0, x_filter, permute_tbl, horiz_const);

                vst1q_s16(d, d0);

                s += 8;
                d += 8;
                width -= 8;
            } while (width > 0);
            src_ptr += src_stride;
            dst_ptr += dst_stride;
        } while (--height != 0);
    }
}

static inline int16x8_t convolve8_8_2d_h(uint8x16_t samples, const int8x8_t x_filter, const uint8x16x3_t permute_tbl,
                                         const int32x4_t horiz_const) {
    uint8x16_t permuted_samples[3];
    int32x4_t  sum[2];

    // Permute samples ready for dot product.
    // { 0,  1,  2,  3,  1,  2,  3,  4,  2,  3,  4,  5,  3,  4,  5,  6 }
    permuted_samples[0] = vqtbl1q_u8(samples, permute_tbl.val[0]);
    // { 4,  5,  6,  7,  5,  6,  7,  8,  6,  7,  8,  9,  7,  8,  9, 10 }
    permuted_samples[1] = vqtbl1q_u8(samples, permute_tbl.val[1]);
    // { 8,  9, 10, 11,  9, 10, 11, 12, 10, 11, 12, 13, 11, 12, 13, 14 }
    permuted_samples[2] = vqtbl1q_u8(samples, permute_tbl.val[2]);

    // First 4 output values.
    sum[0] = vusdotq_lane_s32(horiz_const, permuted_samples[0], x_filter, 0);
    sum[0] = vusdotq_lane_s32(sum[0], permuted_samples[1], x_filter, 1);
    // Second 4 output values.
    sum[1] = vusdotq_lane_s32(horiz_const, permuted_samples[1], x_filter, 0);
    sum[1] = vusdotq_lane_s32(sum[1], permuted_samples[2], x_filter, 1);

    // Narrow and re-pack.
    // We halved the convolution filter values so -1 from the right shift.
    return vcombine_s16(vshrn_n_s32(sum[0], ROUND0_BITS - 1), vshrn_n_s32(sum[1], ROUND0_BITS - 1));
}

static inline void dist_wtd_convolve_2d_horiz_8tap_neon_i8mm(const uint8_t *src, int src_stride, int16_t *im_block,
                                                             const int im_stride, const int16_t *x_filter_ptr,
                                                             const int im_h, int w) {
    const int bd = 8;
    // A shim of 1 << ((ROUND0_BITS - 1) - 1) enables us to use non-rounding
    // shifts - which are generally faster than rounding shifts on modern CPUs.
    // (The extra -1 is needed because we halved the filter values.)
    const int32x4_t horiz_const = vdupq_n_s32((1 << (bd + FILTER_BITS - 2)) + (1 << ((ROUND0_BITS - 1) - 1)));

    const uint8x16x3_t permute_tbl = vld1q_u8_x3(kDotProdPermuteTbl);
    // Filter values are even, so halve to reduce intermediate precision reqs.
    const int8x8_t x_filter = vshrn_n_s16(vld1q_s16(x_filter_ptr), 1);

    const uint8_t *src_ptr    = src;
    int16_t       *dst_ptr    = im_block;
    int            dst_stride = im_stride;
    int            height     = im_h;

    do {
        const uint8_t *s     = src_ptr;
        int16_t       *d     = dst_ptr;
        int            width = w;

        do {
            uint8x16_t s0, s1, s2, s3;
            load_u8_16x4(s, src_stride, &s0, &s1, &s2, &s3);

            int16x8_t d0 = convolve8_8_2d_h(s0, x_filter, permute_tbl, horiz_const);
            int16x8_t d1 = convolve8_8_2d_h(s1, x_filter, permute_tbl, horiz_const);
            int16x8_t d2 = convolve8_8_2d_h(s2, x_filter, permute_tbl, horiz_const);
            int16x8_t d3 = convolve8_8_2d_h(s3, x_filter, permute_tbl, horiz_const);

            store_s16_8x4(d, dst_stride, d0, d1, d2, d3);

            s += 8;
            d += 8;
            width -= 8;
        } while (width > 0);
        src_ptr += 4 * src_stride;
        dst_ptr += 4 * dst_stride;
        height -= 4;
    } while (height > 4);

    do {
        const uint8_t *s     = src_ptr;
        int16_t       *d     = dst_ptr;
        int            width = w;

        do {
            uint8x16_t s0 = vld1q_u8(s);

            int16x8_t d0 = convolve8_8_2d_h(s0, x_filter, permute_tbl, horiz_const);

            vst1q_s16(d, d0);

            s += 8;
            d += 8;
            width -= 8;
        } while (width > 0);
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    } while (--height != 0);
}

void svt_av1_jnt_convolve_2d_neon_i8mm(const uint8_t *src, int src_stride, uint8_t *dst8, int dst8_stride, int w, int h,
                                       InterpFilterParams *filter_params_x, InterpFilterParams *filter_params_y,
                                       const int subpel_x_qn, const int subpel_y_qn, ConvolveParams *conv_params) {
    if (w == 2 || h == 2) {
        svt_av1_jnt_convolve_2d_c(src,
                                  src_stride,
                                  dst8,
                                  dst8_stride,
                                  w,
                                  h,
                                  filter_params_x,
                                  filter_params_y,
                                  subpel_x_qn,
                                  subpel_y_qn,
                                  conv_params);
        return;
    }

    assert(w % 4 == 0);
    assert(h % 4 == 0);

    DECLARE_ALIGNED(16, int16_t, im_block[(MAX_SB_SIZE + SUBPEL_TAPS - 1) * MAX_SB_SIZE]);

    const int x_filter_taps  = get_filter_tap(filter_params_x, subpel_x_qn);
    const int clamped_x_taps = x_filter_taps < 6 ? 6 : x_filter_taps;
    const int y_filter_taps  = get_filter_tap(filter_params_y, subpel_y_qn);
    const int clamped_y_taps = y_filter_taps < 6 ? 6 : y_filter_taps;

    const int      im_h         = h + clamped_y_taps - 1;
    const int      im_stride    = MAX_SB_SIZE;
    const int      vert_offset  = clamped_y_taps / 2 - 1;
    const int      horiz_offset = clamped_x_taps / 2 - 1;
    const uint8_t *src_ptr      = src - vert_offset * src_stride - horiz_offset;
    const int16_t *x_filter_ptr = av1_get_interp_filter_subpel_kernel(*filter_params_x, subpel_x_qn & SUBPEL_MASK);
    const int16_t *y_filter_ptr = av1_get_interp_filter_subpel_kernel(*filter_params_y, subpel_y_qn & SUBPEL_MASK);

    const int16x8_t y_filter = vld1q_s16(y_filter_ptr);

    if (clamped_x_taps == 6) {
        dist_wtd_convolve_2d_horiz_6tap_neon_i8mm(src_ptr, src_stride, im_block, im_stride, x_filter_ptr, im_h, w);
    } else {
        dist_wtd_convolve_2d_horiz_8tap_neon_i8mm(src_ptr, src_stride, im_block, im_stride, x_filter_ptr, im_h, w);
    }

    if (clamped_y_taps == 6) {
        if (conv_params->do_average) {
            if (UNLIKELY(conv_params->use_jnt_comp_avg)) {
                dist_wtd_convolve_2d_vert_6tap_dist_wtd_avg_neon(
                    im_block, im_stride, dst8, dst8_stride, conv_params, y_filter, h, w);
            } else {
                dist_wtd_convolve_2d_vert_6tap_avg_neon(
                    im_block, im_stride, dst8, dst8_stride, conv_params, y_filter, h, w);
            }
        } else {
            dist_wtd_convolve_2d_vert_6tap_neon(im_block, im_stride, conv_params, y_filter, h, w);
        }
    } else {
        if (conv_params->do_average) {
            if (UNLIKELY(conv_params->use_jnt_comp_avg)) {
                dist_wtd_convolve_2d_vert_8tap_dist_wtd_avg_neon(
                    im_block, im_stride, dst8, dst8_stride, conv_params, y_filter, h, w);
            } else {
                dist_wtd_convolve_2d_vert_8tap_avg_neon(
                    im_block, im_stride, dst8, dst8_stride, conv_params, y_filter, h, w);
            }
        } else {
            dist_wtd_convolve_2d_vert_8tap_neon(im_block, im_stride, conv_params, y_filter, h, w);
        }
    }
}
