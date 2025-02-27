/*
* Copyright(c) 2024 Intel Corporation
* Copyright (c) 2024, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include <arm_neon.h>

#include "definitions.h"
#include "common_dsp_rtcd.h"

#include "highbd_txfm_utility_neon.h"
#include "inv_transforms.h"
#include "mem_neon.h"
#include "transpose_neon.h"

static INLINE void round_shift_neon(int32x4_t *in, int32_t shift, int n) {
    const int32x4_t vshift = vdupq_n_s32(shift);
    for (int i = 0; i < n; i++) { in[i] = vrshlq_s32(in[i], vshift); }
}

static INLINE void round_shift_4x4(int32x4_t in[], int32_t shift) {
    const int32x4_t vshift = vdupq_n_s32(-shift); // sign change so that shift > 0 is a shift-right
    in[0]                  = vrshlq_s32(in[0], vshift);
    in[1]                  = vrshlq_s32(in[1], vshift);
    in[2]                  = vrshlq_s32(in[2], vshift);
    in[3]                  = vrshlq_s32(in[3], vshift);
}

static INLINE void round_shift_8x8(int32x4_t in[], int32_t shift) {
    round_shift_4x4(&in[0], shift);
    round_shift_4x4(&in[4], shift);
    round_shift_4x4(&in[8], shift);
    round_shift_4x4(&in[12], shift);
}

static INLINE void round_shift_16x16(int32x4_t in[], int32_t shift) {
    round_shift_8x8(&in[0], shift);
    round_shift_8x8(&in[16], shift);
    round_shift_8x8(&in[32], shift);
    round_shift_8x8(&in[48], shift);
}

static INLINE void round_shift_32x32(int32x4_t in[], int32_t shift) {
    round_shift_16x16(&in[0], shift);
    round_shift_16x16(&in[64], shift);
    round_shift_16x16(&in[128], shift);
    round_shift_16x16(&in[192], shift);
}

static INLINE void swap_addr(uint16_t **output1, uint16_t **output2) {
    uint16_t *tmp = *output1;
    *output1      = *output2;
    *output2      = tmp;
}

static INLINE void assign_16x16_input_from_32x32(const int32x4_t in[], int32x4_t out[], int32_t col) {
    for (int32_t i = 0; i < 16 * 16 / 4; i += 4) {
        out[i]     = in[col];
        out[i + 1] = in[col + 1];
        out[i + 2] = in[col + 2];
        out[i + 3] = in[col + 3];
        col += 8;
    }
}

static INLINE int32x4_t revert_int32x4_register(int32x4_t in) {
    const int32x4_t r_in = vrev64q_s32(in);
    return vextq_s32(r_in, r_in, 2);
}

static INLINE uint16x8_t get_recon_8x8(const int16x8_t pred, int32x4_t res_lo, int32x4_t res_hi, int32_t fliplr,
                                       int32_t bd) {
    int32x4_t x0, x1;

    if (fliplr) {
        res_lo = revert_int32x4_register(res_lo);
        res_hi = revert_int32x4_register(res_hi);
        x0     = vaddw_s16(res_hi, vget_low_s16(pred));
        x1     = vaddw_s16(res_lo, vget_high_s16(pred));
    } else {
        x0 = vaddw_s16(res_lo, vget_low_s16(pred));
        x1 = vaddw_s16(res_hi, vget_high_s16(pred));
    }

    const uint16x8_t x = vcombine_u16(vqmovun_s32(x0), vqmovun_s32(x1));
    return vminq_u16(x, vdupq_n_u16((1 << bd) - 1));
}

static INLINE void get_recon_16x16(const int16x8_t pred_lo, const int16x8_t pred_hi, int32x4_t *in, uint16x8_t *res_lo,
                                   uint16x8_t *res_hi, int32_t fliplr, int32_t bd) {
    int32x4_t x0, x1, x2, x3;

    if (fliplr) {
        in[0 * 16] = revert_int32x4_register(in[0 * 16]);
        in[1 * 16] = revert_int32x4_register(in[1 * 16]);
        in[2 * 16] = revert_int32x4_register(in[2 * 16]);
        in[3 * 16] = revert_int32x4_register(in[3 * 16]);
        x0         = vaddw_s16(in[3 * 16], vget_low_s16(pred_lo));
        x1         = vaddw_s16(in[2 * 16], vget_high_s16(pred_lo));
        x2         = vaddw_s16(in[1 * 16], vget_low_s16(pred_hi));
        x3         = vaddw_s16(in[0 * 16], vget_high_s16(pred_hi));
    } else {
        x0 = vaddw_s16(in[0 * 16], vget_low_s16(pred_lo));
        x1 = vaddw_s16(in[1 * 16], vget_high_s16(pred_lo));
        x2 = vaddw_s16(in[2 * 16], vget_low_s16(pred_hi));
        x3 = vaddw_s16(in[3 * 16], vget_high_s16(pred_hi));
    }

    *res_lo = vcombine_u16(vqmovun_s32(x0), vqmovun_s32(x1));
    *res_hi = vcombine_u16(vqmovun_s32(x2), vqmovun_s32(x3));
    *res_lo = vminq_u16(*res_lo, vdupq_n_u16((1 << bd) - 1));
    *res_hi = vminq_u16(*res_hi, vdupq_n_u16((1 << bd) - 1));
}

static INLINE void write_buffer_8x8(int32x4_t in[], uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                    int32_t stride_w, int32_t fliplr, int32_t flipud, int32_t shift, int32_t bd) {
    uint16x8_t u0, u1, u2, u3, u4, u5, u6, u7;

    round_shift_8x8(in, shift);

    const int16x8_t v0 = vld1q_s16((int16_t *)output_r + 0 * stride_r);
    const int16x8_t v1 = vld1q_s16((int16_t *)output_r + 1 * stride_r);
    const int16x8_t v2 = vld1q_s16((int16_t *)output_r + 2 * stride_r);
    const int16x8_t v3 = vld1q_s16((int16_t *)output_r + 3 * stride_r);
    const int16x8_t v4 = vld1q_s16((int16_t *)output_r + 4 * stride_r);
    const int16x8_t v5 = vld1q_s16((int16_t *)output_r + 5 * stride_r);
    const int16x8_t v6 = vld1q_s16((int16_t *)output_r + 6 * stride_r);
    const int16x8_t v7 = vld1q_s16((int16_t *)output_r + 7 * stride_r);

    if (flipud) {
        u0 = get_recon_8x8(v0, in[14], in[15], fliplr, bd);
        u1 = get_recon_8x8(v1, in[12], in[13], fliplr, bd);
        u2 = get_recon_8x8(v2, in[10], in[11], fliplr, bd);
        u3 = get_recon_8x8(v3, in[8], in[9], fliplr, bd);
        u4 = get_recon_8x8(v4, in[6], in[7], fliplr, bd);
        u5 = get_recon_8x8(v5, in[4], in[5], fliplr, bd);
        u6 = get_recon_8x8(v6, in[2], in[3], fliplr, bd);
        u7 = get_recon_8x8(v7, in[0], in[1], fliplr, bd);
    } else {
        u0 = get_recon_8x8(v0, in[0], in[1], fliplr, bd);
        u1 = get_recon_8x8(v1, in[2], in[3], fliplr, bd);
        u2 = get_recon_8x8(v2, in[4], in[5], fliplr, bd);
        u3 = get_recon_8x8(v3, in[6], in[7], fliplr, bd);
        u4 = get_recon_8x8(v4, in[8], in[9], fliplr, bd);
        u5 = get_recon_8x8(v5, in[10], in[11], fliplr, bd);
        u6 = get_recon_8x8(v6, in[12], in[13], fliplr, bd);
        u7 = get_recon_8x8(v7, in[14], in[15], fliplr, bd);
    }

    vst1q_u16(output_w + 0 * stride_w, u0);
    vst1q_u16(output_w + 1 * stride_w, u1);
    vst1q_u16(output_w + 2 * stride_w, u2);
    vst1q_u16(output_w + 3 * stride_w, u3);
    vst1q_u16(output_w + 4 * stride_w, u4);
    vst1q_u16(output_w + 5 * stride_w, u5);
    vst1q_u16(output_w + 6 * stride_w, u6);
    vst1q_u16(output_w + 7 * stride_w, u7);
}

static INLINE void write_buffer_8x8_(int32x4_t *in, const uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                     int32_t stride_w, int32_t fliplr, int32_t flipud, int32_t shift, int32_t bd) {
    uint16x8_t u[8];
    int16x8_t  v[8];

    round_shift_neon(in, shift, 16);

    load_s16_8x8((int16_t *)output_r, stride_r, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);

    if (flipud) {
        u[0] = get_recon_8x8(v[0], in[7], in[15], fliplr, bd);
        u[1] = get_recon_8x8(v[1], in[6], in[14], fliplr, bd);
        u[2] = get_recon_8x8(v[2], in[5], in[13], fliplr, bd);
        u[3] = get_recon_8x8(v[3], in[4], in[12], fliplr, bd);
        u[4] = get_recon_8x8(v[4], in[3], in[11], fliplr, bd);
        u[5] = get_recon_8x8(v[5], in[2], in[10], fliplr, bd);
        u[6] = get_recon_8x8(v[6], in[1], in[9], fliplr, bd);
        u[7] = get_recon_8x8(v[7], in[0], in[8], fliplr, bd);
    } else {
        u[0] = get_recon_8x8(v[0], in[0], in[8], fliplr, bd);
        u[1] = get_recon_8x8(v[1], in[1], in[9], fliplr, bd);
        u[2] = get_recon_8x8(v[2], in[2], in[10], fliplr, bd);
        u[3] = get_recon_8x8(v[3], in[3], in[11], fliplr, bd);
        u[4] = get_recon_8x8(v[4], in[4], in[12], fliplr, bd);
        u[5] = get_recon_8x8(v[5], in[5], in[13], fliplr, bd);
        u[6] = get_recon_8x8(v[6], in[6], in[14], fliplr, bd);
        u[7] = get_recon_8x8(v[7], in[7], in[15], fliplr, bd);
    }

    store_u16_8x8(output_w, stride_w, u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7]);
}

static INLINE void assign_8x8_input_from_16x16(const int32x4_t in[], int32x4_t in8x8[], int32_t col) {
    for (int32_t i = 0; i < 16; i += 2) {
        in8x8[i]     = in[col];
        in8x8[i + 1] = in[col + 1];
        col += 4;
    }
}

static INLINE void write_buffer_16x16(int32x4_t in[], uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                      int32_t stride_w, int32_t fliplr, int32_t flipud, int32_t shift, int32_t bd) {
    int32x4_t in8x8[16];

    uint16_t *left_up_r    = &output_r[0];
    uint16_t *right_up_r   = &output_r[8];
    uint16_t *left_down_r  = &output_r[8 * stride_r];
    uint16_t *right_down_r = &output_r[8 * stride_r + 8];
    uint16_t *left_up_w    = &output_w[0];
    uint16_t *right_up_w   = &output_w[8];
    uint16_t *left_down_w  = &output_w[8 * stride_w];
    uint16_t *right_down_w = &output_w[8 * stride_w + 8];

    if (fliplr) {
        swap_addr(&left_up_r, &right_up_r);
        swap_addr(&left_down_r, &right_down_r);
        swap_addr(&left_up_w, &right_up_w);
        swap_addr(&left_down_w, &right_down_w);
    }

    if (flipud) {
        swap_addr(&left_up_r, &left_down_r);
        swap_addr(&right_up_r, &right_down_r);
        swap_addr(&left_up_w, &left_down_w);
        swap_addr(&right_up_w, &right_down_w);
    }

    // Left-up quarter
    assign_8x8_input_from_16x16(in, in8x8, 0);
    write_buffer_8x8(in8x8, left_up_r, stride_r, left_up_w, stride_w, fliplr, flipud, shift, bd);

    // Right-up quarter
    assign_8x8_input_from_16x16(in, in8x8, 2);
    write_buffer_8x8(in8x8, right_up_r, stride_r, right_up_w, stride_w, fliplr, flipud, shift, bd);

    // Left-down quarter
    assign_8x8_input_from_16x16(in, in8x8, 32);
    write_buffer_8x8(in8x8, left_down_r, stride_r, left_down_w, stride_w, fliplr, flipud, shift, bd);

    // Right-down quarter
    assign_8x8_input_from_16x16(in, in8x8, 34);
    write_buffer_8x8(in8x8, right_down_r, stride_r, right_down_w, stride_w, fliplr, flipud, shift, bd);
}

static INLINE void write_buffer_16x16_(int32x4_t in[], uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                       int32_t stride_w, int32_t fliplr, int32_t flipud, int32_t shift, int32_t bd) {
    round_shift_neon(in, shift, 64);

    for (int i = 0; i < 16; i++) {
        const int16x8_t v0 = vld1q_s16((int16_t *)output_r + i * stride_r + 0);
        const int16x8_t v1 = vld1q_s16((int16_t *)output_r + i * stride_r + 8);
        uint16x8_t      res0, res1;

        if (flipud) {
            get_recon_16x16(v0, v1, &in[15 - i], &res0, &res1, fliplr, bd);
        } else {
            get_recon_16x16(v0, v1, &in[i], &res0, &res1, fliplr, bd);
        }

        vst1q_u16(output_w + i * stride_w + 0, res0);
        vst1q_u16(output_w + i * stride_w + 8, res1);
    }
}

static INLINE void write_buffer_32x32(int32x4_t in[], uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                      int32_t stride_w, int32_t fliplr, int32_t flipud, int32_t shift, int32_t bd) {
    int32x4_t in16x16[16 * 16 / 4];

    uint16_t *left_up_r    = &output_r[0];
    uint16_t *right_up_r   = &output_r[16];
    uint16_t *left_down_r  = &output_r[16 * stride_r];
    uint16_t *right_down_r = &output_r[16 * stride_r + 16];
    uint16_t *left_up_w    = &output_w[0];
    uint16_t *right_up_w   = &output_w[16];
    uint16_t *left_down_w  = &output_w[16 * stride_w];
    uint16_t *right_down_w = &output_w[16 * stride_w + 16];

    if (fliplr) {
        swap_addr(&left_up_r, &right_up_r);
        swap_addr(&left_down_r, &right_down_r);
        swap_addr(&left_up_w, &right_up_w);
        swap_addr(&left_down_w, &right_down_w);
    }

    if (flipud) {
        swap_addr(&left_up_r, &left_down_r);
        swap_addr(&right_up_r, &right_down_r);
        swap_addr(&left_up_w, &left_down_w);
        swap_addr(&right_up_w, &right_down_w);
    }

    // Left-up quarter
    assign_16x16_input_from_32x32(in, in16x16, 0);
    write_buffer_16x16(in16x16, left_up_r, stride_r, left_up_w, stride_w, fliplr, flipud, shift, bd);

    // Right-up quarter
    assign_16x16_input_from_32x32(in, in16x16, 32 / 2 / 4);
    write_buffer_16x16(in16x16, right_up_r, stride_r, right_up_w, stride_w, fliplr, flipud, shift, bd);

    // Left-down quarter
    assign_16x16_input_from_32x32(in, in16x16, 32 * 32 / 2 / 4);
    write_buffer_16x16(in16x16, left_down_r, stride_r, left_down_w, stride_w, fliplr, flipud, shift, bd);

    // Right-down quarter
    assign_16x16_input_from_32x32(in, in16x16, 32 * 32 / 2 / 4 + 32 / 2 / 4);
    write_buffer_16x16(in16x16, right_down_r, stride_r, right_down_w, stride_w, fliplr, flipud, shift, bd);
}

static INLINE void idct32_neon(int32x4_t in[], int32x4_t out[], int32_t bit) {
    const int32_t  *cospi    = cospi_arr(bit);
    const int32x4_t cospi62  = vdupq_n_s32(cospi[62]);
    const int32x4_t cospi30  = vdupq_n_s32(cospi[30]);
    const int32x4_t cospi46  = vdupq_n_s32(cospi[46]);
    const int32x4_t cospi14  = vdupq_n_s32(cospi[14]);
    const int32x4_t cospi54  = vdupq_n_s32(cospi[54]);
    const int32x4_t cospi22  = vdupq_n_s32(cospi[22]);
    const int32x4_t cospi38  = vdupq_n_s32(cospi[38]);
    const int32x4_t cospi6   = vdupq_n_s32(cospi[6]);
    const int32x4_t cospi58  = vdupq_n_s32(cospi[58]);
    const int32x4_t cospi26  = vdupq_n_s32(cospi[26]);
    const int32x4_t cospi42  = vdupq_n_s32(cospi[42]);
    const int32x4_t cospi10  = vdupq_n_s32(cospi[10]);
    const int32x4_t cospi50  = vdupq_n_s32(cospi[50]);
    const int32x4_t cospi18  = vdupq_n_s32(cospi[18]);
    const int32x4_t cospi34  = vdupq_n_s32(cospi[34]);
    const int32x4_t cospi2   = vdupq_n_s32(cospi[2]);
    const int32x4_t cospim58 = vdupq_n_s32(-cospi[58]);
    const int32x4_t cospim26 = vdupq_n_s32(-cospi[26]);
    const int32x4_t cospim42 = vdupq_n_s32(-cospi[42]);
    const int32x4_t cospim10 = vdupq_n_s32(-cospi[10]);
    const int32x4_t cospim50 = vdupq_n_s32(-cospi[50]);
    const int32x4_t cospim18 = vdupq_n_s32(-cospi[18]);
    const int32x4_t cospim34 = vdupq_n_s32(-cospi[34]);
    const int32x4_t cospim2  = vdupq_n_s32(-cospi[2]);
    const int32x4_t cospi60  = vdupq_n_s32(cospi[60]);
    const int32x4_t cospi28  = vdupq_n_s32(cospi[28]);
    const int32x4_t cospi44  = vdupq_n_s32(cospi[44]);
    const int32x4_t cospi12  = vdupq_n_s32(cospi[12]);
    const int32x4_t cospi52  = vdupq_n_s32(cospi[52]);
    const int32x4_t cospi20  = vdupq_n_s32(cospi[20]);
    const int32x4_t cospi36  = vdupq_n_s32(cospi[36]);
    const int32x4_t cospi4   = vdupq_n_s32(cospi[4]);
    const int32x4_t cospim52 = vdupq_n_s32(-cospi[52]);
    const int32x4_t cospim20 = vdupq_n_s32(-cospi[20]);
    const int32x4_t cospim36 = vdupq_n_s32(-cospi[36]);
    const int32x4_t cospim4  = vdupq_n_s32(-cospi[4]);
    const int32x4_t cospi56  = vdupq_n_s32(cospi[56]);
    const int32x4_t cospi24  = vdupq_n_s32(cospi[24]);
    const int32x4_t cospi40  = vdupq_n_s32(cospi[40]);
    const int32x4_t cospi8   = vdupq_n_s32(cospi[8]);
    const int32x4_t cospim40 = vdupq_n_s32(-cospi[40]);
    const int32x4_t cospim8  = vdupq_n_s32(-cospi[8]);
    const int32x4_t cospim56 = vdupq_n_s32(-cospi[56]);
    const int32x4_t cospim24 = vdupq_n_s32(-cospi[24]);
    const int32x4_t cospi32  = vdupq_n_s32(cospi[32]);
    const int32x4_t cospim32 = vdupq_n_s32(-cospi[32]);
    const int32x4_t cospi48  = vdupq_n_s32(cospi[48]);
    const int32x4_t cospim48 = vdupq_n_s32(-cospi[48]);
    const int32x4_t cospi16  = vdupq_n_s32(cospi[16]);
    const int32x4_t cospim16 = vdupq_n_s32(-cospi[16]);

    int32x4_t bf1[32], bf0[32];

    for (int32_t col = 0; col < 8; ++col) {
        // stage 0
        // stage 1
        bf1[0]  = in[0 * 8 + col];
        bf1[1]  = in[16 * 8 + col];
        bf1[2]  = in[8 * 8 + col];
        bf1[3]  = in[24 * 8 + col];
        bf1[4]  = in[4 * 8 + col];
        bf1[5]  = in[20 * 8 + col];
        bf1[6]  = in[12 * 8 + col];
        bf1[7]  = in[28 * 8 + col];
        bf1[8]  = in[2 * 8 + col];
        bf1[9]  = in[18 * 8 + col];
        bf1[10] = in[10 * 8 + col];
        bf1[11] = in[26 * 8 + col];
        bf1[12] = in[6 * 8 + col];
        bf1[13] = in[22 * 8 + col];
        bf1[14] = in[14 * 8 + col];
        bf1[15] = in[30 * 8 + col];
        bf1[16] = in[1 * 8 + col];
        bf1[17] = in[17 * 8 + col];
        bf1[18] = in[9 * 8 + col];
        bf1[19] = in[25 * 8 + col];
        bf1[20] = in[5 * 8 + col];
        bf1[21] = in[21 * 8 + col];
        bf1[22] = in[13 * 8 + col];
        bf1[23] = in[29 * 8 + col];
        bf1[24] = in[3 * 8 + col];
        bf1[25] = in[19 * 8 + col];
        bf1[26] = in[11 * 8 + col];
        bf1[27] = in[27 * 8 + col];
        bf1[28] = in[7 * 8 + col];
        bf1[29] = in[23 * 8 + col];
        bf1[30] = in[15 * 8 + col];
        bf1[31] = in[31 * 8 + col];

        // stage 2
        bf0[0]  = bf1[0];
        bf0[1]  = bf1[1];
        bf0[2]  = bf1[2];
        bf0[3]  = bf1[3];
        bf0[4]  = bf1[4];
        bf0[5]  = bf1[5];
        bf0[6]  = bf1[6];
        bf0[7]  = bf1[7];
        bf0[8]  = bf1[8];
        bf0[9]  = bf1[9];
        bf0[10] = bf1[10];
        bf0[11] = bf1[11];
        bf0[12] = bf1[12];
        bf0[13] = bf1[13];
        bf0[14] = bf1[14];
        bf0[15] = bf1[15];
        bf0[16] = half_btf_neon(&cospi62, &bf1[16], &cospim2, &bf1[31], bit);
        bf0[17] = half_btf_neon(&cospi30, &bf1[17], &cospim34, &bf1[30], bit);
        bf0[18] = half_btf_neon(&cospi46, &bf1[18], &cospim18, &bf1[29], bit);
        bf0[19] = half_btf_neon(&cospi14, &bf1[19], &cospim50, &bf1[28], bit);
        bf0[20] = half_btf_neon(&cospi54, &bf1[20], &cospim10, &bf1[27], bit);
        bf0[21] = half_btf_neon(&cospi22, &bf1[21], &cospim42, &bf1[26], bit);
        bf0[22] = half_btf_neon(&cospi38, &bf1[22], &cospim26, &bf1[25], bit);
        bf0[23] = half_btf_neon(&cospi6, &bf1[23], &cospim58, &bf1[24], bit);
        bf0[24] = half_btf_neon(&cospi58, &bf1[23], &cospi6, &bf1[24], bit);
        bf0[25] = half_btf_neon(&cospi26, &bf1[22], &cospi38, &bf1[25], bit);
        bf0[26] = half_btf_neon(&cospi42, &bf1[21], &cospi22, &bf1[26], bit);
        bf0[27] = half_btf_neon(&cospi10, &bf1[20], &cospi54, &bf1[27], bit);
        bf0[28] = half_btf_neon(&cospi50, &bf1[19], &cospi14, &bf1[28], bit);
        bf0[29] = half_btf_neon(&cospi18, &bf1[18], &cospi46, &bf1[29], bit);
        bf0[30] = half_btf_neon(&cospi34, &bf1[17], &cospi30, &bf1[30], bit);
        bf0[31] = half_btf_neon(&cospi2, &bf1[16], &cospi62, &bf1[31], bit);

        // stage 3
        bf1[0]  = bf0[0];
        bf1[1]  = bf0[1];
        bf1[2]  = bf0[2];
        bf1[3]  = bf0[3];
        bf1[4]  = bf0[4];
        bf1[5]  = bf0[5];
        bf1[6]  = bf0[6];
        bf1[7]  = bf0[7];
        bf1[8]  = half_btf_neon(&cospi60, &bf0[8], &cospim4, &bf0[15], bit);
        bf1[9]  = half_btf_neon(&cospi28, &bf0[9], &cospim36, &bf0[14], bit);
        bf1[10] = half_btf_neon(&cospi44, &bf0[10], &cospim20, &bf0[13], bit);
        bf1[11] = half_btf_neon(&cospi12, &bf0[11], &cospim52, &bf0[12], bit);
        bf1[12] = half_btf_neon(&cospi52, &bf0[11], &cospi12, &bf0[12], bit);
        bf1[13] = half_btf_neon(&cospi20, &bf0[10], &cospi44, &bf0[13], bit);
        bf1[14] = half_btf_neon(&cospi36, &bf0[9], &cospi28, &bf0[14], bit);
        bf1[15] = half_btf_neon(&cospi4, &bf0[8], &cospi60, &bf0[15], bit);
        bf1[16] = vaddq_s32(bf0[16], bf0[17]);
        bf1[17] = vsubq_s32(bf0[16], bf0[17]);
        bf1[18] = vsubq_s32(bf0[19], bf0[18]);
        bf1[19] = vaddq_s32(bf0[18], bf0[19]);
        bf1[20] = vaddq_s32(bf0[20], bf0[21]);
        bf1[21] = vsubq_s32(bf0[20], bf0[21]);
        bf1[22] = vsubq_s32(bf0[23], bf0[22]);
        bf1[23] = vaddq_s32(bf0[22], bf0[23]);
        bf1[24] = vaddq_s32(bf0[24], bf0[25]);
        bf1[25] = vsubq_s32(bf0[24], bf0[25]);
        bf1[26] = vsubq_s32(bf0[27], bf0[26]);
        bf1[27] = vaddq_s32(bf0[26], bf0[27]);
        bf1[28] = vaddq_s32(bf0[28], bf0[29]);
        bf1[29] = vsubq_s32(bf0[28], bf0[29]);
        bf1[30] = vsubq_s32(bf0[31], bf0[30]);
        bf1[31] = vaddq_s32(bf0[30], bf0[31]);

        // stage 4
        bf0[0]  = bf1[0];
        bf0[1]  = bf1[1];
        bf0[2]  = bf1[2];
        bf0[3]  = bf1[3];
        bf0[4]  = half_btf_neon(&cospi56, &bf1[4], &cospim8, &bf1[7], bit);
        bf0[5]  = half_btf_neon(&cospi24, &bf1[5], &cospim40, &bf1[6], bit);
        bf0[6]  = half_btf_neon(&cospi40, &bf1[5], &cospi24, &bf1[6], bit);
        bf0[7]  = half_btf_neon(&cospi8, &bf1[4], &cospi56, &bf1[7], bit);
        bf0[8]  = vaddq_s32(bf1[8], bf1[9]);
        bf0[9]  = vsubq_s32(bf1[8], bf1[9]);
        bf0[10] = vsubq_s32(bf1[11], bf1[10]);
        bf0[11] = vaddq_s32(bf1[10], bf1[11]);
        bf0[12] = vaddq_s32(bf1[12], bf1[13]);
        bf0[13] = vsubq_s32(bf1[12], bf1[13]);
        bf0[14] = vsubq_s32(bf1[15], bf1[14]);
        bf0[15] = vaddq_s32(bf1[14], bf1[15]);
        bf0[16] = bf1[16];
        bf0[17] = half_btf_neon(&cospim8, &bf1[17], &cospi56, &bf1[30], bit);
        bf0[18] = half_btf_neon(&cospim56, &bf1[18], &cospim8, &bf1[29], bit);
        bf0[19] = bf1[19];
        bf0[20] = bf1[20];
        bf0[21] = half_btf_neon(&cospim40, &bf1[21], &cospi24, &bf1[26], bit);
        bf0[22] = half_btf_neon(&cospim24, &bf1[22], &cospim40, &bf1[25], bit);
        bf0[23] = bf1[23];
        bf0[24] = bf1[24];
        bf0[25] = half_btf_neon(&cospim40, &bf1[22], &cospi24, &bf1[25], bit);
        bf0[26] = half_btf_neon(&cospi24, &bf1[21], &cospi40, &bf1[26], bit);
        bf0[27] = bf1[27];
        bf0[28] = bf1[28];
        bf0[29] = half_btf_neon(&cospim8, &bf1[18], &cospi56, &bf1[29], bit);
        bf0[30] = half_btf_neon(&cospi56, &bf1[17], &cospi8, &bf1[30], bit);
        bf0[31] = bf1[31];

        // stage 5
        bf1[0]  = half_btf_neon(&cospi32, &bf0[0], &cospi32, &bf0[1], bit);
        bf1[1]  = half_btf_neon(&cospi32, &bf0[0], &cospim32, &bf0[1], bit);
        bf1[2]  = half_btf_neon(&cospi48, &bf0[2], &cospim16, &bf0[3], bit);
        bf1[3]  = half_btf_neon(&cospi16, &bf0[2], &cospi48, &bf0[3], bit);
        bf1[4]  = vaddq_s32(bf0[4], bf0[5]);
        bf1[5]  = vsubq_s32(bf0[4], bf0[5]);
        bf1[6]  = vsubq_s32(bf0[7], bf0[6]);
        bf1[7]  = vaddq_s32(bf0[6], bf0[7]);
        bf1[8]  = bf0[8];
        bf1[9]  = half_btf_neon(&cospim16, &bf0[9], &cospi48, &bf0[14], bit);
        bf1[10] = half_btf_neon(&cospim48, &bf0[10], &cospim16, &bf0[13], bit);
        bf1[11] = bf0[11];
        bf1[12] = bf0[12];
        bf1[13] = half_btf_neon(&cospim16, &bf0[10], &cospi48, &bf0[13], bit);
        bf1[14] = half_btf_neon(&cospi48, &bf0[9], &cospi16, &bf0[14], bit);
        bf1[15] = bf0[15];
        bf1[16] = vaddq_s32(bf0[16], bf0[19]);
        bf1[17] = vaddq_s32(bf0[17], bf0[18]);
        bf1[18] = vsubq_s32(bf0[17], bf0[18]);
        bf1[19] = vsubq_s32(bf0[16], bf0[19]);
        bf1[20] = vsubq_s32(bf0[23], bf0[20]);
        bf1[21] = vsubq_s32(bf0[22], bf0[21]);
        bf1[22] = vaddq_s32(bf0[21], bf0[22]);
        bf1[23] = vaddq_s32(bf0[20], bf0[23]);
        bf1[24] = vaddq_s32(bf0[24], bf0[27]);
        bf1[25] = vaddq_s32(bf0[25], bf0[26]);
        bf1[26] = vsubq_s32(bf0[25], bf0[26]);
        bf1[27] = vsubq_s32(bf0[24], bf0[27]);
        bf1[28] = vsubq_s32(bf0[31], bf0[28]);
        bf1[29] = vsubq_s32(bf0[30], bf0[29]);
        bf1[30] = vaddq_s32(bf0[29], bf0[30]);
        bf1[31] = vaddq_s32(bf0[28], bf0[31]);

        // stage 6
        bf0[0]  = vaddq_s32(bf1[0], bf1[3]);
        bf0[1]  = vaddq_s32(bf1[1], bf1[2]);
        bf0[2]  = vsubq_s32(bf1[1], bf1[2]);
        bf0[3]  = vsubq_s32(bf1[0], bf1[3]);
        bf0[4]  = bf1[4];
        bf0[5]  = half_btf_neon(&cospim32, &bf1[5], &cospi32, &bf1[6], bit);
        bf0[6]  = half_btf_neon(&cospi32, &bf1[5], &cospi32, &bf1[6], bit);
        bf0[7]  = bf1[7];
        bf0[8]  = vaddq_s32(bf1[8], bf1[11]);
        bf0[9]  = vaddq_s32(bf1[9], bf1[10]);
        bf0[10] = vsubq_s32(bf1[9], bf1[10]);
        bf0[11] = vsubq_s32(bf1[8], bf1[11]);
        bf0[12] = vsubq_s32(bf1[15], bf1[12]);
        bf0[13] = vsubq_s32(bf1[14], bf1[13]);
        bf0[14] = vaddq_s32(bf1[13], bf1[14]);
        bf0[15] = vaddq_s32(bf1[12], bf1[15]);
        bf0[16] = bf1[16];
        bf0[17] = bf1[17];
        bf0[18] = half_btf_neon(&cospim16, &bf1[18], &cospi48, &bf1[29], bit);
        bf0[19] = half_btf_neon(&cospim16, &bf1[19], &cospi48, &bf1[28], bit);
        bf0[20] = half_btf_neon(&cospim48, &bf1[20], &cospim16, &bf1[27], bit);
        bf0[21] = half_btf_neon(&cospim48, &bf1[21], &cospim16, &bf1[26], bit);
        bf0[22] = bf1[22];
        bf0[23] = bf1[23];
        bf0[24] = bf1[24];
        bf0[25] = bf1[25];
        bf0[26] = half_btf_neon(&cospim16, &bf1[21], &cospi48, &bf1[26], bit);
        bf0[27] = half_btf_neon(&cospim16, &bf1[20], &cospi48, &bf1[27], bit);
        bf0[28] = half_btf_neon(&cospi48, &bf1[19], &cospi16, &bf1[28], bit);
        bf0[29] = half_btf_neon(&cospi48, &bf1[18], &cospi16, &bf1[29], bit);
        bf0[30] = bf1[30];
        bf0[31] = bf1[31];

        // stage 7
        bf1[0]  = vaddq_s32(bf0[0], bf0[7]);
        bf1[1]  = vaddq_s32(bf0[1], bf0[6]);
        bf1[2]  = vaddq_s32(bf0[2], bf0[5]);
        bf1[3]  = vaddq_s32(bf0[3], bf0[4]);
        bf1[4]  = vsubq_s32(bf0[3], bf0[4]);
        bf1[5]  = vsubq_s32(bf0[2], bf0[5]);
        bf1[6]  = vsubq_s32(bf0[1], bf0[6]);
        bf1[7]  = vsubq_s32(bf0[0], bf0[7]);
        bf1[8]  = bf0[8];
        bf1[9]  = bf0[9];
        bf1[10] = half_btf_neon(&cospim32, &bf0[10], &cospi32, &bf0[13], bit);
        bf1[11] = half_btf_neon(&cospim32, &bf0[11], &cospi32, &bf0[12], bit);
        bf1[12] = half_btf_neon(&cospi32, &bf0[11], &cospi32, &bf0[12], bit);
        bf1[13] = half_btf_neon(&cospi32, &bf0[10], &cospi32, &bf0[13], bit);
        bf1[14] = bf0[14];
        bf1[15] = bf0[15];
        bf1[16] = vaddq_s32(bf0[16], bf0[23]);
        bf1[17] = vaddq_s32(bf0[17], bf0[22]);
        bf1[18] = vaddq_s32(bf0[18], bf0[21]);
        bf1[19] = vaddq_s32(bf0[19], bf0[20]);
        bf1[20] = vsubq_s32(bf0[19], bf0[20]);
        bf1[21] = vsubq_s32(bf0[18], bf0[21]);
        bf1[22] = vsubq_s32(bf0[17], bf0[22]);
        bf1[23] = vsubq_s32(bf0[16], bf0[23]);
        bf1[24] = vsubq_s32(bf0[31], bf0[24]);
        bf1[25] = vsubq_s32(bf0[30], bf0[25]);
        bf1[26] = vsubq_s32(bf0[29], bf0[26]);
        bf1[27] = vsubq_s32(bf0[28], bf0[27]);
        bf1[28] = vaddq_s32(bf0[27], bf0[28]);
        bf1[29] = vaddq_s32(bf0[26], bf0[29]);
        bf1[30] = vaddq_s32(bf0[25], bf0[30]);
        bf1[31] = vaddq_s32(bf0[24], bf0[31]);

        // stage 8
        bf0[0]  = vaddq_s32(bf1[0], bf1[15]);
        bf0[1]  = vaddq_s32(bf1[1], bf1[14]);
        bf0[2]  = vaddq_s32(bf1[2], bf1[13]);
        bf0[3]  = vaddq_s32(bf1[3], bf1[12]);
        bf0[4]  = vaddq_s32(bf1[4], bf1[11]);
        bf0[5]  = vaddq_s32(bf1[5], bf1[10]);
        bf0[6]  = vaddq_s32(bf1[6], bf1[9]);
        bf0[7]  = vaddq_s32(bf1[7], bf1[8]);
        bf0[8]  = vsubq_s32(bf1[7], bf1[8]);
        bf0[9]  = vsubq_s32(bf1[6], bf1[9]);
        bf0[10] = vsubq_s32(bf1[5], bf1[10]);
        bf0[11] = vsubq_s32(bf1[4], bf1[11]);
        bf0[12] = vsubq_s32(bf1[3], bf1[12]);
        bf0[13] = vsubq_s32(bf1[2], bf1[13]);
        bf0[14] = vsubq_s32(bf1[1], bf1[14]);
        bf0[15] = vsubq_s32(bf1[0], bf1[15]);
        bf0[16] = bf1[16];
        bf0[17] = bf1[17];
        bf0[18] = bf1[18];
        bf0[19] = bf1[19];
        bf0[20] = half_btf_neon(&cospim32, &bf1[20], &cospi32, &bf1[27], bit);
        bf0[21] = half_btf_neon(&cospim32, &bf1[21], &cospi32, &bf1[26], bit);
        bf0[22] = half_btf_neon(&cospim32, &bf1[22], &cospi32, &bf1[25], bit);
        bf0[23] = half_btf_neon(&cospim32, &bf1[23], &cospi32, &bf1[24], bit);
        bf0[24] = half_btf_neon(&cospi32, &bf1[23], &cospi32, &bf1[24], bit);
        bf0[25] = half_btf_neon(&cospi32, &bf1[22], &cospi32, &bf1[25], bit);
        bf0[26] = half_btf_neon(&cospi32, &bf1[21], &cospi32, &bf1[26], bit);
        bf0[27] = half_btf_neon(&cospi32, &bf1[20], &cospi32, &bf1[27], bit);
        bf0[28] = bf1[28];
        bf0[29] = bf1[29];
        bf0[30] = bf1[30];
        bf0[31] = bf1[31];

        // stage 9
        out[0 * 8 + col]  = vaddq_s32(bf0[0], bf0[31]);
        out[1 * 8 + col]  = vaddq_s32(bf0[1], bf0[30]);
        out[2 * 8 + col]  = vaddq_s32(bf0[2], bf0[29]);
        out[3 * 8 + col]  = vaddq_s32(bf0[3], bf0[28]);
        out[4 * 8 + col]  = vaddq_s32(bf0[4], bf0[27]);
        out[5 * 8 + col]  = vaddq_s32(bf0[5], bf0[26]);
        out[6 * 8 + col]  = vaddq_s32(bf0[6], bf0[25]);
        out[7 * 8 + col]  = vaddq_s32(bf0[7], bf0[24]);
        out[8 * 8 + col]  = vaddq_s32(bf0[8], bf0[23]);
        out[9 * 8 + col]  = vaddq_s32(bf0[9], bf0[22]);
        out[10 * 8 + col] = vaddq_s32(bf0[10], bf0[21]);
        out[11 * 8 + col] = vaddq_s32(bf0[11], bf0[20]);
        out[12 * 8 + col] = vaddq_s32(bf0[12], bf0[19]);
        out[13 * 8 + col] = vaddq_s32(bf0[13], bf0[18]);
        out[14 * 8 + col] = vaddq_s32(bf0[14], bf0[17]);
        out[15 * 8 + col] = vaddq_s32(bf0[15], bf0[16]);
        out[16 * 8 + col] = vsubq_s32(bf0[15], bf0[16]);
        out[17 * 8 + col] = vsubq_s32(bf0[14], bf0[17]);
        out[18 * 8 + col] = vsubq_s32(bf0[13], bf0[18]);
        out[19 * 8 + col] = vsubq_s32(bf0[12], bf0[19]);
        out[20 * 8 + col] = vsubq_s32(bf0[11], bf0[20]);
        out[21 * 8 + col] = vsubq_s32(bf0[10], bf0[21]);
        out[22 * 8 + col] = vsubq_s32(bf0[9], bf0[22]);
        out[23 * 8 + col] = vsubq_s32(bf0[8], bf0[23]);
        out[24 * 8 + col] = vsubq_s32(bf0[7], bf0[24]);
        out[25 * 8 + col] = vsubq_s32(bf0[6], bf0[25]);
        out[26 * 8 + col] = vsubq_s32(bf0[5], bf0[26]);
        out[27 * 8 + col] = vsubq_s32(bf0[4], bf0[27]);
        out[28 * 8 + col] = vsubq_s32(bf0[3], bf0[28]);
        out[29 * 8 + col] = vsubq_s32(bf0[2], bf0[29]);
        out[30 * 8 + col] = vsubq_s32(bf0[1], bf0[30]);
        out[31 * 8 + col] = vsubq_s32(bf0[0], bf0[31]);
    }
}

static INLINE void load_buffer_32x32(const int32_t *coeff, int32x4_t in[]) {
    for (int32_t i = 0; i < 256; ++i) {
        in[i] = vld1q_s32(coeff);
        coeff += 4;
    }
}

static INLINE void transpose_8nx8n(const int32x4_t input[], int32x4_t output[], const int width, const int height) {
    const int numcol = height >> 2;
    const int numrow = width >> 2;
    for (int j = 0; j < numrow; j++) {
        for (int i = 0; i < numcol; i++) {
            TRANSPOSE_4X4(input[i * width + j + (numrow * 0)],
                          input[i * width + j + (numrow * 1)],
                          input[i * width + j + (numrow * 2)],
                          input[i * width + j + (numrow * 3)],
                          output[j * height + i + (numcol * 0)],
                          output[j * height + i + (numcol * 1)],
                          output[j * height + i + (numcol * 2)],
                          output[j * height + i + (numcol * 3)]);
        }
    }
}

void svt_av1_inv_txfm2d_add_32x32_neon(const int32_t *input, uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                       int32_t stride_w, TxType tx_type, int32_t bd) {
    int32x4_t     in[256], out[256];
    const int8_t *shift   = svt_aom_inv_txfm_shift_ls[TX_32X32];
    const int32_t txw_idx = get_txw_idx(TX_32X32);
    const int32_t txh_idx = get_txh_idx(TX_32X32);

    switch (tx_type) {
    case DCT_DCT:
        load_buffer_32x32(input, in);
        transpose_8nx8n(in, out, 32, 32);
        idct32_neon(out, in, inv_cos_bit_row[txw_idx][txh_idx]);
        round_shift_32x32(in, -shift[0]);
        transpose_8nx8n(in, out, 32, 32);
        idct32_neon(out, in, inv_cos_bit_col[txw_idx][txh_idx]);
        write_buffer_32x32(in, output_r, stride_r, output_w, stride_w, 0, 0, -shift[1], bd);
        break;
    case IDTX:
        load_buffer_32x32(input, in);
        write_buffer_32x32(in, output_r, stride_r, output_w, stride_w, 0, 0, (-shift[0] - shift[1] - 4), bd);
        break;
    default: assert(0);
    }
}

static INLINE void load_buffer_64x64_lower_32x32(const int32_t *coeff, int32x4_t in[]) {
    const int32x4_t zero = vdupq_n_s32(0);

    for (int32_t i = 0; i < 32; ++i) {
        for (int32_t j = 0; j < 8; ++j) {
            in[16 * i + j]     = vld1q_s32(coeff + 32 * i + 4 * j);
            in[16 * i + j + 8] = zero;
        }
    }

    int32x4_t *inB = in + 512;
    for (size_t i = 0; i < 512; ++i) { inB[i] = zero; }
}

static INLINE void transpose_64x64(int32x4_t in[], int32x4_t out[], int32_t do_cols) {
    int32_t i, j;
    for (i = 0; i < (do_cols ? 16 : 8); ++i) {
        for (j = 0; j < 8; ++j) {
            TRANSPOSE_4X4(in[(4 * i + 0) * 16 + j],
                          in[(4 * i + 1) * 16 + j],
                          in[(4 * i + 2) * 16 + j],
                          in[(4 * i + 3) * 16 + j],
                          out[(4 * j + 0) * 16 + i],
                          out[(4 * j + 1) * 16 + i],
                          out[(4 * j + 2) * 16 + i],
                          out[(4 * j + 3) * 16 + i]);
        }
    }
}

static INLINE void addsub_neon(const int32x4_t in0, const int32x4_t in1, int32x4_t out0[], int32x4_t out1[],
                               const int32x4_t clamp_lo[], const int32x4_t clamp_hi[]) {
    int32x4_t a0 = vaddq_s32(in0, in1);
    int32x4_t a1 = vsubq_s32(in0, in1);

    a0 = vmaxq_s32(a0, *clamp_lo);
    a0 = vminq_s32(a0, *clamp_hi);
    a1 = vmaxq_s32(a1, *clamp_lo);
    a1 = vminq_s32(a1, *clamp_hi);

    *out0 = a0;
    *out1 = a1;
}

static void idct64x64_neon(int32x4_t in[], int32x4_t out[], int32_t bit, int32_t do_cols, int32_t bd) {
    const int32_t  *cospi     = cospi_arr(bit);
    const int32_t   log_range = AOMMAX(16, bd + (do_cols ? 6 : 8));
    const int32x4_t clamp_lo  = vdupq_n_s32(-(1 << (log_range - 1)));
    const int32x4_t clamp_hi  = vdupq_n_s32((1 << (log_range - 1)) - 1);
    const int32x4_t cospi1    = vdupq_n_s32(cospi[1]);
    const int32x4_t cospi2    = vdupq_n_s32(cospi[2]);
    const int32x4_t cospi3    = vdupq_n_s32(cospi[3]);
    const int32x4_t cospi4    = vdupq_n_s32(cospi[4]);
    const int32x4_t cospi5    = vdupq_n_s32(cospi[5]);
    const int32x4_t cospi6    = vdupq_n_s32(cospi[6]);
    const int32x4_t cospi7    = vdupq_n_s32(cospi[7]);
    const int32x4_t cospi8    = vdupq_n_s32(cospi[8]);
    const int32x4_t cospi9    = vdupq_n_s32(cospi[9]);
    const int32x4_t cospi10   = vdupq_n_s32(cospi[10]);
    const int32x4_t cospi11   = vdupq_n_s32(cospi[11]);
    const int32x4_t cospi12   = vdupq_n_s32(cospi[12]);
    const int32x4_t cospi13   = vdupq_n_s32(cospi[13]);
    const int32x4_t cospi14   = vdupq_n_s32(cospi[14]);
    const int32x4_t cospi15   = vdupq_n_s32(cospi[15]);
    const int32x4_t cospi16   = vdupq_n_s32(cospi[16]);
    const int32x4_t cospi17   = vdupq_n_s32(cospi[17]);
    const int32x4_t cospi18   = vdupq_n_s32(cospi[18]);
    const int32x4_t cospi19   = vdupq_n_s32(cospi[19]);
    const int32x4_t cospi20   = vdupq_n_s32(cospi[20]);
    const int32x4_t cospi21   = vdupq_n_s32(cospi[21]);
    const int32x4_t cospi22   = vdupq_n_s32(cospi[22]);
    const int32x4_t cospi23   = vdupq_n_s32(cospi[23]);
    const int32x4_t cospi24   = vdupq_n_s32(cospi[24]);
    const int32x4_t cospi25   = vdupq_n_s32(cospi[25]);
    const int32x4_t cospi26   = vdupq_n_s32(cospi[26]);
    const int32x4_t cospi27   = vdupq_n_s32(cospi[27]);
    const int32x4_t cospi28   = vdupq_n_s32(cospi[28]);
    const int32x4_t cospi29   = vdupq_n_s32(cospi[29]);
    const int32x4_t cospi30   = vdupq_n_s32(cospi[30]);
    const int32x4_t cospi31   = vdupq_n_s32(cospi[31]);
    const int32x4_t cospi32   = vdupq_n_s32(cospi[32]);
    const int32x4_t cospi35   = vdupq_n_s32(cospi[35]);
    const int32x4_t cospi36   = vdupq_n_s32(cospi[36]);
    const int32x4_t cospi38   = vdupq_n_s32(cospi[38]);
    const int32x4_t cospi39   = vdupq_n_s32(cospi[39]);
    const int32x4_t cospi40   = vdupq_n_s32(cospi[40]);
    const int32x4_t cospi43   = vdupq_n_s32(cospi[43]);
    const int32x4_t cospi44   = vdupq_n_s32(cospi[44]);
    const int32x4_t cospi46   = vdupq_n_s32(cospi[46]);
    const int32x4_t cospi47   = vdupq_n_s32(cospi[47]);
    const int32x4_t cospi48   = vdupq_n_s32(cospi[48]);
    const int32x4_t cospi51   = vdupq_n_s32(cospi[51]);
    const int32x4_t cospi52   = vdupq_n_s32(cospi[52]);
    const int32x4_t cospi54   = vdupq_n_s32(cospi[54]);
    const int32x4_t cospi55   = vdupq_n_s32(cospi[55]);
    const int32x4_t cospi56   = vdupq_n_s32(cospi[56]);
    const int32x4_t cospi59   = vdupq_n_s32(cospi[59]);
    const int32x4_t cospi60   = vdupq_n_s32(cospi[60]);
    const int32x4_t cospi62   = vdupq_n_s32(cospi[62]);
    const int32x4_t cospi63   = vdupq_n_s32(cospi[63]);

    const int32x4_t cospim4  = vdupq_n_s32(-cospi[4]);
    const int32x4_t cospim8  = vdupq_n_s32(-cospi[8]);
    const int32x4_t cospim12 = vdupq_n_s32(-cospi[12]);
    const int32x4_t cospim16 = vdupq_n_s32(-cospi[16]);
    const int32x4_t cospim20 = vdupq_n_s32(-cospi[20]);
    const int32x4_t cospim24 = vdupq_n_s32(-cospi[24]);
    const int32x4_t cospim28 = vdupq_n_s32(-cospi[28]);
    const int32x4_t cospim32 = vdupq_n_s32(-cospi[32]);
    const int32x4_t cospim33 = vdupq_n_s32(-cospi[33]);
    const int32x4_t cospim34 = vdupq_n_s32(-cospi[34]);
    const int32x4_t cospim36 = vdupq_n_s32(-cospi[36]);
    const int32x4_t cospim37 = vdupq_n_s32(-cospi[37]);
    const int32x4_t cospim40 = vdupq_n_s32(-cospi[40]);
    const int32x4_t cospim41 = vdupq_n_s32(-cospi[41]);
    const int32x4_t cospim42 = vdupq_n_s32(-cospi[42]);
    const int32x4_t cospim44 = vdupq_n_s32(-cospi[44]);
    const int32x4_t cospim45 = vdupq_n_s32(-cospi[45]);
    const int32x4_t cospim48 = vdupq_n_s32(-cospi[48]);
    const int32x4_t cospim49 = vdupq_n_s32(-cospi[49]);
    const int32x4_t cospim50 = vdupq_n_s32(-cospi[50]);
    const int32x4_t cospim52 = vdupq_n_s32(-cospi[52]);
    const int32x4_t cospim53 = vdupq_n_s32(-cospi[53]);
    const int32x4_t cospim56 = vdupq_n_s32(-cospi[56]);
    const int32x4_t cospim57 = vdupq_n_s32(-cospi[57]);
    const int32x4_t cospim58 = vdupq_n_s32(-cospi[58]);
    const int32x4_t cospim60 = vdupq_n_s32(-cospi[60]);
    const int32x4_t cospim61 = vdupq_n_s32(-cospi[61]);

    for (int32_t col = 0; col < (do_cols ? 64 / 4 : 32 / 4); ++col) {
        int32x4_t u[64], v[64];

        // stage 1
        u[32] = in[1 * 16 + col];
        u[34] = in[17 * 16 + col];
        u[36] = in[9 * 16 + col];
        u[38] = in[25 * 16 + col];
        u[40] = in[5 * 16 + col];
        u[42] = in[21 * 16 + col];
        u[44] = in[13 * 16 + col];
        u[46] = in[29 * 16 + col];
        u[48] = in[3 * 16 + col];
        u[50] = in[19 * 16 + col];
        u[52] = in[11 * 16 + col];
        u[54] = in[27 * 16 + col];
        u[56] = in[7 * 16 + col];
        u[58] = in[23 * 16 + col];
        u[60] = in[15 * 16 + col];
        u[62] = in[31 * 16 + col];

        v[16] = in[2 * 16 + col];
        v[18] = in[18 * 16 + col];
        v[20] = in[10 * 16 + col];
        v[22] = in[26 * 16 + col];
        v[24] = in[6 * 16 + col];
        v[26] = in[22 * 16 + col];
        v[28] = in[14 * 16 + col];
        v[30] = in[30 * 16 + col];

        u[8]  = in[4 * 16 + col];
        u[10] = in[20 * 16 + col];
        u[12] = in[12 * 16 + col];
        u[14] = in[28 * 16 + col];

        v[4] = in[8 * 16 + col];
        v[6] = in[24 * 16 + col];

        u[0] = in[0 * 16 + col];
        u[2] = in[16 * 16 + col];

        // stage 2
        v[32] = half_btf_0_neon(&cospi63, &u[32], bit);
        v[33] = half_btf_0_neon(&cospim33, &u[62], bit);
        v[34] = half_btf_0_neon(&cospi47, &u[34], bit);
        v[35] = half_btf_0_neon(&cospim49, &u[60], bit);
        v[36] = half_btf_0_neon(&cospi55, &u[36], bit);
        v[37] = half_btf_0_neon(&cospim41, &u[58], bit);
        v[38] = half_btf_0_neon(&cospi39, &u[38], bit);
        v[39] = half_btf_0_neon(&cospim57, &u[56], bit);
        v[40] = half_btf_0_neon(&cospi59, &u[40], bit);
        v[41] = half_btf_0_neon(&cospim37, &u[54], bit);
        v[42] = half_btf_0_neon(&cospi43, &u[42], bit);
        v[43] = half_btf_0_neon(&cospim53, &u[52], bit);
        v[44] = half_btf_0_neon(&cospi51, &u[44], bit);
        v[45] = half_btf_0_neon(&cospim45, &u[50], bit);
        v[46] = half_btf_0_neon(&cospi35, &u[46], bit);
        v[47] = half_btf_0_neon(&cospim61, &u[48], bit);
        v[48] = half_btf_0_neon(&cospi3, &u[48], bit);
        v[49] = half_btf_0_neon(&cospi29, &u[46], bit);
        v[50] = half_btf_0_neon(&cospi19, &u[50], bit);
        v[51] = half_btf_0_neon(&cospi13, &u[44], bit);
        v[52] = half_btf_0_neon(&cospi11, &u[52], bit);
        v[53] = half_btf_0_neon(&cospi21, &u[42], bit);
        v[54] = half_btf_0_neon(&cospi27, &u[54], bit);
        v[55] = half_btf_0_neon(&cospi5, &u[40], bit);
        v[56] = half_btf_0_neon(&cospi7, &u[56], bit);
        v[57] = half_btf_0_neon(&cospi25, &u[38], bit);
        v[58] = half_btf_0_neon(&cospi23, &u[58], bit);
        v[59] = half_btf_0_neon(&cospi9, &u[36], bit);
        v[60] = half_btf_0_neon(&cospi15, &u[60], bit);
        v[61] = half_btf_0_neon(&cospi17, &u[34], bit);
        v[62] = half_btf_0_neon(&cospi31, &u[62], bit);
        v[63] = half_btf_0_neon(&cospi1, &u[32], bit);

        // stage 3
        u[16] = half_btf_0_neon(&cospi62, &v[16], bit);
        u[17] = half_btf_0_neon(&cospim34, &v[30], bit);
        u[18] = half_btf_0_neon(&cospi46, &v[18], bit);
        u[19] = half_btf_0_neon(&cospim50, &v[28], bit);
        u[20] = half_btf_0_neon(&cospi54, &v[20], bit);
        u[21] = half_btf_0_neon(&cospim42, &v[26], bit);
        u[22] = half_btf_0_neon(&cospi38, &v[22], bit);
        u[23] = half_btf_0_neon(&cospim58, &v[24], bit);
        u[24] = half_btf_0_neon(&cospi6, &v[24], bit);
        u[25] = half_btf_0_neon(&cospi26, &v[22], bit);
        u[26] = half_btf_0_neon(&cospi22, &v[26], bit);
        u[27] = half_btf_0_neon(&cospi10, &v[20], bit);
        u[28] = half_btf_0_neon(&cospi14, &v[28], bit);
        u[29] = half_btf_0_neon(&cospi18, &v[18], bit);
        u[30] = half_btf_0_neon(&cospi30, &v[30], bit);
        u[31] = half_btf_0_neon(&cospi2, &v[16], bit);

        for (int32_t i = 32; i < 64; i += 4) {
            addsub_neon(v[i + 0], v[i + 1], &u[i + 0], &u[i + 1], &clamp_lo, &clamp_hi);
            addsub_neon(v[i + 3], v[i + 2], &u[i + 3], &u[i + 2], &clamp_lo, &clamp_hi);
        }

        // stage 4
        v[8]  = half_btf_0_neon(&cospi60, &u[8], bit);
        v[9]  = half_btf_0_neon(&cospim36, &u[14], bit);
        v[10] = half_btf_0_neon(&cospi44, &u[10], bit);
        v[11] = half_btf_0_neon(&cospim52, &u[12], bit);
        v[12] = half_btf_0_neon(&cospi12, &u[12], bit);
        v[13] = half_btf_0_neon(&cospi20, &u[10], bit);
        v[14] = half_btf_0_neon(&cospi28, &u[14], bit);
        v[15] = half_btf_0_neon(&cospi4, &u[8], bit);

        for (int32_t i = 16; i < 32; i += 4) {
            addsub_neon(u[i + 0], u[i + 1], &v[i + 0], &v[i + 1], &clamp_lo, &clamp_hi);
            addsub_neon(u[i + 3], u[i + 2], &v[i + 3], &v[i + 2], &clamp_lo, &clamp_hi);
        }

        for (int32_t i = 32; i < 64; i += 4) {
            v[i + 0] = u[i + 0];
            v[i + 3] = u[i + 3];
        }

        v[33] = half_btf_neon(&cospim4, &u[33], &cospi60, &u[62], bit);
        v[34] = half_btf_neon(&cospim60, &u[34], &cospim4, &u[61], bit);
        v[37] = half_btf_neon(&cospim36, &u[37], &cospi28, &u[58], bit);
        v[38] = half_btf_neon(&cospim28, &u[38], &cospim36, &u[57], bit);
        v[41] = half_btf_neon(&cospim20, &u[41], &cospi44, &u[54], bit);
        v[42] = half_btf_neon(&cospim44, &u[42], &cospim20, &u[53], bit);
        v[45] = half_btf_neon(&cospim52, &u[45], &cospi12, &u[50], bit);
        v[46] = half_btf_neon(&cospim12, &u[46], &cospim52, &u[49], bit);
        v[49] = half_btf_neon(&cospim52, &u[46], &cospi12, &u[49], bit);
        v[50] = half_btf_neon(&cospi12, &u[45], &cospi52, &u[50], bit);
        v[53] = half_btf_neon(&cospim20, &u[42], &cospi44, &u[53], bit);
        v[54] = half_btf_neon(&cospi44, &u[41], &cospi20, &u[54], bit);
        v[57] = half_btf_neon(&cospim36, &u[38], &cospi28, &u[57], bit);
        v[58] = half_btf_neon(&cospi28, &u[37], &cospi36, &u[58], bit);
        v[61] = half_btf_neon(&cospim4, &u[34], &cospi60, &u[61], bit);
        v[62] = half_btf_neon(&cospi60, &u[33], &cospi4, &u[62], bit);

        // stage 5
        u[4] = half_btf_0_neon(&cospi56, &v[4], bit);
        u[5] = half_btf_0_neon(&cospim40, &v[6], bit);
        u[6] = half_btf_0_neon(&cospi24, &v[6], bit);
        u[7] = half_btf_0_neon(&cospi8, &v[4], bit);

        for (int32_t i = 8; i < 16; i += 4) {
            addsub_neon(v[i + 0], v[i + 1], &u[i + 0], &u[i + 1], &clamp_lo, &clamp_hi);
            addsub_neon(v[i + 3], v[i + 2], &u[i + 3], &u[i + 2], &clamp_lo, &clamp_hi);
        }

        for (int32_t i = 16; i < 32; i += 4) {
            u[i + 0] = v[i + 0];
            u[i + 3] = v[i + 3];
        }

        u[17] = half_btf_neon(&cospim8, &v[17], &cospi56, &v[30], bit);
        u[18] = half_btf_neon(&cospim56, &v[18], &cospim8, &v[29], bit);
        u[21] = half_btf_neon(&cospim40, &v[21], &cospi24, &v[26], bit);
        u[22] = half_btf_neon(&cospim24, &v[22], &cospim40, &v[25], bit);
        u[25] = half_btf_neon(&cospim40, &v[22], &cospi24, &v[25], bit);
        u[26] = half_btf_neon(&cospi24, &v[21], &cospi40, &v[26], bit);
        u[29] = half_btf_neon(&cospim8, &v[18], &cospi56, &v[29], bit);
        u[30] = half_btf_neon(&cospi56, &v[17], &cospi8, &v[30], bit);

        for (int32_t i = 32; i < 64; i += 8) {
            addsub_neon(v[i + 0], v[i + 3], &u[i + 0], &u[i + 3], &clamp_lo, &clamp_hi);
            addsub_neon(v[i + 1], v[i + 2], &u[i + 1], &u[i + 2], &clamp_lo, &clamp_hi);

            addsub_neon(v[i + 7], v[i + 4], &u[i + 7], &u[i + 4], &clamp_lo, &clamp_hi);
            addsub_neon(v[i + 6], v[i + 5], &u[i + 6], &u[i + 5], &clamp_lo, &clamp_hi);
        }

        // stage 6
        v[0] = half_btf_0_neon(&cospi32, &u[0], bit);
        v[1] = half_btf_0_neon(&cospi32, &u[0], bit);
        v[2] = half_btf_0_neon(&cospi48, &u[2], bit);
        v[3] = half_btf_0_neon(&cospi16, &u[2], bit);

        addsub_neon(u[4], u[5], &v[4], &v[5], &clamp_lo, &clamp_hi);
        addsub_neon(u[7], u[6], &v[7], &v[6], &clamp_lo, &clamp_hi);

        for (int32_t i = 8; i < 16; i += 4) {
            v[i + 0] = u[i + 0];
            v[i + 3] = u[i + 3];
        }

        v[9]  = half_btf_neon(&cospim16, &u[9], &cospi48, &u[14], bit);
        v[10] = half_btf_neon(&cospim48, &u[10], &cospim16, &u[13], bit);
        v[13] = half_btf_neon(&cospim16, &u[10], &cospi48, &u[13], bit);
        v[14] = half_btf_neon(&cospi48, &u[9], &cospi16, &u[14], bit);

        for (int32_t i = 16; i < 32; i += 8) {
            addsub_neon(u[i + 0], u[i + 3], &v[i + 0], &v[i + 3], &clamp_lo, &clamp_hi);
            addsub_neon(u[i + 1], u[i + 2], &v[i + 1], &v[i + 2], &clamp_lo, &clamp_hi);

            addsub_neon(u[i + 7], u[i + 4], &v[i + 7], &v[i + 4], &clamp_lo, &clamp_hi);
            addsub_neon(u[i + 6], u[i + 5], &v[i + 6], &v[i + 5], &clamp_lo, &clamp_hi);
        }

        for (int32_t i = 32; i < 64; i += 8) {
            v[i + 0] = u[i + 0];
            v[i + 1] = u[i + 1];
            v[i + 6] = u[i + 6];
            v[i + 7] = u[i + 7];
        }

        v[34] = half_btf_neon(&cospim8, &u[34], &cospi56, &u[61], bit);
        v[35] = half_btf_neon(&cospim8, &u[35], &cospi56, &u[60], bit);
        v[36] = half_btf_neon(&cospim56, &u[36], &cospim8, &u[59], bit);
        v[37] = half_btf_neon(&cospim56, &u[37], &cospim8, &u[58], bit);
        v[42] = half_btf_neon(&cospim40, &u[42], &cospi24, &u[53], bit);
        v[43] = half_btf_neon(&cospim40, &u[43], &cospi24, &u[52], bit);
        v[44] = half_btf_neon(&cospim24, &u[44], &cospim40, &u[51], bit);
        v[45] = half_btf_neon(&cospim24, &u[45], &cospim40, &u[50], bit);
        v[50] = half_btf_neon(&cospim40, &u[45], &cospi24, &u[50], bit);
        v[51] = half_btf_neon(&cospim40, &u[44], &cospi24, &u[51], bit);
        v[52] = half_btf_neon(&cospi24, &u[43], &cospi40, &u[52], bit);
        v[53] = half_btf_neon(&cospi24, &u[42], &cospi40, &u[53], bit);
        v[58] = half_btf_neon(&cospim8, &u[37], &cospi56, &u[58], bit);
        v[59] = half_btf_neon(&cospim8, &u[36], &cospi56, &u[59], bit);
        v[60] = half_btf_neon(&cospi56, &u[35], &cospi8, &u[60], bit);
        v[61] = half_btf_neon(&cospi56, &u[34], &cospi8, &u[61], bit);

        // stage 7
        addsub_neon(v[0], v[3], &u[0], &u[3], &clamp_lo, &clamp_hi);
        addsub_neon(v[1], v[2], &u[1], &u[2], &clamp_lo, &clamp_hi);

        u[4] = v[4];
        u[7] = v[7];
        u[5] = half_btf_neon(&cospim32, &v[5], &cospi32, &v[6], bit);
        u[6] = half_btf_neon(&cospi32, &v[5], &cospi32, &v[6], bit);

        addsub_neon(v[8], v[11], &u[8], &u[11], &clamp_lo, &clamp_hi);
        addsub_neon(v[9], v[10], &u[9], &u[10], &clamp_lo, &clamp_hi);
        addsub_neon(v[15], v[12], &u[15], &u[12], &clamp_lo, &clamp_hi);
        addsub_neon(v[14], v[13], &u[14], &u[13], &clamp_lo, &clamp_hi);

        for (int32_t i = 16; i < 32; i += 8) {
            u[i + 0] = v[i + 0];
            u[i + 1] = v[i + 1];
            u[i + 6] = v[i + 6];
            u[i + 7] = v[i + 7];
        }

        u[18] = half_btf_neon(&cospim16, &v[18], &cospi48, &v[29], bit);
        u[19] = half_btf_neon(&cospim16, &v[19], &cospi48, &v[28], bit);
        u[20] = half_btf_neon(&cospim48, &v[20], &cospim16, &v[27], bit);
        u[21] = half_btf_neon(&cospim48, &v[21], &cospim16, &v[26], bit);
        u[26] = half_btf_neon(&cospim16, &v[21], &cospi48, &v[26], bit);
        u[27] = half_btf_neon(&cospim16, &v[20], &cospi48, &v[27], bit);
        u[28] = half_btf_neon(&cospi48, &v[19], &cospi16, &v[28], bit);
        u[29] = half_btf_neon(&cospi48, &v[18], &cospi16, &v[29], bit);

        for (int32_t i = 32; i < 64; i += 16) {
            for (int32_t j = i; j < i + 4; j++) {
                addsub_neon(v[j], v[j ^ 7], &u[j], &u[j ^ 7], &clamp_lo, &clamp_hi);
                addsub_neon(v[j ^ 15], v[j ^ 8], &u[j ^ 15], &u[j ^ 8], &clamp_lo, &clamp_hi);
            }
        }

        // stage 8
        for (int32_t i = 0; i < 4; ++i) { addsub_neon(u[i], u[7 - i], &v[i], &v[7 - i], &clamp_lo, &clamp_hi); }
        v[8]  = u[8];
        v[9]  = u[9];
        v[14] = u[14];
        v[15] = u[15];

        v[10] = half_btf_neon(&cospim32, &u[10], &cospi32, &u[13], bit);
        v[11] = half_btf_neon(&cospim32, &u[11], &cospi32, &u[12], bit);
        v[12] = half_btf_neon(&cospi32, &u[11], &cospi32, &u[12], bit);
        v[13] = half_btf_neon(&cospi32, &u[10], &cospi32, &u[13], bit);

        for (int32_t i = 16; i < 20; ++i) {
            addsub_neon(u[i], u[i ^ 7], &v[i], &v[i ^ 7], &clamp_lo, &clamp_hi);
            addsub_neon(u[i ^ 15], u[i ^ 8], &v[i ^ 15], &v[i ^ 8], &clamp_lo, &clamp_hi);
        }

        for (int32_t i = 32; i < 36; ++i) {
            v[i]      = u[i];
            v[i + 12] = u[i + 12];
            v[i + 16] = u[i + 16];
            v[i + 28] = u[i + 28];
        }

        v[36] = half_btf_neon(&cospim16, &u[36], &cospi48, &u[59], bit);
        v[37] = half_btf_neon(&cospim16, &u[37], &cospi48, &u[58], bit);
        v[38] = half_btf_neon(&cospim16, &u[38], &cospi48, &u[57], bit);
        v[39] = half_btf_neon(&cospim16, &u[39], &cospi48, &u[56], bit);
        v[40] = half_btf_neon(&cospim48, &u[40], &cospim16, &u[55], bit);
        v[41] = half_btf_neon(&cospim48, &u[41], &cospim16, &u[54], bit);
        v[42] = half_btf_neon(&cospim48, &u[42], &cospim16, &u[53], bit);
        v[43] = half_btf_neon(&cospim48, &u[43], &cospim16, &u[52], bit);
        v[52] = half_btf_neon(&cospim16, &u[43], &cospi48, &u[52], bit);
        v[53] = half_btf_neon(&cospim16, &u[42], &cospi48, &u[53], bit);
        v[54] = half_btf_neon(&cospim16, &u[41], &cospi48, &u[54], bit);
        v[55] = half_btf_neon(&cospim16, &u[40], &cospi48, &u[55], bit);
        v[56] = half_btf_neon(&cospi48, &u[39], &cospi16, &u[56], bit);
        v[57] = half_btf_neon(&cospi48, &u[38], &cospi16, &u[57], bit);
        v[58] = half_btf_neon(&cospi48, &u[37], &cospi16, &u[58], bit);
        v[59] = half_btf_neon(&cospi48, &u[36], &cospi16, &u[59], bit);

        // stage 9
        for (int32_t i = 0; i < 8; ++i) { addsub_neon(v[i], v[15 - i], &u[i], &u[15 - i], &clamp_lo, &clamp_hi); }
        for (int32_t i = 16; i < 20; ++i) {
            u[i]      = v[i];
            u[i + 12] = v[i + 12];
        }

        u[20] = half_btf_neon(&cospim32, &v[20], &cospi32, &v[27], bit);
        u[21] = half_btf_neon(&cospim32, &v[21], &cospi32, &v[26], bit);
        u[22] = half_btf_neon(&cospim32, &v[22], &cospi32, &v[25], bit);
        u[23] = half_btf_neon(&cospim32, &v[23], &cospi32, &v[24], bit);
        u[24] = half_btf_neon(&cospi32, &v[23], &cospi32, &v[24], bit);
        u[25] = half_btf_neon(&cospi32, &v[22], &cospi32, &v[25], bit);
        u[26] = half_btf_neon(&cospi32, &v[21], &cospi32, &v[26], bit);
        u[27] = half_btf_neon(&cospi32, &v[20], &cospi32, &v[27], bit);

        for (int32_t i = 32; i < 40; i++) { addsub_neon(v[i], v[i ^ 15], &u[i], &u[i ^ 15], &clamp_lo, &clamp_hi); }
        for (int32_t i = 48; i < 56; i++) { addsub_neon(v[i ^ 15], v[i], &u[i ^ 15], &u[i], &clamp_lo, &clamp_hi); }
        // stage 10
        for (int32_t i = 0; i < 16; i++) { addsub_neon(u[i], u[31 - i], &v[i], &v[31 - i], &clamp_lo, &clamp_hi); }
        for (int32_t i = 32; i < 40; i++) { v[i] = u[i]; }

        v[40] = half_btf_neon(&cospim32, &u[40], &cospi32, &u[55], bit);
        v[41] = half_btf_neon(&cospim32, &u[41], &cospi32, &u[54], bit);
        v[42] = half_btf_neon(&cospim32, &u[42], &cospi32, &u[53], bit);
        v[43] = half_btf_neon(&cospim32, &u[43], &cospi32, &u[52], bit);
        v[44] = half_btf_neon(&cospim32, &u[44], &cospi32, &u[51], bit);
        v[45] = half_btf_neon(&cospim32, &u[45], &cospi32, &u[50], bit);
        v[46] = half_btf_neon(&cospim32, &u[46], &cospi32, &u[49], bit);
        v[47] = half_btf_neon(&cospim32, &u[47], &cospi32, &u[48], bit);
        v[48] = half_btf_neon(&cospi32, &u[47], &cospi32, &u[48], bit);
        v[49] = half_btf_neon(&cospi32, &u[46], &cospi32, &u[49], bit);
        v[50] = half_btf_neon(&cospi32, &u[45], &cospi32, &u[50], bit);
        v[51] = half_btf_neon(&cospi32, &u[44], &cospi32, &u[51], bit);
        v[52] = half_btf_neon(&cospi32, &u[43], &cospi32, &u[52], bit);
        v[53] = half_btf_neon(&cospi32, &u[42], &cospi32, &u[53], bit);
        v[54] = half_btf_neon(&cospi32, &u[41], &cospi32, &u[54], bit);
        v[55] = half_btf_neon(&cospi32, &u[40], &cospi32, &u[55], bit);

        for (int32_t i = 56; i < 64; i++) { v[i] = u[i]; }

        // stage 11
        for (int32_t i = 0; i < 32; i++) {
            addsub_neon(v[i], v[63 - i], &out[16 * (i) + col], &out[16 * (63 - i) + col], &clamp_lo, &clamp_hi);
        }
    }
}

static INLINE void round_shift_64x64(int32x4_t in[], int32_t shift) {
    round_shift_32x32(&in[0], shift);
    round_shift_32x32(&in[256], shift);
}

static INLINE void assign_32x32_input_from_64x64(const int32x4_t in[], int32x4_t in32x32[], int32_t col) {
    for (int32_t i = 0; i < 32 * 32 / 4; i += 8) {
        in32x32[i]     = in[col];
        in32x32[i + 1] = in[col + 1];
        in32x32[i + 2] = in[col + 2];
        in32x32[i + 3] = in[col + 3];
        in32x32[i + 4] = in[col + 4];
        in32x32[i + 5] = in[col + 5];
        in32x32[i + 6] = in[col + 6];
        in32x32[i + 7] = in[col + 7];
        col += 16;
    }
}

static void write_buffer_64x64(int32x4_t in[], uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                               int32_t stride_w, int32_t fliplr, int32_t flipud, int32_t shift, int32_t bd) {
    int32x4_t in32x32[32 * 32 / 4];
    uint16_t *left_up_r    = &output_r[0];
    uint16_t *right_up_r   = &output_r[32];
    uint16_t *left_down_r  = &output_r[32 * stride_r];
    uint16_t *right_down_r = &output_r[32 * stride_r + 32];
    uint16_t *left_up_w    = &output_w[0];
    uint16_t *right_up_w   = &output_w[32];
    uint16_t *left_down_w  = &output_w[32 * stride_w];
    uint16_t *right_down_w = &output_w[32 * stride_w + 32];

    if (fliplr) {
        swap_addr(&left_up_r, &right_up_r);
        swap_addr(&left_down_r, &right_down_r);
        swap_addr(&left_up_w, &right_up_w);
        swap_addr(&left_down_w, &right_down_w);
    }

    if (flipud) {
        swap_addr(&left_up_r, &left_down_r);
        swap_addr(&right_up_r, &right_down_r);
        swap_addr(&left_up_w, &left_down_w);
        swap_addr(&right_up_w, &right_down_w);
    }

    // Left-up quarter
    assign_32x32_input_from_64x64(in, in32x32, 0);
    write_buffer_32x32(in32x32, left_up_r, stride_r, left_up_w, stride_w, fliplr, flipud, shift, bd);

    // Right-up quarter
    assign_32x32_input_from_64x64(in, in32x32, 64 / 2 / 4);
    write_buffer_32x32(in32x32, right_up_r, stride_r, right_up_w, stride_w, fliplr, flipud, shift, bd);

    // Left-down quarter
    assign_32x32_input_from_64x64(in, in32x32, 64 * 64 / 2 / 4);
    write_buffer_32x32(in32x32, left_down_r, stride_r, left_down_w, stride_w, fliplr, flipud, shift, bd);

    // Right-down quarter
    assign_32x32_input_from_64x64(in, in32x32, 64 * 64 / 2 / 4 + 64 / 2 / 4);
    write_buffer_32x32(in32x32, right_down_r, stride_r, right_down_w, stride_w, fliplr, flipud, shift, bd);
}

void svt_av1_inv_txfm2d_add_64x64_neon(const int32_t *input, uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                       int32_t stride_w, TxType tx_type, int32_t bd) {
    int32x4_t     in[64 * 64 / 4], out[64 * 64 / 4];
    const int8_t *shift   = svt_aom_inv_txfm_shift_ls[TX_64X64];
    const int32_t txw_idx = tx_size_wide_log2[TX_64X64] - tx_size_wide_log2[0];
    const int32_t txh_idx = tx_size_high_log2[TX_64X64] - tx_size_high_log2[0];

    switch (tx_type) {
    case DCT_DCT:
        load_buffer_64x64_lower_32x32(input, in);
        transpose_64x64(in, out, 0);
        idct64x64_neon(out, in, inv_cos_bit_row[txw_idx][txh_idx], 0, bd);
        transpose_64x64(in, out, 1);
        round_shift_64x64(out, -shift[0]);
        idct64x64_neon(out, in, inv_cos_bit_col[txw_idx][txh_idx], 1, bd);
        write_buffer_64x64(in, output_r, stride_r, output_w, stride_w, 0, 0, -shift[1], bd);
        break;

    default: svt_av1_inv_txfm2d_add_64x64_c(input, output_r, stride_r, output_w, stride_w, tx_type, bd); break;
    }
}

static void idct16_x4_neon(int32x4_t *in, int32x4_t *out, int32_t bit) {
    const int32_t  *cospi    = cospi_arr(bit);
    const int32x4_t cospi60  = vdupq_n_s32(cospi[60]);
    const int32x4_t cospim4  = vdupq_n_s32(-cospi[4]);
    const int32x4_t cospi28  = vdupq_n_s32(cospi[28]);
    const int32x4_t cospim36 = vdupq_n_s32(-cospi[36]);
    const int32x4_t cospi44  = vdupq_n_s32(cospi[44]);
    const int32x4_t cospi20  = vdupq_n_s32(cospi[20]);
    const int32x4_t cospim20 = vdupq_n_s32(-cospi[20]);
    const int32x4_t cospi12  = vdupq_n_s32(cospi[12]);
    const int32x4_t cospim52 = vdupq_n_s32(-cospi[52]);
    const int32x4_t cospi52  = vdupq_n_s32(cospi[52]);
    const int32x4_t cospi36  = vdupq_n_s32(cospi[36]);
    const int32x4_t cospi4   = vdupq_n_s32(cospi[4]);
    const int32x4_t cospi56  = vdupq_n_s32(cospi[56]);
    const int32x4_t cospim8  = vdupq_n_s32(-cospi[8]);
    const int32x4_t cospi24  = vdupq_n_s32(cospi[24]);
    const int32x4_t cospim40 = vdupq_n_s32(-cospi[40]);
    const int32x4_t cospi40  = vdupq_n_s32(cospi[40]);
    const int32x4_t cospi8   = vdupq_n_s32(cospi[8]);
    const int32x4_t cospi32  = vdupq_n_s32(cospi[32]);
    const int32x4_t cospi48  = vdupq_n_s32(cospi[48]);
    const int32x4_t cospi16  = vdupq_n_s32(cospi[16]);
    const int32x4_t cospim16 = vdupq_n_s32(-cospi[16]);
    const int32x4_t cospim48 = vdupq_n_s32(-cospi[48]);
    const int32x4_t vbit     = vdupq_n_s32(bit);

    int32x4_t u[16], v[16], x, y;

    u[0]  = in[0];
    u[1]  = in[8];
    u[2]  = in[4];
    u[3]  = in[12];
    u[4]  = in[2];
    u[5]  = in[10];
    u[6]  = in[6];
    u[7]  = in[14];
    u[8]  = in[1];
    u[9]  = in[9];
    u[10] = in[5];
    u[11] = in[13];
    u[12] = in[3];
    u[13] = in[11];
    u[14] = in[7];
    u[15] = in[15];

    // stage 2
    v[0] = u[0];
    v[1] = u[1];
    v[2] = u[2];
    v[3] = u[3];
    v[4] = u[4];
    v[5] = u[5];
    v[6] = u[6];
    v[7] = u[7];

    v[8]  = half_btf_neon(&cospi60, &u[8], &cospim4, &u[15], bit);
    v[9]  = half_btf_neon(&cospi28, &u[9], &cospim36, &u[14], bit);
    v[10] = half_btf_neon(&cospi44, &u[10], &cospim20, &u[13], bit);
    v[11] = half_btf_neon(&cospi12, &u[11], &cospim52, &u[12], bit);
    v[12] = half_btf_neon(&cospi52, &u[11], &cospi12, &u[12], bit);
    v[13] = half_btf_neon(&cospi20, &u[10], &cospi44, &u[13], bit);
    v[14] = half_btf_neon(&cospi36, &u[9], &cospi28, &u[14], bit);
    v[15] = half_btf_neon(&cospi4, &u[8], &cospi60, &u[15], bit);

    // stage 3
    u[0]  = v[0];
    u[1]  = v[1];
    u[2]  = v[2];
    u[3]  = v[3];
    u[4]  = half_btf_neon(&cospi56, &v[4], &cospim8, &v[7], bit);
    u[5]  = half_btf_neon(&cospi24, &v[5], &cospim40, &v[6], bit);
    u[6]  = half_btf_neon(&cospi40, &v[5], &cospi24, &v[6], bit);
    u[7]  = half_btf_neon(&cospi8, &v[4], &cospi56, &v[7], bit);
    u[8]  = vaddq_s32(v[8], v[9]);
    u[9]  = vsubq_s32(v[8], v[9]);
    u[10] = vsubq_s32(v[11], v[10]);
    u[11] = vaddq_s32(v[10], v[11]);
    u[12] = vaddq_s32(v[12], v[13]);
    u[13] = vsubq_s32(v[12], v[13]);
    u[14] = vsubq_s32(v[15], v[14]);
    u[15] = vaddq_s32(v[14], v[15]);

    // stage 4
    x    = vmulq_s32(u[0], cospi32);
    y    = vmulq_s32(u[1], cospi32);
    v[0] = vaddq_s32(x, y);
    v[0] = vrshlq_s32(v[0], -vbit);

    v[1] = vsubq_s32(x, y);
    v[1] = vrshlq_s32(v[1], -vbit);

    v[2]  = half_btf_neon(&cospi48, &u[2], &cospim16, &u[3], bit);
    v[3]  = half_btf_neon(&cospi16, &u[2], &cospi48, &u[3], bit);
    v[4]  = vaddq_s32(u[4], u[5]);
    v[5]  = vsubq_s32(u[4], u[5]);
    v[6]  = vsubq_s32(u[7], u[6]);
    v[7]  = vaddq_s32(u[6], u[7]);
    v[8]  = u[8];
    v[9]  = half_btf_neon(&cospim16, &u[9], &cospi48, &u[14], bit);
    v[10] = half_btf_neon(&cospim48, &u[10], &cospim16, &u[13], bit);
    v[11] = u[11];
    v[12] = u[12];
    v[13] = half_btf_neon(&cospim16, &u[10], &cospi48, &u[13], bit);
    v[14] = half_btf_neon(&cospi48, &u[9], &cospi16, &u[14], bit);
    v[15] = u[15];

    // stage 5
    u[0] = vaddq_s32(v[0], v[3]);
    u[1] = vaddq_s32(v[1], v[2]);
    u[2] = vsubq_s32(v[1], v[2]);
    u[3] = vsubq_s32(v[0], v[3]);
    u[4] = v[4];

    x    = vmulq_s32(v[5], cospi32);
    y    = vmulq_s32(v[6], cospi32);
    u[5] = vsubq_s32(y, x);
    u[5] = vrshlq_s32(u[5], -vbit);

    u[6] = vaddq_s32(y, x);
    u[6] = vrshlq_s32(u[6], -vbit);

    u[7]  = v[7];
    u[8]  = vaddq_s32(v[8], v[11]);
    u[9]  = vaddq_s32(v[9], v[10]);
    u[10] = vsubq_s32(v[9], v[10]);
    u[11] = vsubq_s32(v[8], v[11]);
    u[12] = vsubq_s32(v[15], v[12]);
    u[13] = vsubq_s32(v[14], v[13]);
    u[14] = vaddq_s32(v[13], v[14]);
    u[15] = vaddq_s32(v[12], v[15]);

    // stage 6
    v[0] = vaddq_s32(u[0], u[7]);
    v[1] = vaddq_s32(u[1], u[6]);
    v[2] = vaddq_s32(u[2], u[5]);
    v[3] = vaddq_s32(u[3], u[4]);
    v[4] = vsubq_s32(u[3], u[4]);
    v[5] = vsubq_s32(u[2], u[5]);
    v[6] = vsubq_s32(u[1], u[6]);
    v[7] = vsubq_s32(u[0], u[7]);
    v[8] = u[8];
    v[9] = u[9];

    x     = vmulq_s32(u[10], cospi32);
    y     = vmulq_s32(u[13], cospi32);
    v[10] = vsubq_s32(y, x);
    v[10] = vrshlq_s32(v[10], -vbit);

    v[13] = vaddq_s32(x, y);
    v[13] = vrshlq_s32(v[13], -vbit);

    x     = vmulq_s32(u[11], cospi32);
    y     = vmulq_s32(u[12], cospi32);
    v[11] = vsubq_s32(y, x);
    v[11] = vrshlq_s32(v[11], -vbit);

    v[12] = vaddq_s32(x, y);
    v[12] = vrshlq_s32(v[12], -vbit);

    v[14] = u[14];
    v[15] = u[15];

    // stage 7
    out[0]  = vaddq_s32(v[0], v[15]);
    out[1]  = vaddq_s32(v[1], v[14]);
    out[2]  = vaddq_s32(v[2], v[13]);
    out[3]  = vaddq_s32(v[3], v[12]);
    out[4]  = vaddq_s32(v[4], v[11]);
    out[5]  = vaddq_s32(v[5], v[10]);
    out[6]  = vaddq_s32(v[6], v[9]);
    out[7]  = vaddq_s32(v[7], v[8]);
    out[8]  = vsubq_s32(v[7], v[8]);
    out[9]  = vsubq_s32(v[6], v[9]);
    out[10] = vsubq_s32(v[5], v[10]);
    out[11] = vsubq_s32(v[4], v[11]);
    out[12] = vsubq_s32(v[3], v[12]);
    out[13] = vsubq_s32(v[2], v[13]);
    out[14] = vsubq_s32(v[1], v[14]);
    out[15] = vsubq_s32(v[0], v[15]);
}

static INLINE void idct16_xn_neon(int32x4_t *in, int32x4_t *out, int bit, int howmany) {
    const int stride = 16;
    int       i      = 0;
    do { idct16_x4_neon(in + i * stride, out + i * stride, bit); } while (++i < howmany);
}

static void iadst16_x4_neon(int32x4_t *in, int32x4_t *out, int32_t bit) {
    const int32_t  *cospi = cospi_arr(bit);
    const int32x4_t vbit  = vdupq_n_s32(bit);

    int32x4_t u[16], v[16], x, y;

    v[0] = vmulq_n_s32(in[15], cospi[2]);
    v[0] = vmlaq_n_s32(v[0], in[0], cospi[62]);
    v[0] = vrshlq_s32(v[0], -vbit);

    v[1] = vmulq_n_s32(in[15], cospi[62]);
    v[1] = vmlsq_n_s32(v[1], in[0], cospi[2]);
    v[1] = vrshlq_s32(v[1], -vbit);

    v[2] = vmulq_n_s32(in[13], cospi[10]);
    v[2] = vmlaq_n_s32(v[2], in[2], cospi[54]);
    v[2] = vrshlq_s32(v[2], -vbit);

    v[3] = vmulq_n_s32(in[13], cospi[54]);
    v[3] = vmlsq_n_s32(v[3], in[2], cospi[10]);
    v[3] = vrshlq_s32(v[3], -vbit);

    v[4] = vmulq_n_s32(in[11], cospi[18]);
    v[4] = vmlaq_n_s32(v[4], in[4], cospi[46]);
    v[4] = vrshlq_s32(v[4], -vbit);

    v[5] = vmulq_n_s32(in[11], cospi[46]);
    v[5] = vmlsq_n_s32(v[5], in[4], cospi[18]);
    v[5] = vrshlq_s32(v[5], -vbit);

    v[6] = vmulq_n_s32(in[9], cospi[26]);
    v[6] = vmlaq_n_s32(v[6], in[6], cospi[38]);
    v[6] = vrshlq_s32(v[6], -vbit);

    v[7] = vmulq_n_s32(in[9], cospi[38]);
    v[7] = vmlsq_n_s32(v[7], in[6], cospi[26]);
    v[7] = vrshlq_s32(v[7], -vbit);

    v[8] = vmulq_n_s32(in[7], cospi[34]);
    v[8] = vmlaq_n_s32(v[8], in[8], cospi[30]);
    v[8] = vrshlq_s32(v[8], -vbit);

    v[9] = vmulq_n_s32(in[7], cospi[30]);
    v[9] = vmlsq_n_s32(v[9], in[8], cospi[34]);
    v[9] = vrshlq_s32(v[9], -vbit);

    v[10] = vmulq_n_s32(in[5], cospi[42]);
    v[10] = vmlaq_n_s32(v[10], in[10], cospi[22]);
    v[10] = vrshlq_s32(v[10], -vbit);

    v[11] = vmulq_n_s32(in[5], cospi[22]);
    v[11] = vmlsq_n_s32(v[11], in[10], cospi[42]);
    v[11] = vrshlq_s32(v[11], -vbit);

    v[12] = vmulq_n_s32(in[3], cospi[50]);
    v[12] = vmlaq_n_s32(v[12], in[12], cospi[14]);
    v[12] = vrshlq_s32(v[12], -vbit);

    v[13] = vmulq_n_s32(in[3], cospi[14]);
    v[13] = vmlsq_n_s32(v[13], in[12], cospi[50]);
    v[13] = vrshlq_s32(v[13], -vbit);

    v[14] = vmulq_n_s32(in[1], cospi[58]);
    v[14] = vmlaq_n_s32(v[14], in[14], cospi[6]);
    v[14] = vrshlq_s32(v[14], -vbit);

    v[15] = vmulq_n_s32(in[1], cospi[6]);
    v[15] = vmlsq_n_s32(v[15], in[14], cospi[58]);
    v[15] = vrshlq_s32(v[15], -vbit);

    // stage 3
    u[0]  = vaddq_s32(v[0], v[8]);
    u[8]  = vsubq_s32(v[0], v[8]);
    u[1]  = vaddq_s32(v[1], v[9]);
    u[9]  = vsubq_s32(v[1], v[9]);
    u[2]  = vaddq_s32(v[2], v[10]);
    u[10] = vsubq_s32(v[2], v[10]);
    u[3]  = vaddq_s32(v[3], v[11]);
    u[11] = vsubq_s32(v[3], v[11]);
    u[4]  = vaddq_s32(v[4], v[12]);
    u[12] = vsubq_s32(v[4], v[12]);
    u[5]  = vaddq_s32(v[5], v[13]);
    u[13] = vsubq_s32(v[5], v[13]);
    u[6]  = vaddq_s32(v[6], v[14]);
    u[14] = vsubq_s32(v[6], v[14]);
    u[7]  = vaddq_s32(v[7], v[15]);
    u[15] = vsubq_s32(v[7], v[15]);

    // stage 4
    v[0] = u[0];
    v[1] = u[1];
    v[2] = u[2];
    v[3] = u[3];
    v[4] = u[4];
    v[5] = u[5];
    v[6] = u[6];
    v[7] = u[7];

    v[8] = vmulq_n_s32(u[8], cospi[8]);
    v[8] = vmlaq_n_s32(v[8], u[9], cospi[56]);
    v[8] = vrshlq_s32(v[8], -vbit);

    v[9] = vmulq_n_s32(u[8], cospi[56]);
    v[9] = vmlsq_n_s32(v[9], u[9], cospi[8]);
    v[9] = vrshlq_s32(v[9], -vbit);

    v[10] = vmulq_n_s32(u[10], cospi[40]);
    v[10] = vmlaq_n_s32(v[10], u[11], cospi[24]);
    v[10] = vrshlq_s32(v[10], -vbit);

    v[11] = vmulq_n_s32(u[10], cospi[24]);
    v[11] = vmlsq_n_s32(v[11], u[11], cospi[40]);
    v[11] = vrshlq_s32(v[11], -vbit);

    v[12] = vmulq_n_s32(u[12], -cospi[56]);
    v[12] = vmlaq_n_s32(v[12], u[13], cospi[8]);
    v[12] = vrshlq_s32(v[12], -vbit);

    v[13] = vmulq_n_s32(u[12], cospi[8]);
    v[13] = vmlsq_n_s32(v[13], u[13], -cospi[56]);
    v[13] = vrshlq_s32(v[13], -vbit);

    v[14] = vmulq_n_s32(u[14], -cospi[24]);
    v[14] = vmlaq_n_s32(v[14], u[15], cospi[40]);
    v[14] = vrshlq_s32(v[14], -vbit);

    v[15] = vmulq_n_s32(u[14], cospi[40]);
    v[15] = vmlsq_n_s32(v[15], u[15], -cospi[24]);
    v[15] = vrshlq_s32(v[15], -vbit);

    // stage 5
    u[0]  = vaddq_s32(v[0], v[4]);
    u[4]  = vsubq_s32(v[0], v[4]);
    u[1]  = vaddq_s32(v[1], v[5]);
    u[5]  = vsubq_s32(v[1], v[5]);
    u[2]  = vaddq_s32(v[2], v[6]);
    u[6]  = vsubq_s32(v[2], v[6]);
    u[3]  = vaddq_s32(v[3], v[7]);
    u[7]  = vsubq_s32(v[3], v[7]);
    u[8]  = vaddq_s32(v[8], v[12]);
    u[12] = vsubq_s32(v[8], v[12]);
    u[9]  = vaddq_s32(v[9], v[13]);
    u[13] = vsubq_s32(v[9], v[13]);
    u[10] = vaddq_s32(v[10], v[14]);
    u[14] = vsubq_s32(v[10], v[14]);
    u[11] = vaddq_s32(v[11], v[15]);
    u[15] = vsubq_s32(v[11], v[15]);

    // stage 6
    v[0] = u[0];
    v[1] = u[1];
    v[2] = u[2];
    v[3] = u[3];

    v[4] = vmulq_n_s32(u[4], cospi[16]);
    v[4] = vmlaq_n_s32(v[4], u[5], cospi[48]);
    v[4] = vrshlq_s32(v[4], -vbit);

    v[5] = vmulq_n_s32(u[4], cospi[48]);
    v[5] = vmlsq_n_s32(v[5], u[5], cospi[16]);
    v[5] = vrshlq_s32(v[5], -vbit);

    v[6] = vmulq_n_s32(u[6], -cospi[48]);
    v[6] = vmlaq_n_s32(v[6], u[7], cospi[16]);
    v[6] = vrshlq_s32(v[6], -vbit);

    v[7] = vmulq_n_s32(u[6], cospi[16]);
    v[7] = vmlsq_n_s32(v[7], u[7], -cospi[48]);
    v[7] = vrshlq_s32(v[7], -vbit);

    v[8]  = u[8];
    v[9]  = u[9];
    v[10] = u[10];
    v[11] = u[11];

    v[12] = vmulq_n_s32(u[12], cospi[16]);
    v[12] = vmlaq_n_s32(v[12], u[13], cospi[48]);
    v[12] = vrshlq_s32(v[12], -vbit);

    v[13] = vmulq_n_s32(u[12], cospi[48]);
    v[13] = vmlsq_n_s32(v[13], u[13], cospi[16]);
    v[13] = vrshlq_s32(v[13], -vbit);

    v[14] = vmulq_n_s32(u[14], -cospi[48]);
    v[14] = vmlaq_n_s32(v[14], u[15], cospi[16]);
    v[14] = vrshlq_s32(v[14], -vbit);

    v[15] = vmulq_n_s32(u[14], cospi[16]);
    v[15] = vmlsq_n_s32(v[15], u[15], -cospi[48]);
    v[15] = vrshlq_s32(v[15], -vbit);

    // stage 7
    u[0]  = vaddq_s32(v[0], v[2]);
    u[2]  = vsubq_s32(v[0], v[2]);
    u[1]  = vaddq_s32(v[1], v[3]);
    u[3]  = vsubq_s32(v[1], v[3]);
    u[4]  = vaddq_s32(v[4], v[6]);
    u[6]  = vsubq_s32(v[4], v[6]);
    u[5]  = vaddq_s32(v[5], v[7]);
    u[7]  = vsubq_s32(v[5], v[7]);
    u[8]  = vaddq_s32(v[8], v[10]);
    u[10] = vsubq_s32(v[8], v[10]);
    u[9]  = vaddq_s32(v[9], v[11]);
    u[11] = vsubq_s32(v[9], v[11]);
    u[12] = vaddq_s32(v[12], v[14]);
    u[14] = vsubq_s32(v[12], v[14]);
    u[13] = vaddq_s32(v[13], v[15]);
    u[15] = vsubq_s32(v[13], v[15]);

    // stage 8
    v[0] = u[0];
    v[1] = u[1];

    y    = vmulq_n_s32(u[2], cospi[32]);
    x    = vmulq_n_s32(u[3], cospi[32]);
    v[2] = vaddq_s32(y, x);
    v[2] = vrshlq_s32(v[2], -vbit);

    v[3] = vsubq_s32(y, x);
    v[3] = vrshlq_s32(v[3], -vbit);

    v[4] = u[4];
    v[5] = u[5];

    y    = vmulq_n_s32(u[6], cospi[32]);
    x    = vmulq_n_s32(u[7], cospi[32]);
    v[6] = vaddq_s32(y, x);
    v[6] = vrshlq_s32(v[6], -vbit);

    v[7] = vsubq_s32(y, x);
    v[7] = vrshlq_s32(v[7], -vbit);

    v[8] = u[8];
    v[9] = u[9];

    y     = vmulq_n_s32(u[10], cospi[32]);
    x     = vmulq_n_s32(u[11], cospi[32]);
    v[10] = vaddq_s32(y, x);
    v[10] = vrshlq_s32(v[10], -vbit);

    v[11] = vsubq_s32(y, x);
    v[11] = vrshlq_s32(v[11], -vbit);

    v[12] = u[12];
    v[13] = u[13];

    y     = vmulq_n_s32(u[14], cospi[32]);
    x     = vmulq_n_s32(u[15], cospi[32]);
    v[14] = vaddq_s32(y, x);
    v[14] = vrshlq_s32(v[14], -vbit);

    v[15] = vsubq_s32(y, x);
    v[15] = vrshlq_s32(v[15], -vbit);

    // stage 9
    out[0]  = v[0];
    out[1]  = vnegq_s32(v[8]);
    out[2]  = v[12];
    out[3]  = vnegq_s32(v[4]);
    out[4]  = v[6];
    out[5]  = vnegq_s32(v[14]);
    out[6]  = v[10];
    out[7]  = vnegq_s32(v[2]);
    out[8]  = v[3];
    out[9]  = vnegq_s32(v[11]);
    out[10] = v[15];
    out[11] = vnegq_s32(v[7]);
    out[12] = v[5];
    out[13] = vnegq_s32(v[13]);
    out[14] = v[9];
    out[15] = vnegq_s32(v[1]);
}

static INLINE void iadst16_xn_neon(int32x4_t *in, int32x4_t *out, int bit, int howmany) {
    const int stride = 16;
    int       i      = 0;
    do { iadst16_x4_neon(in + i * stride, out + i * stride, bit); } while (++i < howmany);
}

static INLINE void iidentity16_x4_neon(int32x4_t *in, int32x4_t *out, int bit) {
    (void)bit;
    const int32x4_t scalar = vdupq_n_s32(2 * new_sqrt2);

    for (int32_t i = 0; i < 16; i++) {
        out[i] = vmulq_s32(in[i], scalar);
        out[i] = vrshlq_s32(out[i], vdupq_n_s32(-new_sqrt2_bits));
    }
}

static INLINE void iidentity16_xn_neon(int32x4_t *in, int32x4_t *out, int bit, int howmany) {
    const int stride = 16;
    int       i      = 0;
    do { iidentity16_x4_neon(in + i * stride, out + i * stride, bit); } while (++i < howmany);
}

static INLINE void load_buffer_s32(const int32_t *input, int32x4_t *in, const int col, const int row) {
    for (int i = 0; i < col; i++) {
        for (int j = 0; j < row >> 2; j++) { in[i + j * col] = vld1q_s32(input + i * row + j * 4); }
    }
}

void svt_av1_inv_txfm2d_add_16x16_neon(const int32_t *input, uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                       int32_t stride_w, TxType tx_type, int32_t bd) {
    int32x4_t     in[64], out[64];
    const int8_t *shift = svt_aom_inv_txfm_shift_ls[TX_16X16];

    switch (tx_type) {
    case DCT_DCT:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        idct16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        round_shift_neon(in, shift[0], 64);
        transpose_arrays_s32_16x16(in, out);
        idct16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case ADST_DCT:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        idct16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        round_shift_neon(in, shift[0], 64);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case DCT_ADST:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        round_shift_neon(in, shift[0], 64);
        transpose_arrays_s32_16x16(in, out);
        idct16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case ADST_ADST:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        round_shift_neon(in, shift[0], 64);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case FLIPADST_DCT:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        idct16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        round_shift_neon(in, shift[0], 64);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 1, shift[1], bd);
        break;
    case DCT_FLIPADST:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        round_shift_neon(in, shift[0], 64);
        transpose_arrays_s32_16x16(in, out);
        idct16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 1, 0, shift[1], bd);
        break;
    case FLIPADST_FLIPADST:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        round_shift_neon(in, shift[0], 64);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 1, 1, shift[1], bd);
        break;
    case ADST_FLIPADST:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        round_shift_neon(in, shift[0], 64);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 1, 0, shift[1], bd);
        break;
    case FLIPADST_ADST:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        round_shift_neon(in, shift[0], 64);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 1, shift[1], bd);
        break;
    case IDTX:
        load_buffer_s32(input, in, 16, 16);
        iidentity16_xn_neon(in, out, 0 /*unused*/, 4);
        round_shift_neon(out, shift[0], 64);
        iidentity16_xn_neon(out, in, 0 /*unused*/, 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case V_DCT:
        load_buffer_s32(input, in, 16, 16);
        iidentity16_xn_neon(in, out, /*unused*/ 0, 4);
        round_shift_neon(out, shift[0], 64);
        idct16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case H_DCT:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        idct16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        transpose_arrays_s32_16x16(in, out);
        round_shift_neon(out, shift[0], 64);
        iidentity16_xn_neon(out, in, /*unused*/ 0, 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case V_ADST:
        load_buffer_s32(input, in, 16, 16);
        iidentity16_xn_neon(in, out, /*unused*/ 0, 4);
        round_shift_neon(out, shift[0], 64);
        iadst16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case H_ADST:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        transpose_arrays_s32_16x16(in, out);
        round_shift_neon(out, shift[0], 64);
        iidentity16_xn_neon(out, in, /*unused*/ 0, 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case V_FLIPADST:
        load_buffer_s32(input, in, 16, 16);
        iidentity16_xn_neon(in, out, /*unused*/ 0, 4);
        round_shift_neon(out, shift[0], 64);
        iadst16_xn_neon(out, in, inv_cos_bit_col[2][2], 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 0, 1, shift[1], bd);
        break;
    case H_FLIPADST:
        load_buffer_s32(input, in, 16, 16);
        transpose_arrays_s32_16x16(in, out);
        iadst16_xn_neon(out, in, inv_cos_bit_row[2][2], 4);
        transpose_arrays_s32_16x16(in, out);
        round_shift_neon(out, shift[0], 64);
        iidentity16_xn_neon(out, in, /*unused*/ 0, 4);
        write_buffer_16x16_(in, output_r, stride_r, output_w, stride_w, 1, 0, shift[1], bd);
        break;
    default: assert(0);
    }
}

static INLINE void idct8_x4_neon(int32x4_t *in, int32x4_t *out, int32_t bit) {
    const int32_t  *cospi = cospi_arr(bit);
    const int32x4_t shift = -vdupq_n_s32(bit);
    int32x4_t       u0, u1, u2, u3, u4, u5, u6, u7;
    int32x4_t       v0, v1, v2, v3, v4, v5, v6, v7;
    int32x4_t       x, y;

    u0 = in[0];
    u1 = in[4];
    u2 = in[2];
    u3 = in[6];

    u4 = vmulq_n_s32(in[1], cospi[56]);
    u4 = vmlaq_n_s32(u4, in[7], -cospi[8]);
    u4 = vqrshlq_s32(u4, shift);

    u7 = vmulq_n_s32(in[1], cospi[8]);
    u7 = vmlaq_n_s32(u7, in[7], cospi[56]);
    u7 = vqrshlq_s32(u7, shift);

    u5 = vmulq_n_s32(in[5], cospi[24]);
    u5 = vmlaq_n_s32(u5, in[3], -cospi[40]);
    u5 = vqrshlq_s32(u5, shift);

    u6 = vmulq_n_s32(in[5], cospi[40]);
    u6 = vmlaq_n_s32(u6, in[3], cospi[24]);
    u6 = vqrshlq_s32(u6, shift);

    // stage 3
    x  = vmulq_n_s32(u0, cospi[32]);
    y  = vmulq_n_s32(u1, cospi[32]);
    v0 = vaddq_s32(x, y);
    v0 = vqrshlq_s32(v0, shift);

    v1 = vsubq_s32(x, y);
    v1 = vqrshlq_s32(v1, shift);

    v2 = vmulq_n_s32(u2, cospi[48]);
    v2 = vmlaq_n_s32(v2, u3, -cospi[16]);
    v2 = vqrshlq_s32(v2, shift);

    v3 = vmulq_n_s32(u2, cospi[16]);
    v3 = vmlaq_n_s32(v3, u3, cospi[48]);
    v3 = vqrshlq_s32(v3, shift);

    v4 = vaddq_s32(u4, u5);
    v5 = vsubq_s32(u4, u5);
    v6 = vsubq_s32(u7, u6);
    v7 = vaddq_s32(u6, u7);

    // stage 4
    u0 = vaddq_s32(v0, v3);
    u1 = vaddq_s32(v1, v2);
    u2 = vsubq_s32(v1, v2);
    u3 = vsubq_s32(v0, v3);
    u4 = v4;
    u7 = v7;

    x  = vmulq_n_s32(v5, cospi[32]);
    y  = vmulq_n_s32(v6, cospi[32]);
    u6 = vaddq_s32(y, x);
    u6 = vqrshlq_s32(u6, shift);

    u5 = vsubq_s32(y, x);
    u5 = vqrshlq_s32(u5, shift);

    // stage 5
    out[0] = vaddq_s32(u0, u7);
    out[1] = vaddq_s32(u1, u6);
    out[2] = vaddq_s32(u2, u5);
    out[3] = vaddq_s32(u3, u4);
    out[4] = vsubq_s32(u3, u4);
    out[5] = vsubq_s32(u2, u5);
    out[6] = vsubq_s32(u1, u6);
    out[7] = vsubq_s32(u0, u7);
}

static INLINE void idct8_xn_neon(int32x4_t *in, int32x4_t *out, int32_t bit, int howmany) {
    const int stride = 8;
    int       i      = 0;
    do { idct8_x4_neon(in + i * stride, out + i * stride, bit); } while (++i < howmany);
}

static INLINE void iadst8_x4_neon(int32x4_t *in, int32x4_t *out, int32_t bit) {
    const int32_t  *cospi = cospi_arr(bit);
    const int32x4_t shift = -vdupq_n_s32(bit);
    int32x4_t       u[8], v[8], x;

    // stage 0
    // stage 1
    // stage 2
    // (1)
    u[0] = vmulq_n_s32(in[7], cospi[4]);
    u[0] = vmlaq_n_s32(u[0], in[0], cospi[60]);
    u[0] = vqrshlq_s32(u[0], shift);

    u[1] = vmulq_n_s32(in[7], cospi[60]);
    u[1] = vmlsq_n_s32(u[1], in[0], cospi[4]);
    u[1] = vqrshlq_s32(u[1], shift);

    // (2)
    u[2] = vmulq_n_s32(in[5], cospi[20]);
    u[2] = vmlaq_n_s32(u[2], in[2], cospi[44]);
    u[2] = vqrshlq_s32(u[2], shift);

    u[3] = vmulq_n_s32(in[5], cospi[44]);
    u[3] = vmlsq_n_s32(u[3], in[2], cospi[20]);
    u[3] = vqrshlq_s32(u[3], shift);

    // (3)
    u[4] = vmulq_n_s32(in[3], cospi[36]);
    u[4] = vmlaq_n_s32(u[4], in[4], cospi[28]);
    u[4] = vqrshlq_s32(u[4], shift);

    u[5] = vmulq_n_s32(in[3], cospi[28]);
    u[5] = vmlsq_n_s32(u[5], in[4], cospi[36]);
    u[5] = vqrshlq_s32(u[5], shift);

    // (4)
    u[6] = vmulq_n_s32(in[1], cospi[52]);
    u[6] = vmlaq_n_s32(u[6], in[6], cospi[12]);
    u[6] = vqrshlq_s32(u[6], shift);

    u[7] = vmulq_n_s32(in[1], cospi[12]);
    u[7] = vmlsq_n_s32(u[7], in[6], cospi[52]);
    u[7] = vqrshlq_s32(u[7], shift);

    // stage 3
    v[0] = vaddq_s32(u[0], u[4]);
    v[4] = vsubq_s32(u[0], u[4]);
    v[1] = vaddq_s32(u[1], u[5]);
    v[5] = vsubq_s32(u[1], u[5]);
    v[2] = vaddq_s32(u[2], u[6]);
    v[6] = vsubq_s32(u[2], u[6]);
    v[3] = vaddq_s32(u[3], u[7]);
    v[7] = vsubq_s32(u[3], u[7]);

    // stage 4
    u[0] = v[0];
    u[1] = v[1];
    u[2] = v[2];
    u[3] = v[3];

    u[4] = vmulq_n_s32(v[4], cospi[16]);
    u[4] = vmlaq_n_s32(u[4], v[5], cospi[48]);
    u[4] = vqrshlq_s32(u[4], shift);

    u[5] = vmulq_n_s32(v[4], cospi[48]);
    u[5] = vmlsq_n_s32(u[5], v[5], cospi[16]);
    u[5] = vqrshlq_s32(u[5], shift);

    u[6] = vmulq_n_s32(v[6], -cospi[48]);
    u[6] = vmlaq_n_s32(u[6], v[7], cospi[16]);
    u[6] = vqrshlq_s32(u[6], shift);

    u[7] = vmulq_n_s32(v[6], cospi[16]);
    u[7] = vmlsq_n_s32(u[7], v[7], -cospi[48]);
    u[7] = vqrshlq_s32(u[7], shift);

    // stage 5
    v[0] = vaddq_s32(u[0], u[2]);
    v[2] = vsubq_s32(u[0], u[2]);
    v[1] = vaddq_s32(u[1], u[3]);
    v[3] = vsubq_s32(u[1], u[3]);
    v[4] = vaddq_s32(u[4], u[6]);
    v[6] = vsubq_s32(u[4], u[6]);
    v[5] = vaddq_s32(u[5], u[7]);
    v[7] = vsubq_s32(u[5], u[7]);

    // stage 6
    u[0] = v[0];
    u[1] = v[1];
    u[4] = v[4];
    u[5] = v[5];

    v[0] = vmulq_n_s32(v[2], cospi[32]);
    x    = vmulq_n_s32(v[3], cospi[32]);
    u[2] = vaddq_s32(v[0], x);
    u[2] = vqrshlq_s32(u[2], shift);

    u[3] = vsubq_s32(v[0], x);
    u[3] = vqrshlq_s32(u[3], shift);

    v[0] = vmulq_n_s32(v[6], cospi[32]);
    x    = vmulq_n_s32(v[7], cospi[32]);
    u[6] = vaddq_s32(v[0], x);
    u[6] = vqrshlq_s32(u[6], shift);

    u[7] = vsubq_s32(v[0], x);
    u[7] = vqrshlq_s32(u[7], shift);

    // stage 7
    out[0] = u[0];
    out[1] = vnegq_s32(u[4]);
    out[2] = u[6];
    out[3] = vnegq_s32(u[2]);
    out[4] = u[3];
    out[5] = vnegq_s32(u[7]);
    out[6] = u[5];
    out[7] = vnegq_s32(u[1]);
}

static INLINE void iadst8_xn_neon(int32x4_t *in, int32x4_t *out, int32_t bit, const int howmany) {
    const int stride = 8;
    int       i      = 0;
    do { iadst8_x4_neon(in + i * stride, out + i * stride, bit); } while (++i < howmany);
}

void svt_av1_inv_txfm2d_add_8x8_neon(const int32_t *input, uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                     int32_t stride_w, TxType tx_type, int32_t bd) {
    int32x4_t     in[16], out[16];
    const int8_t *shift = svt_aom_inv_txfm_shift_ls[TX_8X8];

    switch (tx_type) {
    case DCT_DCT:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        idct8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        idct8_xn_neon(out, in, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case ADST_DCT:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        idct8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        iadst8_xn_neon(out, in, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case DCT_ADST:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        iadst8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        idct8_xn_neon(out, in, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case ADST_ADST:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        iadst8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        iadst8_xn_neon(out, in, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case FLIPADST_DCT:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        idct8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        iadst8_xn_neon(out, in, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(in, output_r, stride_r, output_w, stride_w, 0, 1, shift[1], bd);
        break;
    case DCT_FLIPADST:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        iadst8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        idct8_xn_neon(out, in, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(in, output_r, stride_r, output_w, stride_w, 1, 0, shift[1], bd);
        break;
    case ADST_FLIPADST:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        iadst8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        iadst8_xn_neon(out, in, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(in, output_r, stride_r, output_w, stride_w, 1, 0, shift[1], bd);
        break;
    case FLIPADST_FLIPADST:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        iadst8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        iadst8_xn_neon(out, in, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(in, output_r, stride_r, output_w, stride_w, 1, 1, shift[1], bd);
        break;
    case FLIPADST_ADST:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        iadst8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        iadst8_xn_neon(out, in, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(in, output_r, stride_r, output_w, stride_w, 0, 1, shift[1], bd);
        break;
    case IDTX:
        load_buffer_s32(input, in, 8, 8);
        // Operations can be joined together without losing precision
        // svt_av1_iidentity8_c() shift left 1 bits
        // round_shift_neon(, -shift[0]) shift right 1 bits
        // svt_av1_iidentity8_c() shift left 1 bits
        // round_shift_neon(, -shift[1]) shift right 4 bits with complement
        write_buffer_8x8_(in, output_r, stride_r, output_w, stride_w, 0, 0, shift[0] + shift[1] + 2, bd);
        break;
    case V_DCT:
        load_buffer_s32(input, in, 8, 8);
        // svt_av1_iidentity8_c() shift left 1 bits
        // round_shift_neon(, -shift[0]) shift right 1 bits
        idct8_xn_neon(in, out, inv_cos_bit_row[1][1], 2);
        write_buffer_8x8_(out, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case H_DCT:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        idct8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        // svt_av1_iidentity8_c() shift left 1 bits
        // round_shift_neon(, -shift[1]) shift right 4 bits with complement
        write_buffer_8x8_(out, output_r, stride_r, output_w, stride_w, 0, 0, shift[1] + 1, bd);
        break;
    case V_ADST:
        load_buffer_s32(input, in, 8, 8);
        // svt_av1_iidentity8_c() shift left 1 bits
        // round_shift_neon(, -shift[0]) shift right 1 bits
        iadst8_xn_neon(in, out, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(out, output_r, stride_r, output_w, stride_w, 0, 0, shift[1], bd);
        break;
    case H_ADST:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        iadst8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        // svt_av1_iidentity8_c() shift left 1 bits
        // round_shift_neon(, -shift[1]) shift right 4 bits with complement
        write_buffer_8x8_(out, output_r, stride_r, output_w, stride_w, 0, 0, shift[1] + 1, bd);
        break;
    case V_FLIPADST:
        load_buffer_s32(input, in, 8, 8);
        // svt_av1_iidentity8_c() shift left 1 bits
        // round_shift_neon(, -shift[0]) shift right 1 bits
        iadst8_xn_neon(in, out, inv_cos_bit_col[1][1], 2);
        write_buffer_8x8_(out, output_r, stride_r, output_w, stride_w, 0, 1, shift[1], bd);
        break;
    case H_FLIPADST:
        load_buffer_s32(input, in, 8, 8);
        transpose_arrays_s32_8x8(in, out);
        iadst8_xn_neon(out, in, inv_cos_bit_row[1][1], 2);
        transpose_arrays_s32_8x8(in, out);
        round_shift_neon(out, shift[0], 16);
        // svt_av1_iidentity8_c() shift left 1 bits
        // round_shift_neon(, -shift[1]) shift right 4 bits with complement
        write_buffer_8x8_(out, output_r, stride_r, output_w, stride_w, 1, 0, shift[1] + 1, bd);
        break;
    default: assert(0);
    }
}

static INLINE void highbd_clamp_s32_neon(const int32x4_t *in, int32x4_t *out, const int32x4_t *clamp_lo,
                                         const int32x4_t *clamp_hi, int32_t size) {
    int32x4_t a0, a1;
    for (int32_t i = 0; i < size; i += 4) {
        a0     = vmaxq_s32(in[i], *clamp_lo);
        out[i] = vminq_s32(a0, *clamp_hi);

        a1         = vmaxq_s32(in[i + 1], *clamp_lo);
        out[i + 1] = vminq_s32(a1, *clamp_hi);

        a0         = vmaxq_s32(in[i + 2], *clamp_lo);
        out[i + 2] = vminq_s32(a0, *clamp_hi);

        a1         = vmaxq_s32(in[i + 3], *clamp_lo);
        out[i + 3] = vminq_s32(a1, *clamp_hi);
    }
}

static INLINE void idct4_x4_neon(int32x4_t *in, int32x4_t *out, int32_t bit) {
    const int32_t *cospi = cospi_arr(bit);

    // stage 2
    int32x4_t x  = vmulq_n_s32(in[0], cospi[32]);
    int32x4_t y  = vmulq_n_s32(in[2], cospi[32]);
    int32x4_t d0 = vaddq_s32(x, y);
    d0           = vqrshlq_s32(d0, vdupq_n_s32(-bit));

    int32x4_t d1 = vsubq_s32(x, y);
    d1           = vqrshlq_s32(d1, vdupq_n_s32(-bit));

    int32x4_t d2 = vmulq_n_s32(in[1], cospi[48]);
    d2           = vmlaq_n_s32(d2, in[3], -cospi[16]);
    d2           = vqrshlq_s32(d2, vdupq_n_s32(-bit));

    int32x4_t d3 = vmulq_n_s32(in[1], cospi[16]);
    d3           = vmlaq_n_s32(d3, in[3], cospi[48]);
    d3           = vqrshlq_s32(d3, vdupq_n_s32(-bit));

    // stage 3
    out[0] = vaddq_s32(d0, d3);
    out[3] = vsubq_s32(d0, d3);
    out[1] = vaddq_s32(d1, d2);
    out[2] = vsubq_s32(d1, d2);
}

static INLINE void write_buffer_4x4(int32x4_t *in, const uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                    int32_t stride_w, int32_t fliplr, int32_t flipud, int32_t bd) {
    int32x4_t u0, u1, u2, u3;
    int16x4_t v0, v1, v2, v3;

    load_s16_4x4((int16_t *)output_r, stride_r, &v0, &v1, &v2, &v3);

    if (fliplr) {
        in[0] = vcombine_s32(vrev64_s32(vget_high_s32(in[0])), vrev64_s32(vget_low_s32(in[0])));
        in[1] = vcombine_s32(vrev64_s32(vget_high_s32(in[1])), vrev64_s32(vget_low_s32(in[1])));
        in[2] = vcombine_s32(vrev64_s32(vget_high_s32(in[2])), vrev64_s32(vget_low_s32(in[2])));
        in[3] = vcombine_s32(vrev64_s32(vget_high_s32(in[3])), vrev64_s32(vget_low_s32(in[3])));
    }

    if (flipud) {
        u0 = vaddw_s16(in[3], v0);
        u1 = vaddw_s16(in[2], v1);
        u2 = vaddw_s16(in[1], v2);
        u3 = vaddw_s16(in[0], v3);
    } else {
        u0 = vaddw_s16(in[0], v0);
        u1 = vaddw_s16(in[1], v1);
        u2 = vaddw_s16(in[2], v2);
        u3 = vaddw_s16(in[3], v3);
    }

    uint16x8_t res0 = vcombine_u16(vqmovun_s32(u0), vqmovun_s32(u1));
    uint16x8_t res1 = vcombine_u16(vqmovun_s32(u2), vqmovun_s32(u3));

    res0 = vminq_u16(res0, vdupq_n_u16((1 << bd) - 1));
    res1 = vminq_u16(res1, vdupq_n_u16((1 << bd) - 1));

    vst1_u16(output_w + 0 * stride_w, vget_low_u16(res0));
    vst1_u16(output_w + 1 * stride_w, vget_high_u16(res0));
    vst1_u16(output_w + 2 * stride_w, vget_low_u16(res1));
    vst1_u16(output_w + 3 * stride_w, vget_high_u16(res1));
}

static INLINE void iadst4_x4_neon(int32x4_t *in, int32x4_t *out, int bit) {
    const int32_t *sinpi = sinpi_arr(bit);

    // stage 1
    int32x4_t s0 = vmulq_n_s32(in[0], sinpi[1]);
    int32x4_t s1 = vmulq_n_s32(in[0], sinpi[2]);
    int32x4_t s2 = vmulq_n_s32(in[1], sinpi[3]);
    int32x4_t s3 = vmulq_n_s32(in[2], sinpi[4]);
    int32x4_t s4 = vmulq_n_s32(in[2], sinpi[1]);
    int32x4_t s5 = vmulq_n_s32(in[3], sinpi[2]);
    int32x4_t s6 = vmulq_n_s32(in[3], sinpi[4]);
    int32x4_t t  = vsubq_s32(in[0], in[2]);

    // stage 2
    int32x4_t s7 = vaddq_s32(t, in[3]);

    // stage 3
    s0 = vaddq_s32(s0, s3);
    s1 = vsubq_s32(s1, s4);
    s3 = s2;
    s2 = vmulq_n_s32(s7, sinpi[3]);

    // stage 4
    s0 = vaddq_s32(s0, s5);
    s1 = vsubq_s32(s1, s6);

    // stage 5
    int32x4_t u0 = vaddq_s32(s0, s3);
    int32x4_t u1 = vaddq_s32(s1, s3);
    int32x4_t u2 = s2;
    int32x4_t u3 = vaddq_s32(s0, s1);

    // stage 6
    u3 = vsubq_s32(u3, s3);

    u0 = vqrshlq_s32(u0, vdupq_n_s32(-bit));
    u1 = vqrshlq_s32(u1, vdupq_n_s32(-bit));
    u2 = vqrshlq_s32(u2, vdupq_n_s32(-bit));
    u3 = vqrshlq_s32(u3, vdupq_n_s32(-bit));

    out[0] = u0;
    out[1] = u1;
    out[2] = u2;
    out[3] = u3;
}

static INLINE void iidentity4_x4_neon(int32x4_t *in, int32x4_t *out) {
    out[0] = vmulq_n_s32(in[0], new_sqrt2);
    out[1] = vmulq_n_s32(in[1], new_sqrt2);
    out[2] = vmulq_n_s32(in[2], new_sqrt2);
    out[3] = vmulq_n_s32(in[3], new_sqrt2);

    round_shift_neon(out, -new_sqrt2_bits, 4);
}

static INLINE void clamp_neon(const int32x4_t *in, int32x4_t *out, int log_range, int size) {
    const int32x4_t clamp_lo = vdupq_n_s32(-(1 << (log_range - 1)));
    const int32x4_t clamp_hi = vdupq_n_s32((1 << (log_range - 1)) - 1);
    highbd_clamp_s32_neon(in, out, &clamp_lo, &clamp_hi, size);
}

void svt_av1_inv_txfm2d_add_4x4_neon(const int32_t *input, uint16_t *output_r, int32_t stride_r, uint16_t *output_w,
                                     int32_t stride_w, TxType tx_type, int32_t bd) {
    int32x4_t in[4];

    switch (tx_type) {
    case DCT_DCT:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        idct4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        idct4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 0, bd);
        break;
    case ADST_DCT:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        idct4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iadst4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 0, bd);
        break;
    case DCT_ADST:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        iadst4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        idct4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 0, bd);
        break;
    case ADST_ADST:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        iadst4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iadst4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 0, bd);
        break;
    case FLIPADST_DCT:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        idct4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iadst4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 1, bd);
        break;
    case DCT_FLIPADST:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        iadst4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        idct4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 1, 0, bd);
        break;
    case FLIPADST_FLIPADST:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        iadst4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iadst4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 1, 1, bd);
        break;
    case ADST_FLIPADST:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        iadst4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iadst4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 1, 0, bd);
        break;
    case FLIPADST_ADST:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        iadst4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iadst4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 1, bd);
        break;
    case IDTX:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        iidentity4_x4_neon(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iidentity4_x4_neon(in, in);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 0, bd);
        break;
    case V_DCT:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        iidentity4_x4_neon(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        idct4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 0, bd);
        break;
    case H_DCT:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        idct4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iidentity4_x4_neon(in, in);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 0, bd);
        break;
    case V_ADST:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        iidentity4_x4_neon(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iadst4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 0, bd);
        break;
    case H_ADST:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        iadst4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iidentity4_x4_neon(in, in);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 0, bd);
        break;
    case V_FLIPADST:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        iidentity4_x4_neon(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iadst4_x4_neon(in, in, inv_cos_bit_col[0][0]);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 0, 1, bd);
        break;
    case H_FLIPADST:
        load_s32_4x4((int32_t *)input, 4, &in[0], &in[1], &in[2], &in[3]);
        transpose_arrays_s32_4x4(in, in);
        iadst4_x4_neon(in, in, inv_cos_bit_row[0][0]);
        transpose_arrays_s32_4x4(in, in);
        clamp_neon(in, in, AOMMAX(bd + 6, 16), 4);
        iidentity4_x4_neon(in, in);
        round_shift_neon(in, /*shift[1]=*/-4, 4);
        write_buffer_4x4(in, output_r, stride_r, output_w, stride_w, 1, 0, bd);
        break;
    default: assert(0);
    }
}
