/*
* Copyright(c) 2019 Intel Corporation
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include "definitions.h"
#include "intra_prediction.h"
#include <immintrin.h>
#include "lpf_common_sse2.h"
#include "txfm_common_avx2.h"
#include "common_dsp_rtcd.h"

// Indices are sign, integer, and fractional part of the gradient value
static const uint8_t gradient_to_angle_bin[2][7][16] = {
    {
        {6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
        {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    },
    {
        {6, 6, 6, 6, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4},
        {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3},
        {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
        {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
        {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
        {3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2},
        {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    },
};

static INLINE __m256i __m256i_div_epi32(const __m256i *a, const __m256i *b) {
    __m256 d_f = _mm256_div_ps(_mm256_cvtepi32_ps(*a), _mm256_cvtepi32_ps(*b));
    //Integer devide round down
    return _mm256_cvtps_epi32(_mm256_floor_ps(d_f));
}

static INLINE void get_gradient_hist_avx2_internal(const __m256i *src1, const __m256i *src2, const __m256i *src3,
                                                   int16_t *dy_mask_array, int16_t *quot_array, int16_t *remd_array,
                                                   int16_t *sn_array, int32_t *temp_array) {
    const __m256i zero       = _mm256_setzero_si256();
    const __m256i val_15_i16 = _mm256_set1_epi16(15);
    const __m256i val_6_i16  = _mm256_set1_epi16(6);
    __m256i       dx, dy;
    __m256i       tmp1_32, tmp2_32;
    __m256i       dx1_32, dx2_32;
    __m256i       dy1_32, dy2_32;
    __m256i       sn;
    __m256i       remd;
    __m256i       quot;
    __m256i       dy_mask;

    dx = _mm256_sub_epi16(*src1, *src2);
    dy = _mm256_sub_epi16(*src1, *src3);

    //sn = (dx > 0) ^ (dy > 0);
    sn = _mm256_xor_si256(dx, dy); //result is 0 or 0xFFFF
    sn = _mm256_srli_epi16(sn, 15); //change output from 0xFFFF to 1

    //mask which shows where are zeros in dy register 0/1
    dy_mask = _mm256_srli_epi16(_mm256_cmpeq_epi16(dy, zero), 15);

    //dx = abs(dx); dy = abs(dy);
    dx = _mm256_abs_epi16(dx);
    dy = _mm256_abs_epi16(dy);

    _mm256_add_epi16(dy, dy_mask);

    //  temp = dx * dx + dy * dy;
    dx1_32 = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(dx)); //dx
    dy1_32 = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(dy)); //dy

    tmp1_32 = _mm256_add_epi32(_mm256_mullo_epi32(dx1_32, dx1_32), _mm256_mullo_epi32(dy1_32, dy1_32));

    dx2_32 = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(dx, 1));
    dy2_32 = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(dy, 1));

    tmp2_32 = _mm256_add_epi32(_mm256_mullo_epi32(dx2_32, dx2_32), _mm256_mullo_epi32(dy2_32, dy2_32));

    /* Code:
     quot16 = (dx << 4) / dy;
     quot = quot16 >> 4;
     remd = = (quot16 & (15));
    Equivalent of:
     quot = dx / dy;
     remd = (dx % dy) * 16 / dy;*/

    //quot16 = (dx << 4) / dy;
    dx1_32               = _mm256_slli_epi32(dx1_32, 4);
    dx2_32               = _mm256_slli_epi32(dx2_32, 4);
    const __m256i d1_i32 = __m256i_div_epi32(&dx1_32, &dy1_32);
    const __m256i d2_i32 = __m256i_div_epi32(&dx2_32, &dy2_32);
    __m256i       quot16 = _mm256_permute4x64_epi64(_mm256_packs_epi32(d1_i32, d2_i32), 0xD8);

    quot = _mm256_srli_epi16(quot16, 4);

    //remd = (quot16 & (15));
    remd = _mm256_and_si256(quot16, val_15_i16);

    //AOMMIN(remdA, 15)
    remd = _mm256_min_epi16(remd, val_15_i16);
    //AOMMIN(quotA, 6)
    quot = _mm256_min_epi16(quot, val_6_i16);

    _mm256_storeu_si256((__m256i *)dy_mask_array, dy_mask);
    _mm256_storeu_si256((__m256i *)quot_array, quot);
    _mm256_storeu_si256((__m256i *)remd_array, remd);
    _mm256_storeu_si256((__m256i *)sn_array, sn);
    _mm256_storeu_si256((__m256i *)temp_array, tmp1_32);
    _mm256_storeu_si256((__m256i *)&temp_array[8], tmp2_32);
}

void svt_av1_get_gradient_hist_avx2(const uint8_t *src, int src_stride, int rows, int cols, uint64_t *hist) {
    src += src_stride;

    __m128i tmp_src;
    __m256i src1; //src[c]
    __m256i src2; //src[c-1]
    __m256i src3; //src[c - src_stride]

    DECLARE_ALIGNED(64, int16_t, dy_mask_array[16]);
    DECLARE_ALIGNED(64, int16_t, quot_array[16]);
    DECLARE_ALIGNED(64, int16_t, remd_array[16]);
    DECLARE_ALIGNED(64, int16_t, sn_array[16]);
    DECLARE_ALIGNED(64, int32_t, temp_array[16]);

    if (cols < 8) { //i.e cols ==4
        for (int r = 1; r < rows; r += 4) {
            if ((r + 3) >= rows) {
                tmp_src = _mm_set_epi32(0,
                                        *(uint32_t *)(src + 1),
                                        *(uint32_t *)(src + 1 + src_stride),
                                        *(uint32_t *)(src + 1 + 2 * src_stride));
                src1    = _mm256_cvtepu8_epi16(tmp_src);

                tmp_src = _mm_set_epi32(
                    0, *(uint32_t *)(src), *(uint32_t *)(src + src_stride), *(uint32_t *)(src + 2 * src_stride));
                src2 = _mm256_cvtepu8_epi16(tmp_src);

                tmp_src = _mm_set_epi32(0,
                                        *(uint32_t *)(src + 1 - src_stride),
                                        *(uint32_t *)(src + 1),
                                        *(uint32_t *)(src + 1 + src_stride));
                src3    = _mm256_cvtepu8_epi16(tmp_src);
            } else {
                tmp_src = _mm_set_epi32(*(uint32_t *)(src + 1),
                                        *(uint32_t *)(src + 1 + src_stride),
                                        *(uint32_t *)(src + 1 + 2 * src_stride),
                                        *(uint32_t *)(src + 1 + 3 * src_stride));
                src1    = _mm256_cvtepu8_epi16(tmp_src);

                tmp_src = _mm_set_epi32(*(uint32_t *)(src),
                                        *(uint32_t *)(src + src_stride),
                                        *(uint32_t *)(src + 2 * src_stride),
                                        *(uint32_t *)(src + 3 * src_stride));
                src2    = _mm256_cvtepu8_epi16(tmp_src);

                tmp_src = _mm_set_epi32(*(uint32_t *)(src + 1 - src_stride),
                                        *(uint32_t *)(src + 1),
                                        *(uint32_t *)(src + 1 + src_stride),
                                        *(uint32_t *)(src + 1 + 2 * src_stride));
                src3    = _mm256_cvtepu8_epi16(tmp_src);
            }

            get_gradient_hist_avx2_internal(
                &src1, &src2, &src3, dy_mask_array, quot_array, remd_array, sn_array, temp_array);

            if ((r + 3) >= rows) {
                for (int w = 0; w < 11; ++w) {
                    if (w == 3 || w == 7)
                        continue;
                    if (dy_mask_array[w] != 1) {
                        int index = gradient_to_angle_bin[sn_array[w]][quot_array[w]][remd_array[w]];
                        hist[index] += temp_array[w];
                    } else {
                        hist[2] += temp_array[w];
                    }
                }
            } else {
                for (int w = 0; w < 15; ++w) {
                    if (w == 3 || w == 7 || w == 11)
                        continue;
                    if (dy_mask_array[w] != 1) {
                        int index = gradient_to_angle_bin[sn_array[w]][quot_array[w]][remd_array[w]];
                        hist[index] += temp_array[w];
                    } else {
                        hist[2] += temp_array[w];
                    }
                }
            }
            src += 4 * src_stride;
        }
    } else if (cols < 16) { //i.e cols ==8
        for (int r = 1; r < rows; r += 2) {
            if ((r + 1) >= rows) {
                tmp_src = _mm_set1_epi64x(*(uint64_t *)(src + 1));
                src1    = _mm256_cvtepu8_epi16(tmp_src);

                tmp_src = _mm_set1_epi64x(*(uint64_t *)(src));
                src2    = _mm256_cvtepu8_epi16(tmp_src);

                tmp_src = _mm_set1_epi64x(*(uint64_t *)(src + 1 - src_stride));
                src3    = _mm256_cvtepu8_epi16(tmp_src);
            } else {
                tmp_src = _mm_set_epi64x(*(uint64_t *)(src + 1 + src_stride), *(uint64_t *)(src + 1));
                src1    = _mm256_cvtepu8_epi16(tmp_src);

                tmp_src = _mm_set_epi64x(*(uint64_t *)(src + src_stride), *(uint64_t *)(src));
                src2    = _mm256_cvtepu8_epi16(tmp_src);

                tmp_src = _mm_set_epi64x(*(uint64_t *)(src + 1), *(uint64_t *)(src + 1 - src_stride));
                src3    = _mm256_cvtepu8_epi16(tmp_src);
            }

            get_gradient_hist_avx2_internal(
                &src1, &src2, &src3, dy_mask_array, quot_array, remd_array, sn_array, temp_array);

            if ((r + 1) >= rows) {
                for (int w = 0; w < 7; ++w) {
                    if (dy_mask_array[w] != 1) {
                        int index = gradient_to_angle_bin[sn_array[w]][quot_array[w]][remd_array[w]];
                        hist[index] += temp_array[w];
                    } else {
                        hist[2] += temp_array[w];
                    }
                }
            } else {
                for (int w = 0; w < 15; ++w) {
                    if (w == 7)
                        continue;
                    if (dy_mask_array[w] != 1) {
                        int index = gradient_to_angle_bin[sn_array[w]][quot_array[w]][remd_array[w]];
                        hist[index] += temp_array[w];
                    } else {
                        hist[2] += temp_array[w];
                    }
                }
            }
            src += 2 * src_stride;
        }
    } else {
        for (int r = 1; r < rows; ++r) {
            int c = 1;
            for (; cols - c >= 15; c += 16) {
                //read too many [1:16], while max is 15
                src1 = _mm256_cvtepu8_epi16(_mm_loadu_si128((__m128i const *)&src[c]));
                src2 = _mm256_cvtepu8_epi16(_mm_loadu_si128((__m128i const *)&src[c - 1]));
                src3 = _mm256_cvtepu8_epi16(_mm_loadu_si128((__m128i const *)&src[c - src_stride]));

                get_gradient_hist_avx2_internal(
                    &src1, &src2, &src3, dy_mask_array, quot_array, remd_array, sn_array, temp_array);

                int max = 16;
                if (c + 16 > cols) {
                    max = 15;
                }

                for (int w = 0; w < max; ++w) {
                    if (dy_mask_array[w] != 1) {
                        int index = gradient_to_angle_bin[sn_array[w]][quot_array[w]][remd_array[w]];
                        hist[index] += temp_array[w];
                    } else {
                        hist[2] += temp_array[w];
                    }
                }
            }
            src += src_stride;
        }
    }
}

#ifndef _mm256_setr_m128i
#define _mm256_setr_m128i(/* __m128i */ lo, /* __m128i */ hi) \
    _mm256_insertf128_si256(_mm256_castsi128_si256(lo), (hi), 0x1)
#endif

static INLINE void highbd_transpose16x4_8x8_sse2(__m128i *x, __m128i *d) {
    __m128i r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15;

    r0 = _mm_unpacklo_epi16(x[0], x[1]);
    r1 = _mm_unpacklo_epi16(x[2], x[3]);
    r2 = _mm_unpacklo_epi16(x[4], x[5]);
    r3 = _mm_unpacklo_epi16(x[6], x[7]);

    r4 = _mm_unpacklo_epi16(x[8], x[9]);
    r5 = _mm_unpacklo_epi16(x[10], x[11]);
    r6 = _mm_unpacklo_epi16(x[12], x[13]);
    r7 = _mm_unpacklo_epi16(x[14], x[15]);

    r8  = _mm_unpacklo_epi32(r0, r1);
    r9  = _mm_unpackhi_epi32(r0, r1);
    r10 = _mm_unpacklo_epi32(r2, r3);
    r11 = _mm_unpackhi_epi32(r2, r3);

    r12 = _mm_unpacklo_epi32(r4, r5);
    r13 = _mm_unpackhi_epi32(r4, r5);
    r14 = _mm_unpacklo_epi32(r6, r7);
    r15 = _mm_unpackhi_epi32(r6, r7);

    r0 = _mm_unpacklo_epi64(r8, r9);
    r1 = _mm_unpackhi_epi64(r8, r9);
    r2 = _mm_unpacklo_epi64(r10, r11);
    r3 = _mm_unpackhi_epi64(r10, r11);

    r4 = _mm_unpacklo_epi64(r12, r13);
    r5 = _mm_unpackhi_epi64(r12, r13);
    r6 = _mm_unpacklo_epi64(r14, r15);
    r7 = _mm_unpackhi_epi64(r14, r15);

    d[0] = _mm_unpacklo_epi64(r0, r2);
    d[1] = _mm_unpacklo_epi64(r4, r6);
    d[2] = _mm_unpacklo_epi64(r1, r3);
    d[3] = _mm_unpacklo_epi64(r5, r7);

    d[4] = _mm_unpackhi_epi64(r0, r2);
    d[5] = _mm_unpackhi_epi64(r4, r6);
    d[6] = _mm_unpackhi_epi64(r1, r3);
    d[7] = _mm_unpackhi_epi64(r5, r7);
}
static INLINE void highbd_transpose4x16_avx2(__m256i *x, __m256i *d) {
    __m256i w0, w1, w2, w3, ww0, ww1;

    w0 = _mm256_unpacklo_epi16(x[0], x[1]); // 00 10 01 11 02 12 03 13
    w1 = _mm256_unpacklo_epi16(x[2], x[3]); // 20 30 21 31 22 32 23 33
    w2 = _mm256_unpackhi_epi16(x[0], x[1]); // 40 50 41 51 42 52 43 53
    w3 = _mm256_unpackhi_epi16(x[2], x[3]); // 60 70 61 71 62 72 63 73

    ww0 = _mm256_unpacklo_epi32(w0, w1); // 00 10 20 30 01 11 21 31
    ww1 = _mm256_unpacklo_epi32(w2, w3); // 40 50 60 70 41 51 61 71

    d[0] = _mm256_unpacklo_epi64(ww0, ww1); // 00 10 20 30 40 50 60 70
    d[1] = _mm256_unpackhi_epi64(ww0, ww1); // 01 11 21 31 41 51 61 71

    ww0 = _mm256_unpackhi_epi32(w0, w1); // 02 12 22 32 03 13 23 33
    ww1 = _mm256_unpackhi_epi32(w2, w3); // 42 52 62 72 43 53 63 73

    d[2] = _mm256_unpacklo_epi64(ww0, ww1); // 02 12 22 32 42 52 62 72
    d[3] = _mm256_unpackhi_epi64(ww0, ww1); // 03 13 23 33 43 53 63 73
}

static INLINE void highbd_transpose8x16_16x8_avx2(__m256i *x, __m256i *d) {
    __m256i w0, w1, w2, w3, ww0, ww1;

    w0 = _mm256_unpacklo_epi16(x[0], x[1]); // 00 10 01 11 02 12 03 13
    w1 = _mm256_unpacklo_epi16(x[2], x[3]); // 20 30 21 31 22 32 23 33
    w2 = _mm256_unpacklo_epi16(x[4], x[5]); // 40 50 41 51 42 52 43 53
    w3 = _mm256_unpacklo_epi16(x[6], x[7]); // 60 70 61 71 62 72 63 73

    ww0 = _mm256_unpacklo_epi32(w0, w1); // 00 10 20 30 01 11 21 31
    ww1 = _mm256_unpacklo_epi32(w2, w3); // 40 50 60 70 41 51 61 71

    d[0] = _mm256_unpacklo_epi64(ww0, ww1); // 00 10 20 30 40 50 60 70
    d[1] = _mm256_unpackhi_epi64(ww0, ww1); // 01 11 21 31 41 51 61 71

    ww0 = _mm256_unpackhi_epi32(w0, w1); // 02 12 22 32 03 13 23 33
    ww1 = _mm256_unpackhi_epi32(w2, w3); // 42 52 62 72 43 53 63 73

    d[2] = _mm256_unpacklo_epi64(ww0, ww1); // 02 12 22 32 42 52 62 72
    d[3] = _mm256_unpackhi_epi64(ww0, ww1); // 03 13 23 33 43 53 63 73

    w0 = _mm256_unpackhi_epi16(x[0], x[1]); // 04 14 05 15 06 16 07 17
    w1 = _mm256_unpackhi_epi16(x[2], x[3]); // 24 34 25 35 26 36 27 37
    w2 = _mm256_unpackhi_epi16(x[4], x[5]); // 44 54 45 55 46 56 47 57
    w3 = _mm256_unpackhi_epi16(x[6], x[7]); // 64 74 65 75 66 76 67 77

    ww0 = _mm256_unpacklo_epi32(w0, w1); // 04 14 24 34 05 15 25 35
    ww1 = _mm256_unpacklo_epi32(w2, w3); // 44 54 64 74 45 55 65 75

    d[4] = _mm256_unpacklo_epi64(ww0, ww1); // 04 14 24 34 44 54 64 74
    d[5] = _mm256_unpackhi_epi64(ww0, ww1); // 05 15 25 35 45 55 65 75

    ww0 = _mm256_unpackhi_epi32(w0, w1); // 06 16 26 36 07 17 27 37
    ww1 = _mm256_unpackhi_epi32(w2, w3); // 46 56 66 76 47 57 67 77

    d[6] = _mm256_unpacklo_epi64(ww0, ww1); // 06 16 26 36 46 56 66 76
    d[7] = _mm256_unpackhi_epi64(ww0, ww1); // 07 17 27 37 47 57 67 77
}

static INLINE __m128i dc_sum_16_sse2(const uint8_t *ref) {
    __m128i       x    = _mm_loadu_si128((__m128i const *)ref);
    const __m128i zero = _mm_setzero_si128();
    x                  = _mm_sad_epu8(x, zero);
    const __m128i high = _mm_unpackhi_epi64(x, x);
    return _mm_add_epi16(x, high);
}

static INLINE __m128i dc_sum_32_sse2(const uint8_t *ref) {
    __m128i       x0   = _mm_loadu_si128((__m128i const *)ref);
    __m128i       x1   = _mm_loadu_si128((__m128i const *)(ref + 16));
    const __m128i zero = _mm_setzero_si128();
    x0                 = _mm_sad_epu8(x0, zero);
    x1                 = _mm_sad_epu8(x1, zero);
    x0                 = _mm_add_epi16(x0, x1);
    const __m128i high = _mm_unpackhi_epi64(x0, x0);
    return _mm_add_epi16(x0, high);
}

static INLINE __m256i dc_sum_32(const uint8_t *ref) {
    const __m256i x    = _mm256_loadu_si256((const __m256i *)ref);
    const __m256i zero = _mm256_setzero_si256();
    __m256i       y    = _mm256_sad_epu8(x, zero);
    __m256i       u    = _mm256_permute2x128_si256(y, y, 1);
    y                  = _mm256_add_epi64(u, y);
    u                  = _mm256_unpackhi_epi64(y, y);
    return _mm256_add_epi16(y, u);
}
static INLINE void row_store_32xh(const __m256i *r, int32_t height, uint8_t *dst, ptrdiff_t stride) {
    for (int32_t i = 0; i < height; ++i) {
        _mm256_storeu_si256((__m256i *)dst, *r);
        dst += stride;
    }
}

static INLINE void row_store_64xh(const __m256i *r, int32_t height, uint8_t *dst, ptrdiff_t stride) {
    for (int32_t i = 0; i < height; ++i) {
        _mm256_storeu_si256((__m256i *)dst, *r);
        _mm256_storeu_si256((__m256i *)(dst + 32), *r);
        dst += stride;
    }
}
static INLINE __m256i dc_sum_64(const uint8_t *ref) {
    const __m256i x0   = _mm256_loadu_si256((const __m256i *)ref);
    const __m256i x1   = _mm256_loadu_si256((const __m256i *)(ref + 32));
    const __m256i zero = _mm256_setzero_si256();
    __m256i       y0   = _mm256_sad_epu8(x0, zero);
    __m256i       y1   = _mm256_sad_epu8(x1, zero);
    y0                 = _mm256_add_epi64(y0, y1);
    __m256i u0         = _mm256_permute2x128_si256(y0, y0, 1);
    y0                 = _mm256_add_epi64(u0, y0);
    u0                 = _mm256_unpackhi_epi64(y0, y0);
    return _mm256_add_epi16(y0, u0);
}
void svt_aom_dc_predictor_64x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i sum_above = dc_sum_64(above);
    __m256i       sum_left  = dc_sum_64(left);
    sum_left                = _mm256_add_epi16(sum_left, sum_above);
    uint32_t sum            = _mm_cvtsi128_si32(_mm256_castsi256_si128(sum_left));
    sum += 64;
    sum /= 128;
    const __m256i row = _mm256_set1_epi8((uint8_t)sum);
    row_store_64xh(&row, 64, dst, stride);
}

void svt_aom_dc_left_predictor_64x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i sum = dc_sum_64(left);
    (void)above;

    const __m256i thirtytwo = _mm256_set1_epi16(32);
    sum                     = _mm256_add_epi16(sum, thirtytwo);
    sum                     = _mm256_srai_epi16(sum, 6);
    const __m256i zero      = _mm256_setzero_si256();
    __m256i       row       = _mm256_shuffle_epi8(sum, zero);
    row_store_64xh(&row, 64, dst, stride);
}
void svt_aom_dc_top_predictor_64x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i sum = dc_sum_64(above);
    (void)left;

    const __m256i thirtytwo = _mm256_set1_epi16(32);
    sum                     = _mm256_add_epi16(sum, thirtytwo);
    sum                     = _mm256_srai_epi16(sum, 6);
    const __m256i zero      = _mm256_setzero_si256();
    __m256i       row       = _mm256_shuffle_epi8(sum, zero);
    row_store_64xh(&row, 64, dst, stride);
}
void svt_aom_dc_top_predictor_32x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i sum = dc_sum_32(above);
    (void)left;

    const __m256i sixteen = _mm256_set1_epi16(16);
    sum                   = _mm256_add_epi16(sum, sixteen);
    sum                   = _mm256_srai_epi16(sum, 5);
    const __m256i zero    = _mm256_setzero_si256();
    __m256i       row     = _mm256_shuffle_epi8(sum, zero);
    row_store_32xh(&row, 32, dst, stride);
}
void svt_aom_dc_left_predictor_32x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i sum = dc_sum_32(left);
    (void)above;

    const __m256i sixteen = _mm256_set1_epi16(16);
    sum                   = _mm256_add_epi16(sum, sixteen);
    sum                   = _mm256_srai_epi16(sum, 5);
    const __m256i zero    = _mm256_setzero_si256();
    __m256i       row     = _mm256_shuffle_epi8(sum, zero);
    row_store_32xh(&row, 32, dst, stride);
}
void svt_aom_dc_128_predictor_64x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    (void)above;
    (void)left;
    const __m256i row = _mm256_set1_epi8((uint8_t)0x80);
    row_store_64xh(&row, 64, dst, stride);
}
void svt_aom_dc_128_predictor_32x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    (void)above;
    (void)left;
    const __m256i row = _mm256_set1_epi8((uint8_t)0x80);
    row_store_32xh(&row, 32, dst, stride);
}

void svt_aom_dc_predictor_32x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m128i top_sum  = dc_sum_32_sse2(above);
    __m128i       left_sum = dc_sum_16_sse2(left);
    left_sum               = _mm_add_epi16(top_sum, left_sum);
    uint32_t sum           = _mm_cvtsi128_si32(left_sum);
    sum += 24;
    sum /= 48;
    const __m256i row = _mm256_set1_epi8((uint8_t)sum);
    row_store_32xh(&row, 16, dst, stride);
}

void svt_aom_dc_predictor_32x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i sum_above = dc_sum_32(above);
    __m256i       sum_left  = dc_sum_64(left);
    sum_left                = _mm256_add_epi16(sum_left, sum_above);
    uint32_t sum            = _mm_cvtsi128_si32(_mm256_castsi256_si128(sum_left));
    sum += 48;
    sum /= 96;
    const __m256i row = _mm256_set1_epi8((uint8_t)sum);
    row_store_32xh(&row, 64, dst, stride);
}

void svt_aom_dc_predictor_64x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i sum_above = dc_sum_64(above);
    __m256i       sum_left  = dc_sum_32(left);
    sum_left                = _mm256_add_epi16(sum_left, sum_above);
    uint32_t sum            = _mm_cvtsi128_si32(_mm256_castsi256_si128(sum_left));
    sum += 48;
    sum /= 96;
    const __m256i row = _mm256_set1_epi8((uint8_t)sum);
    row_store_64xh(&row, 32, dst, stride);
}

void svt_aom_dc_predictor_64x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i sum_above = dc_sum_64(above);
    __m256i       sum_left  = _mm256_castsi128_si256(dc_sum_16_sse2(left));
    sum_left                = _mm256_add_epi16(sum_left, sum_above);
    uint32_t sum            = _mm_cvtsi128_si32(_mm256_castsi256_si128(sum_left));
    sum += 40;
    sum /= 80;
    const __m256i row = _mm256_set1_epi8((uint8_t)sum);
    row_store_64xh(&row, 16, dst, stride);
}

void svt_aom_dc_left_predictor_32x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m128i sum = dc_sum_16_sse2(left);
    (void)above;

    const __m128i eight = _mm_set1_epi16(8);
    sum                 = _mm_add_epi16(sum, eight);
    sum                 = _mm_srai_epi16(sum, 4);
    const __m128i zero  = _mm_setzero_si128();
    const __m128i r     = _mm_shuffle_epi8(sum, zero);
    const __m256i row   = _mm256_inserti128_si256(_mm256_castsi128_si256(r), r, 1);
    row_store_32xh(&row, 16, dst, stride);
}

void svt_aom_dc_left_predictor_32x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i sum = dc_sum_64(left);
    (void)above;

    const __m256i thirtytwo = _mm256_set1_epi16(32);
    sum                     = _mm256_add_epi16(sum, thirtytwo);
    sum                     = _mm256_srai_epi16(sum, 6);
    const __m256i zero      = _mm256_setzero_si256();
    __m256i       row       = _mm256_shuffle_epi8(sum, zero);
    row_store_32xh(&row, 64, dst, stride);
}

void svt_aom_dc_left_predictor_64x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i sum = dc_sum_32(left);
    (void)above;

    const __m256i sixteen = _mm256_set1_epi16(16);
    sum                   = _mm256_add_epi16(sum, sixteen);
    sum                   = _mm256_srai_epi16(sum, 5);
    const __m256i zero    = _mm256_setzero_si256();
    __m256i       row     = _mm256_shuffle_epi8(sum, zero);
    row_store_64xh(&row, 32, dst, stride);
}

void svt_aom_dc_left_predictor_64x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m128i sum = dc_sum_16_sse2(left);
    (void)above;

    const __m128i eight = _mm_set1_epi16(8);
    sum                 = _mm_add_epi16(sum, eight);
    sum                 = _mm_srai_epi16(sum, 4);
    const __m128i zero  = _mm_setzero_si128();
    const __m128i r     = _mm_shuffle_epi8(sum, zero);
    const __m256i row   = _mm256_inserti128_si256(_mm256_castsi128_si256(r), r, 1);
    row_store_64xh(&row, 16, dst, stride);
}

void svt_aom_dc_top_predictor_32x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i sum = dc_sum_32(above);
    (void)left;

    const __m256i sixteen = _mm256_set1_epi16(16);
    sum                   = _mm256_add_epi16(sum, sixteen);
    sum                   = _mm256_srai_epi16(sum, 5);
    const __m256i zero    = _mm256_setzero_si256();
    __m256i       row     = _mm256_shuffle_epi8(sum, zero);
    row_store_32xh(&row, 16, dst, stride);
}

void svt_aom_dc_top_predictor_32x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i sum = dc_sum_32(above);
    (void)left;

    const __m256i sixteen = _mm256_set1_epi16(16);
    sum                   = _mm256_add_epi16(sum, sixteen);
    sum                   = _mm256_srai_epi16(sum, 5);
    const __m256i zero    = _mm256_setzero_si256();
    __m256i       row     = _mm256_shuffle_epi8(sum, zero);
    row_store_32xh(&row, 64, dst, stride);
}

void svt_aom_dc_top_predictor_64x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i sum = dc_sum_64(above);
    (void)left;

    const __m256i thirtytwo = _mm256_set1_epi16(32);
    sum                     = _mm256_add_epi16(sum, thirtytwo);
    sum                     = _mm256_srai_epi16(sum, 6);
    const __m256i zero      = _mm256_setzero_si256();
    __m256i       row       = _mm256_shuffle_epi8(sum, zero);
    row_store_64xh(&row, 32, dst, stride);
}

void svt_aom_dc_top_predictor_64x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i sum = dc_sum_64(above);
    (void)left;

    const __m256i thirtytwo = _mm256_set1_epi16(32);
    sum                     = _mm256_add_epi16(sum, thirtytwo);
    sum                     = _mm256_srai_epi16(sum, 6);
    const __m256i zero      = _mm256_setzero_si256();
    __m256i       row       = _mm256_shuffle_epi8(sum, zero);
    row_store_64xh(&row, 16, dst, stride);
}

void svt_aom_dc_128_predictor_32x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    (void)above;
    (void)left;
    const __m256i row = _mm256_set1_epi8((uint8_t)0x80);
    row_store_32xh(&row, 16, dst, stride);
}
void svt_aom_dc_128_predictor_32x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    (void)above;
    (void)left;
    const __m256i row = _mm256_set1_epi8((uint8_t)0x80);
    row_store_32xh(&row, 64, dst, stride);
}
void svt_aom_dc_128_predictor_64x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    (void)above;
    (void)left;
    const __m256i row = _mm256_set1_epi8((uint8_t)0x80);
    row_store_64xh(&row, 16, dst, stride);
}
void svt_aom_dc_128_predictor_64x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    (void)above;
    (void)left;
    const __m256i row = _mm256_set1_epi8((uint8_t)0x80);
    row_store_64xh(&row, 32, dst, stride);
}

// There are 32 rows togeter. This function does line:
// 0,1,2,3, and 16,17,18,19. The next call would do
// 4,5,6,7, and 20,21,22,23. So 4 times of calling
// would finish 32 rows.
static INLINE void h_predictor_32x8line(const __m256i *row, uint8_t *dst, ptrdiff_t stride) {
    __m256i       t[4];
    __m256i       m   = _mm256_setzero_si256();
    const __m256i inc = _mm256_set1_epi8(4);
    int32_t       i;

    for (i = 0; i < 4; i++) {
        t[i]       = _mm256_shuffle_epi8(*row, m);
        __m256i r0 = _mm256_inserti128_si256(t[i], _mm256_castsi256_si128(t[i]), 1);
        __m256i r1 = _mm256_permute2x128_si256(t[i], t[i], 0x11);
        _mm256_storeu_si256((__m256i *)dst, r0);
        _mm256_storeu_si256((__m256i *)(dst + (stride << 4)), r1);
        dst += stride;
        m = _mm256_add_epi8(m, inc);
    }
}

void svt_aom_h_predictor_32x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    (void)above;
    const __m256i left_col = _mm256_loadu_si256((__m256i const *)left);

    __m256i u = _mm256_unpacklo_epi8(left_col, left_col);

    __m256i v = _mm256_unpacklo_epi8(u, u);
    h_predictor_32x8line(&v, dst, stride);
    dst += stride << 2;

    v = _mm256_unpackhi_epi8(u, u);
    h_predictor_32x8line(&v, dst, stride);
    dst += stride << 2;

    u = _mm256_unpackhi_epi8(left_col, left_col);

    v = _mm256_unpacklo_epi8(u, u);
    h_predictor_32x8line(&v, dst, stride);
    dst += stride << 2;

    v = _mm256_unpackhi_epi8(u, u);
    h_predictor_32x8line(&v, dst, stride);
}
static INLINE void row_store_32x2xh(const __m256i *r0, const __m256i *r1, int32_t height, uint8_t *dst,
                                    ptrdiff_t stride) {
    for (int32_t i = 0; i < height; ++i) {
        _mm256_storeu_si256((__m256i *)dst, *r0);
        _mm256_storeu_si256((__m256i *)(dst + 32), *r1);
        dst += stride;
    }
}
void svt_aom_v_predictor_64x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i row0 = _mm256_loadu_si256((const __m256i *)above);
    const __m256i row1 = _mm256_loadu_si256((const __m256i *)(above + 32));
    (void)left;
    row_store_32x2xh(&row0, &row1, 64, dst, stride);
}
void svt_aom_v_predictor_32x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i row = _mm256_loadu_si256((const __m256i *)above);
    (void)left;
    row_store_32xh(&row, 32, dst, stride);
}

void svt_aom_v_predictor_32x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i row = _mm256_loadu_si256((const __m256i *)above);
    (void)left;
    row_store_32xh(&row, 16, dst, stride);
}
void svt_aom_v_predictor_32x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i row = _mm256_loadu_si256((const __m256i *)above);
    (void)left;
    row_store_32xh(&row, 64, dst, stride);
}
void svt_aom_v_predictor_64x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i row0 = _mm256_loadu_si256((const __m256i *)above);
    const __m256i row1 = _mm256_loadu_si256((const __m256i *)(above + 32));
    (void)left;
    row_store_32x2xh(&row0, &row1, 16, dst, stride);
}
void svt_aom_v_predictor_64x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i row0 = _mm256_loadu_si256((const __m256i *)above);
    const __m256i row1 = _mm256_loadu_si256((const __m256i *)(above + 32));
    (void)left;
    row_store_32x2xh(&row0, &row1, 32, dst, stride);
}

void svt_aom_dc_predictor_32x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i sum_above = dc_sum_32(above);
    __m256i       sum_left  = dc_sum_32(left);
    sum_left                = _mm256_add_epi16(sum_left, sum_above);
    const __m256i thirtytwo = _mm256_set1_epi16(32);
    sum_left                = _mm256_add_epi16(sum_left, thirtytwo);
    sum_left                = _mm256_srai_epi16(sum_left, 6);
    const __m256i zero      = _mm256_setzero_si256();
    __m256i       row       = _mm256_shuffle_epi8(sum_left, zero);
    row_store_32xh(&row, 32, dst, stride);
}

// only define these intrinsics if immintrin.h doesn't have them
#if defined(_MSC_VER) && _MSC_VER < 1910
static inline int32_t _mm256_extract_epi32(__m256i a, const int32_t i) { return a.m256i_i32[i & 7]; }

static inline __m256i _mm256_insert_epi32(__m256i a, int32_t b, const int32_t i) {
    __m256i c          = a;
    c.m256i_i32[i & 7] = b;
    return c;
}
#endif

// Low bit depth functions

static DECLARE_ALIGNED(16, uint8_t, load_mask_x[16][16]) = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14},
    {0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13},
    {0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
    {0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
    {0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
    {0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static DECLARE_ALIGNED(32, int, load_mask_z2[8][8]) = {
    {-1, 0, 0, 0, 0, 0, 0, 0},
    {-1, -1, 0, 0, 0, 0, 0, 0},
    {-1, -1, -1, 0, 0, 0, 0, 0},
    {-1, -1, -1, -1, 0, 0, 0, 0},
    {-1, -1, -1, -1, -1, 0, 0, 0},
    {-1, -1, -1, -1, -1, -1, 0, 0},
    {-1, -1, -1, -1, -1, -1, -1, 0},
    {-1, -1, -1, -1, -1, -1, -1, -1},
};

static AOM_FORCE_INLINE void dr_prediction_z1_hxw_internal_avx2(int H, int W, __m128i *dst, const uint8_t *above,
                                                                int upsample_above, int dx) {
    const int frac_bits  = 6 - upsample_above;
    const int max_base_x = ((W + H) - 1) << upsample_above;

    assert(dx > 0);
    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be caluculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0, a1, a32, a16;
    __m256i diff, c3f;
    __m128i a_mbase_x;

    a16       = _mm256_set1_epi16(16);
    a_mbase_x = _mm_set1_epi8(above[max_base_x]);
    c3f       = _mm256_set1_epi16(0x3f);

    int x = dx;
    for (int r = 0; r < W; r++) {
        __m256i b, res, shift;
        __m128i res1, a0_128, a1_128;

        int base          = x >> frac_bits;
        int base_max_diff = (max_base_x - base) >> upsample_above;
        if (base_max_diff <= 0) {
            for (int i = r; i < W; ++i) {
                dst[i] = a_mbase_x; // save 4 values
            }
            return;
        }
        if (base_max_diff > H)
            base_max_diff = H;
        a0_128 = _mm_loadu_si128((__m128i *)(above + base));
        a1_128 = _mm_loadu_si128((__m128i *)(above + base + 1));

        if (upsample_above) {
            a0_128 = _mm_shuffle_epi8(a0_128, *(__m128i *)even_odd_mask_x[0]);
            a1_128 = _mm_srli_si128(a0_128, 8);

            shift = _mm256_srli_epi16(_mm256_and_si256(_mm256_slli_epi16(_mm256_set1_epi16(x), upsample_above), c3f),
                                      1);
        } else {
            shift = _mm256_srli_epi16(_mm256_and_si256(_mm256_set1_epi16(x), c3f), 1);
        }
        a0 = _mm256_cvtepu8_epi16(a0_128);
        a1 = _mm256_cvtepu8_epi16(a1_128);

        diff = _mm256_sub_epi16(a1, a0); // a[x+1] - a[x]
        a32  = _mm256_slli_epi16(a0, 5); // a[x] * 32
        a32  = _mm256_add_epi16(a32, a16); // a[x] * 32 + 16

        b   = _mm256_mullo_epi16(diff, shift);
        res = _mm256_add_epi16(a32, b);
        res = _mm256_srli_epi16(res, 5);

        res  = _mm256_packus_epi16(res, _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1))); // goto 8 bit
        res1 = _mm256_castsi256_si128(res); // 16 8bit values

        dst[r] = _mm_blendv_epi8(a_mbase_x, res1, *(__m128i *)base_mask[base_max_diff]);
        x += dx;
    }
}

static void dr_prediction_z1_4xn_avx2(int32_t N, uint8_t *dst, ptrdiff_t stride, const uint8_t *above,
                                      int32_t upsample_above, int32_t dx) {
    __m128i dstvec[16];
    dr_prediction_z1_hxw_internal_avx2(4, N, dstvec, above, upsample_above, dx);

    for (int32_t i = 0; i < N; i++) { *(uint32_t *)(dst + stride * i) = _mm_cvtsi128_si32(dstvec[i]); }
}

static void dr_prediction_z1_8xn_avx2(int32_t N, uint8_t *dst, ptrdiff_t stride, const uint8_t *above,
                                      int32_t upsample_above, int32_t dx) {
    __m128i dstvec[32];

    dr_prediction_z1_hxw_internal_avx2(8, N, dstvec, above, upsample_above, dx);
    for (int32_t i = 0; i < N; i++) { _mm_storel_epi64((__m128i *)(dst + stride * i), dstvec[i]); }
}

static void dr_prediction_z1_16xn_avx2(int32_t N, uint8_t *dst, ptrdiff_t stride, const uint8_t *above,
                                       int32_t upsample_above, int32_t dx) {
    __m128i dstvec[64];

    dr_prediction_z1_hxw_internal_avx2(16, N, dstvec, above, upsample_above, dx);

    for (int32_t i = 0; i < N; i++) { _mm_storeu_si128((__m128i *)(dst + stride * i), dstvec[i]); }
}

static AOM_FORCE_INLINE void dr_prediction_z1_32xn_internal_avx2(int32_t N, __m256i *dstvec, const uint8_t *above,
                                                                 int32_t upsample_above, int32_t dx) {
    int32_t x;
    // here upsample_above is 0 by design of av1_use_intra_edge_upsample
    (void)upsample_above;
    const int32_t frac_bits  = 6;
    const int32_t max_base_x = ((32 + N) - 1);

    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be calculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0, a1, a32, a16;
    __m256i a_mbase_x, diff, c3f;

    a16       = _mm256_set1_epi16(16);
    a_mbase_x = _mm256_set1_epi8(above[max_base_x]);
    c3f       = _mm256_set1_epi16(0x3f);

    x = dx;
    for (int32_t r = 0; r < N; r++) {
        __m256i b, res, res16[2];
        __m128i a0_128, a1_128;

        int32_t base          = x >> frac_bits;
        int32_t base_max_diff = (max_base_x - base);
        if (base_max_diff <= 0) {
            for (int32_t i = r; i < N; ++i) {
                dstvec[i] = a_mbase_x; // save 32 values
            }
            return;
        }
        if (base_max_diff > 32)
            base_max_diff = 32;
        __m256i shift = _mm256_srli_epi16(_mm256_and_si256(_mm256_set1_epi16(x), c3f), 1);

        for (int32_t j = 0, jj = 0; j < 32; j += 16, jj++) {
            int32_t mdiff = base_max_diff - j;
            if (mdiff <= 0) {
                res16[jj] = a_mbase_x;
            } else {
                a0_128 = _mm_loadu_si128((__m128i *)(above + base + j));
                a1_128 = _mm_loadu_si128((__m128i *)(above + base + j + 1));
                a0     = _mm256_cvtepu8_epi16(a0_128);
                a1     = _mm256_cvtepu8_epi16(a1_128);

                diff = _mm256_sub_epi16(a1, a0); // a[x+1] - a[x]
                a32  = _mm256_slli_epi16(a0, 5); // a[x] * 32
                a32  = _mm256_add_epi16(a32, a16); // a[x] * 32 + 16
                b    = _mm256_mullo_epi16(diff, shift);

                res       = _mm256_add_epi16(a32, b);
                res       = _mm256_srli_epi16(res, 5);
                res16[jj] = _mm256_packus_epi16(
                    res, _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1))); // 16 8bit values
            }
        }
        res16[1] = _mm256_inserti128_si256(res16[0], _mm256_castsi256_si128(res16[1]),
                                           1); // 32 8bit values

        dstvec[r] = _mm256_blendv_epi8(a_mbase_x, res16[1],
                                       *(__m256i *)base_mask[base_max_diff]); // 32 8bit values
        x += dx;
    }
}

static void dr_prediction_z1_32xn_avx2(int32_t N, uint8_t *dst, ptrdiff_t stride, const uint8_t *above,
                                       int32_t upsample_above, int32_t dx) {
    __m256i dstvec[64];
    dr_prediction_z1_32xn_internal_avx2(N, dstvec, above, upsample_above, dx);
    for (int32_t i = 0; i < N; i++) { _mm256_storeu_si256((__m256i *)(dst + stride * i), dstvec[i]); }
}

static void dr_prediction_z1_64xn_avx2(int32_t N, uint8_t *dst, ptrdiff_t stride, const uint8_t *above,
                                       int32_t upsample_above, int32_t dx) {
    int32_t x;

    // here upsample_above is 0 by design of av1_use_intra_edge_upsample
    (void)upsample_above;
    const int32_t frac_bits  = 6;
    const int32_t max_base_x = ((64 + N) - 1);

    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be calculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0, a1, a32, a16;
    __m256i a_mbase_x, diff, c3f;
    __m128i max_base_x128, base_inc128, mask128;

    a16           = _mm256_set1_epi16(16);
    a_mbase_x     = _mm256_set1_epi8(above[max_base_x]);
    max_base_x128 = _mm_set1_epi8(max_base_x);
    c3f           = _mm256_set1_epi16(0x3f);

    x = dx;
    for (int32_t r = 0; r < N; r++, dst += stride) {
        __m256i b, res;

        int32_t base = x >> frac_bits;
        if (base >= max_base_x) {
            for (int32_t i = r; i < N; ++i) {
                _mm256_storeu_si256((__m256i *)dst, a_mbase_x); // save 32 values
                _mm256_storeu_si256((__m256i *)(dst + 32), a_mbase_x);
                dst += stride;
            }
            return;
        }

        __m256i shift = _mm256_srli_epi16(_mm256_and_si256(_mm256_set1_epi16(x), c3f), 1);

        __m128i a0_128, a1_128, res128;
        for (int32_t j = 0; j < 64; j += 16) {
            int32_t mdif = max_base_x - (base + j);
            if (mdif <= 0) {
                _mm_storeu_si128((__m128i *)(dst + j), _mm256_castsi256_si128(a_mbase_x));
            } else {
                a0_128 = _mm_loadu_si128((__m128i *)(above + base + j));
                a1_128 = _mm_loadu_si128((__m128i *)(above + base + 1 + j));
                a0     = _mm256_cvtepu8_epi16(a0_128);
                a1     = _mm256_cvtepu8_epi16(a1_128);

                diff = _mm256_sub_epi16(a1, a0); // a[x+1] - a[x]
                a32  = _mm256_slli_epi16(a0, 5); // a[x] * 32
                a32  = _mm256_add_epi16(a32, a16); // a[x] * 32 + 16
                b    = _mm256_mullo_epi16(diff, shift);

                res = _mm256_add_epi16(a32, b);
                res = _mm256_srli_epi16(res, 5);
                res = _mm256_packus_epi16(res,
                                          _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1))); // 16 8bit values

                base_inc128 = _mm_setr_epi8((uint8_t)(base + j),
                                            (uint8_t)(base + j + 1),
                                            (uint8_t)(base + j + 2),
                                            (uint8_t)(base + j + 3),
                                            (uint8_t)(base + j + 4),
                                            (uint8_t)(base + j + 5),
                                            (uint8_t)(base + j + 6),
                                            (uint8_t)(base + j + 7),
                                            (uint8_t)(base + j + 8),
                                            (uint8_t)(base + j + 9),
                                            (uint8_t)(base + j + 10),
                                            (uint8_t)(base + j + 11),
                                            (uint8_t)(base + j + 12),
                                            (uint8_t)(base + j + 13),
                                            (uint8_t)(base + j + 14),
                                            (uint8_t)(base + j + 15));

                mask128 = _mm_cmpgt_epi8(_mm_subs_epu8(max_base_x128, base_inc128), _mm_setzero_si128());
                res128  = _mm_blendv_epi8(_mm256_castsi256_si128(a_mbase_x), _mm256_castsi256_si128(res), mask128);
                _mm_storeu_si128((__m128i *)(dst + j), res128);
            }
        }
        x += dx;
    }
}

// Directional prediction, zone 1: 0 < angle < 90
void svt_av1_dr_prediction_z1_avx2(uint8_t *dst, ptrdiff_t stride, int32_t bw, int32_t bh, const uint8_t *above,
                                   const uint8_t *left, int32_t upsample_above, int32_t dx, int32_t dy) {
    (void)left;
    (void)dy;
    switch (bw) {
    case 4: dr_prediction_z1_4xn_avx2(bh, dst, stride, above, upsample_above, dx); break;
    case 8: dr_prediction_z1_8xn_avx2(bh, dst, stride, above, upsample_above, dx); break;
    case 16: dr_prediction_z1_16xn_avx2(bh, dst, stride, above, upsample_above, dx); break;
    case 32: dr_prediction_z1_32xn_avx2(bh, dst, stride, above, upsample_above, dx); break;
    case 64: dr_prediction_z1_64xn_avx2(bh, dst, stride, above, upsample_above, dx); break;
    default: break;
    }
    return;
}

static AOM_FORCE_INLINE void highbd_dr_prediction_z1_4xn_internal_avx2(int32_t N, __m128i *dst, const uint16_t *above,
                                                                       int32_t upsample_above, int32_t dx) {
    const int32_t frac_bits  = 6 - upsample_above;
    const int32_t max_base_x = ((N + 4) - 1) << upsample_above;
    int32_t       x;
    // a assert(dx > 0);
    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be caluculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0, a1, a32, a16;
    __m256i diff;
    __m128i a_mbase_x, max_base_x128, base_inc128, mask128;

    a16           = _mm256_set1_epi32(16);
    a_mbase_x     = _mm_set1_epi16(above[max_base_x]);
    max_base_x128 = _mm_set1_epi32(max_base_x);

    x = dx;
    for (int32_t r = 0; r < N; r++) {
        __m256i b, res, shift;
        __m128i res1;

        int32_t base = x >> frac_bits;
        if (base >= max_base_x) {
            for (int32_t i = r; i < N; ++i) {
                dst[i] = a_mbase_x; // save 4 values
            }
            return;
        }

        a0 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base)));
        a1 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base + 1)));

        if (upsample_above) {
            a0          = _mm256_permutevar8x32_epi32(a0, _mm256_set_epi32(7, 5, 3, 1, 6, 4, 2, 0));
            a1          = _mm256_castsi128_si256(_mm256_extracti128_si256(a0, 1));
            base_inc128 = _mm_setr_epi32(base, base + 2, base + 4, base + 6);
            shift       = _mm256_srli_epi32(
                _mm256_and_si256(_mm256_slli_epi32(_mm256_set1_epi32(x), upsample_above), _mm256_set1_epi32(0x3f)), 1);
        } else {
            base_inc128 = _mm_setr_epi32(base, base + 1, base + 2, base + 3);
            shift       = _mm256_srli_epi32(_mm256_and_si256(_mm256_set1_epi32(x), _mm256_set1_epi32(0x3f)), 1);
        }

        diff = _mm256_sub_epi32(a1, a0); // a[x+1] - a[x]
        a32  = _mm256_slli_epi32(a0, 5); // a[x] * 32
        a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16

        b   = _mm256_mullo_epi32(diff, shift);
        res = _mm256_add_epi32(a32, b);
        res = _mm256_srli_epi32(res, 5);

        res1 = _mm256_castsi256_si128(res);
        res1 = _mm_packus_epi32(res1, res1);

        mask128 = _mm_cmpgt_epi32(max_base_x128, base_inc128);
        mask128 = _mm_packs_epi32(mask128, mask128); // goto 16 bit
        dst[r]  = _mm_blendv_epi8(a_mbase_x, res1, mask128);
        x += dx;
    }
}

static void highbd_dr_prediction_z1_4xn_avx2(int32_t N, uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                             int32_t upsample_above, int32_t dx) {
    __m128i dstvec[16];

    highbd_dr_prediction_z1_4xn_internal_avx2(N, dstvec, above, upsample_above, dx);
    for (int32_t i = 0; i < N; i++) { _mm_storel_epi64((__m128i *)(dst + stride * i), dstvec[i]); }
}

static AOM_FORCE_INLINE void highbd_dr_prediction_z1_8xn_internal_avx2(int32_t N, __m128i *dst, const uint16_t *above,
                                                                       int32_t upsample_above, int32_t dx) {
    const int32_t frac_bits  = 6 - upsample_above;
    const int32_t max_base_x = ((8 + N) - 1) << upsample_above;

    int32_t x;
    // a assert(dx > 0);
    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be caluculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0, a1, a0_1, a1_1, a32, a16;
    __m256i a_mbase_x, diff, max_base_x256, base_inc256, mask256;

    a16           = _mm256_set1_epi32(16);
    a_mbase_x     = _mm256_set1_epi16(above[max_base_x]);
    max_base_x256 = _mm256_set1_epi32(max_base_x);

    x = dx;
    for (int32_t r = 0; r < N; r++) {
        __m256i b, res, res1, shift;

        int32_t base = x >> frac_bits;
        if (base >= max_base_x) {
            for (int32_t i = r; i < N; ++i) {
                dst[i] = _mm256_castsi256_si128(a_mbase_x); // save 8 values
            }
            return;
        }

        a0 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base)));
        a1 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base + 1)));

        if (upsample_above) {
            a0 = _mm256_permutevar8x32_epi32(a0, _mm256_set_epi32(7, 5, 3, 1, 6, 4, 2, 0));
            a1 = _mm256_castsi128_si256(_mm256_extracti128_si256(a0, 1));

            a0_1 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base + 8)));
            a0_1 = _mm256_permutevar8x32_epi32(a0_1, _mm256_set_epi32(7, 5, 3, 1, 6, 4, 2, 0));
            a1_1 = _mm256_castsi128_si256(_mm256_extracti128_si256(a0_1, 1));

            a0          = _mm256_inserti128_si256(a0, _mm256_castsi256_si128(a0_1), 1);
            a1          = _mm256_inserti128_si256(a1, _mm256_castsi256_si128(a1_1), 1);
            base_inc256 = _mm256_setr_epi32(
                base, base + 2, base + 4, base + 6, base + 8, base + 10, base + 12, base + 14);
            shift = _mm256_srli_epi32(
                _mm256_and_si256(_mm256_slli_epi32(_mm256_set1_epi32(x), upsample_above), _mm256_set1_epi32(0x3f)), 1);
        } else {
            base_inc256 = _mm256_setr_epi32(base, base + 1, base + 2, base + 3, base + 4, base + 5, base + 6, base + 7);
            shift       = _mm256_srli_epi32(_mm256_and_si256(_mm256_set1_epi32(x), _mm256_set1_epi32(0x3f)), 1);
        }

        diff = _mm256_sub_epi32(a1, a0); // a[x+1] - a[x]
        a32  = _mm256_slli_epi32(a0, 5); // a[x] * 32
        a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16

        b   = _mm256_mullo_epi32(diff, shift);
        res = _mm256_add_epi32(a32, b);
        res = _mm256_srli_epi32(res, 5);

        res1 = _mm256_packus_epi32(res, _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1)));

        mask256 = _mm256_cmpgt_epi32(max_base_x256, base_inc256);
        mask256 = _mm256_packs_epi32(mask256,
                                     _mm256_castsi128_si256(_mm256_extracti128_si256(mask256, 1))); // goto 16 bit
        res1    = _mm256_blendv_epi8(a_mbase_x, res1, mask256);
        dst[r]  = _mm256_castsi256_si128(res1);
        x += dx;
    }
}

static void highbd_dr_prediction_z1_8xn_avx2(int32_t N, uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                             int32_t upsample_above, int32_t dx) {
    __m128i dstvec[32];

    highbd_dr_prediction_z1_8xn_internal_avx2(N, dstvec, above, upsample_above, dx);
    for (int32_t i = 0; i < N; i++) { _mm_storeu_si128((__m128i *)(dst + stride * i), dstvec[i]); }
}

static AOM_FORCE_INLINE void highbd_dr_prediction_z1_16xn_internal_avx2(int32_t N, __m256i *dstvec,
                                                                        const uint16_t *above, int32_t upsample_above,
                                                                        int32_t dx) {
    int32_t x;
    // here upsample_above is 0 by design of av1_use_intra_edge_upsample
    (void)upsample_above;
    const int32_t frac_bits  = 6;
    const int32_t max_base_x = ((16 + N) - 1);

    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be caluculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0, a0_1, a1, a1_1, a32, a16;
    __m256i a_mbase_x, diff, max_base_x256, base_inc256, mask256;

    a16           = _mm256_set1_epi32(16);
    a_mbase_x     = _mm256_set1_epi16(above[max_base_x]);
    max_base_x256 = _mm256_set1_epi16(max_base_x);

    x = dx;
    for (int32_t r = 0; r < N; r++) {
        __m256i b, res[2], res1;

        int32_t base = x >> frac_bits;
        if (base >= max_base_x) {
            for (int32_t i = r; i < N; ++i) {
                dstvec[i] = a_mbase_x; // save 16 values
            }
            return;
        }
        __m256i shift = _mm256_srli_epi32(_mm256_and_si256(_mm256_set1_epi32(x), _mm256_set1_epi32(0x3f)), 1);

        a0 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base)));
        a1 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base + 1)));

        diff = _mm256_sub_epi32(a1, a0); // a[x+1] - a[x]
        a32  = _mm256_slli_epi32(a0, 5); // a[x] * 32
        a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16
        b    = _mm256_mullo_epi32(diff, shift);

        res[0] = _mm256_add_epi32(a32, b);
        res[0] = _mm256_srli_epi32(res[0], 5);
        res[0] = _mm256_packus_epi32(res[0], _mm256_castsi128_si256(_mm256_extracti128_si256(res[0], 1)));

        int32_t mdif = max_base_x - base;
        if (mdif > 8) {
            a0_1 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base + 8)));
            a1_1 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base + 9)));

            diff = _mm256_sub_epi32(a1_1, a0_1); // a[x+1] - a[x]
            a32  = _mm256_slli_epi32(a0_1, 5); // a[x] * 32
            a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16
            b    = _mm256_mullo_epi32(diff, shift);

            res[1] = _mm256_add_epi32(a32, b);
            res[1] = _mm256_srli_epi32(res[1], 5);
            res[1] = _mm256_packus_epi32(res[1], _mm256_castsi128_si256(_mm256_extracti128_si256(res[1], 1)));
        } else {
            res[1] = a_mbase_x;
        }
        res1 = _mm256_inserti128_si256(res[0], _mm256_castsi256_si128(res[1]),
                                       1); // 16 16bit values

        base_inc256 = _mm256_setr_epi16(base,
                                        base + 1,
                                        base + 2,
                                        base + 3,
                                        base + 4,
                                        base + 5,
                                        base + 6,
                                        base + 7,
                                        base + 8,
                                        base + 9,
                                        base + 10,
                                        base + 11,
                                        base + 12,
                                        base + 13,
                                        base + 14,
                                        base + 15);
        mask256     = _mm256_cmpgt_epi16(max_base_x256, base_inc256);
        dstvec[r]   = _mm256_blendv_epi8(a_mbase_x, res1, mask256);
        x += dx;
    }
}

static void highbd_dr_prediction_z1_16xn_avx2(int32_t N, uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                              int32_t upsample_above, int32_t dx) {
    __m256i dstvec[64];
    highbd_dr_prediction_z1_16xn_internal_avx2(N, dstvec, above, upsample_above, dx);
    for (int32_t i = 0; i < N; i++) { _mm256_storeu_si256((__m256i *)(dst + stride * i), dstvec[i]); }
}

static AOM_FORCE_INLINE void highbd_dr_prediction_z1_32xn_internal_avx2(int32_t N, __m256i *dstvec,
                                                                        const uint16_t *above, int32_t upsample_above,
                                                                        int32_t dx) {
    int32_t x;
    // here upsample_above is 0 by design of av1_use_intra_edge_upsample
    (void)upsample_above;
    const int32_t frac_bits  = 6;
    const int32_t max_base_x = ((32 + N) - 1);

    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be caluculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0, a0_1, a1, a1_1, a32, a16;
    __m256i a_mbase_x, diff, max_base_x256, base_inc256, mask256;

    a16           = _mm256_set1_epi32(16);
    a_mbase_x     = _mm256_set1_epi16(above[max_base_x]);
    max_base_x256 = _mm256_set1_epi16(max_base_x);

    x = dx;
    for (int32_t r = 0; r < N; r++) {
        __m256i b, res[2], res1;

        int32_t base = x >> frac_bits;
        if (base >= max_base_x) {
            for (int32_t i = r; i < N; ++i) {
                dstvec[i]     = a_mbase_x; // save 32 values
                dstvec[i + N] = a_mbase_x;
            }
            return;
        }

        __m256i shift = _mm256_srli_epi32(_mm256_and_si256(_mm256_set1_epi32(x), _mm256_set1_epi32(0x3f)), 1);

        for (int32_t j = 0; j < 32; j += 16) {
            int32_t mdif = max_base_x - (base + j);
            if (mdif <= 0) {
                res1 = a_mbase_x;
            } else {
                a0 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base + j)));
                a1 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base + 1 + j)));

                diff = _mm256_sub_epi32(a1, a0); // a[x+1] - a[x]
                a32  = _mm256_slli_epi32(a0, 5); // a[x] * 32
                a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16
                b    = _mm256_mullo_epi32(diff, shift);

                res[0] = _mm256_add_epi32(a32, b);
                res[0] = _mm256_srli_epi32(res[0], 5);
                res[0] = _mm256_packus_epi32(res[0], _mm256_castsi128_si256(_mm256_extracti128_si256(res[0], 1)));
                if (mdif > 8) {
                    a0_1 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base + 8 + j)));
                    a1_1 = _mm256_cvtepu16_epi32(_mm_loadu_si128((__m128i *)(above + base + 9 + j)));

                    diff = _mm256_sub_epi32(a1_1, a0_1); // a[x+1] - a[x]
                    a32  = _mm256_slli_epi32(a0_1, 5); // a[x] * 32
                    a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16
                    b    = _mm256_mullo_epi32(diff, shift);

                    res[1] = _mm256_add_epi32(a32, b);
                    res[1] = _mm256_srli_epi32(res[1], 5);
                    res[1] = _mm256_packus_epi32(res[1], _mm256_castsi128_si256(_mm256_extracti128_si256(res[1], 1)));
                } else {
                    res[1] = a_mbase_x;
                }
                res1        = _mm256_inserti128_si256(res[0], _mm256_castsi256_si128(res[1]),
                                               1); // 16 16bit values
                base_inc256 = _mm256_setr_epi16(base + j,
                                                base + j + 1,
                                                base + j + 2,
                                                base + j + 3,
                                                base + j + 4,
                                                base + j + 5,
                                                base + j + 6,
                                                base + j + 7,
                                                base + j + 8,
                                                base + j + 9,
                                                base + j + 10,
                                                base + j + 11,
                                                base + j + 12,
                                                base + j + 13,
                                                base + j + 14,
                                                base + j + 15);

                mask256 = _mm256_cmpgt_epi16(max_base_x256, base_inc256);
                res1    = _mm256_blendv_epi8(a_mbase_x, res1, mask256);
            }
            if (!j)
                dstvec[r] = res1;
            else
                dstvec[r + N] = res1;
        }
        x += dx;
    }
}

static void highbd_dr_prediction_z1_32xn_avx2(int32_t N, uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                              int32_t upsample_above, int32_t dx) {
    __m256i dstvec[128];

    highbd_dr_prediction_z1_32xn_internal_avx2(N, dstvec, above, upsample_above, dx);
    for (int32_t i = 0; i < N; i++) {
        _mm256_storeu_si256((__m256i *)(dst + stride * i), dstvec[i]);
        _mm256_storeu_si256((__m256i *)(dst + stride * i + 16), dstvec[i + N]);
    }
}

static void highbd_dr_prediction_z1_64xn_avx2(int32_t N, uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                              int32_t upsample_above, int32_t dx) {
    int32_t x;

    // here upsample_above is 0 by design of av1_use_intra_edge_upsample
    (void)upsample_above;
    const int32_t frac_bits  = 6;
    const int32_t max_base_x = ((64 + N) - 1);

    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be caluculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0, a0_1, a1, a1_1, a32, a16;
    __m256i a_mbase_x, diff, max_base_x256, base_inc256, mask256;

    a16           = _mm256_set1_epi32(16);
    a_mbase_x     = _mm256_set1_epi16(above[max_base_x]);
    max_base_x256 = _mm256_set1_epi16(max_base_x);

    x = dx;
    for (int32_t r = 0; r < N; r++, dst += stride) {
        __m256i b, res[2], res1;

        int32_t base = x >> frac_bits;
        if (base >= max_base_x) {
            for (int32_t i = r; i < N; ++i) {
                _mm256_storeu_si256((__m256i *)dst, a_mbase_x); // save 32 values
                _mm256_storeu_si256((__m256i *)(dst + 16), a_mbase_x);
                _mm256_storeu_si256((__m256i *)(dst + 32), a_mbase_x);
                _mm256_storeu_si256((__m256i *)(dst + 48), a_mbase_x);
                dst += stride;
            }
            return;
        }

        __m256i shift = _mm256_srli_epi32(_mm256_and_si256(_mm256_set1_epi32(x), _mm256_set1_epi32(0x3f)), 1);

        __m128i a0_128, a0_1_128, a1_128, a1_1_128;
        for (int32_t j = 0; j < 64; j += 16) {
            int32_t mdif = max_base_x - (base + j);
            if (mdif <= 0) {
                _mm256_storeu_si256((__m256i *)(dst + j), a_mbase_x);
            } else {
                a0_128 = _mm_loadu_si128((__m128i *)(above + base + j));
                a1_128 = _mm_loadu_si128((__m128i *)(above + base + 1 + j));
                a0     = _mm256_cvtepu16_epi32(a0_128);
                a1     = _mm256_cvtepu16_epi32(a1_128);

                diff = _mm256_sub_epi32(a1, a0); // a[x+1] - a[x]
                a32  = _mm256_slli_epi32(a0, 5); // a[x] * 32
                a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16
                b    = _mm256_mullo_epi32(diff, shift);

                res[0] = _mm256_add_epi32(a32, b);
                res[0] = _mm256_srli_epi32(res[0], 5);
                res[0] = _mm256_packus_epi32(res[0], _mm256_castsi128_si256(_mm256_extracti128_si256(res[0], 1)));
                if (mdif > 8) {
                    a0_1_128 = _mm_loadu_si128((__m128i *)(above + base + 8 + j));
                    a1_1_128 = _mm_loadu_si128((__m128i *)(above + base + 9 + j));
                    a0_1     = _mm256_cvtepu16_epi32(a0_1_128);
                    a1_1     = _mm256_cvtepu16_epi32(a1_1_128);

                    diff = _mm256_sub_epi32(a1_1, a0_1); // a[x+1] - a[x]
                    a32  = _mm256_slli_epi32(a0_1, 5); // a[x] * 32
                    a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16
                    b    = _mm256_mullo_epi32(diff, shift);

                    res[1] = _mm256_add_epi32(a32, b);
                    res[1] = _mm256_srli_epi32(res[1], 5);
                    res[1] = _mm256_packus_epi32(res[1], _mm256_castsi128_si256(_mm256_extracti128_si256(res[1], 1)));
                } else {
                    res[1] = a_mbase_x;
                }
                res1        = _mm256_inserti128_si256(res[0], _mm256_castsi256_si128(res[1]),
                                               1); // 16 16bit values
                base_inc256 = _mm256_setr_epi16(base + j,
                                                base + j + 1,
                                                base + j + 2,
                                                base + j + 3,
                                                base + j + 4,
                                                base + j + 5,
                                                base + j + 6,
                                                base + j + 7,
                                                base + j + 8,
                                                base + j + 9,
                                                base + j + 10,
                                                base + j + 11,
                                                base + j + 12,
                                                base + j + 13,
                                                base + j + 14,
                                                base + j + 15);

                mask256 = _mm256_cmpgt_epi16(max_base_x256, base_inc256);
                res1    = _mm256_blendv_epi8(a_mbase_x, res1, mask256);
                _mm256_storeu_si256((__m256i *)(dst + j), res1);
            }
        }
        x += dx;
    }
}

// Directional prediction, zone 1: 0 < angle < 90
void svt_av1_highbd_dr_prediction_z1_avx2(uint16_t *dst, ptrdiff_t stride, int32_t bw, int32_t bh,
                                          const uint16_t *above, const uint16_t *left, int32_t upsample_above,
                                          int32_t dx, int32_t dy, int32_t bd) {
    (void)left;
    (void)dy;
    (void)bd;

    switch (bw) {
    case 4: highbd_dr_prediction_z1_4xn_avx2(bh, dst, stride, above, upsample_above, dx); break;
    case 8: highbd_dr_prediction_z1_8xn_avx2(bh, dst, stride, above, upsample_above, dx); break;
    case 16: highbd_dr_prediction_z1_16xn_avx2(bh, dst, stride, above, upsample_above, dx); break;
    case 32: highbd_dr_prediction_z1_32xn_avx2(bh, dst, stride, above, upsample_above, dx); break;
    case 64: highbd_dr_prediction_z1_64xn_avx2(bh, dst, stride, above, upsample_above, dx); break;
    default: break;
    }
    return;
}

static void dr_prediction_z2_nx4_avx2(int32_t N, uint8_t *dst, ptrdiff_t stride, const uint8_t *above,
                                      const uint8_t *left, int32_t upsample_above, int32_t upsample_left, int32_t dx,
                                      int32_t dy) {
    const int32_t min_base_x  = -(1 << upsample_above);
    const int32_t min_base_y  = -(1 << upsample_left);
    const int32_t frac_bits_x = 6 - upsample_above;
    const int32_t frac_bits_y = 6 - upsample_left;

    assert(dx > 0);
    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be calculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m128i a0_x, a1_x, a32, a16, diff;
    __m128i c3f, min_base_y128, c1234, dy128;

    a16           = _mm_set1_epi16(16);
    c3f           = _mm_set1_epi16(0x3f);
    min_base_y128 = _mm_set1_epi16(min_base_y);
    c1234         = _mm_setr_epi16(0, 1, 2, 3, 4, 0, 0, 0);
    dy128         = _mm_set1_epi16(dy);

    for (int r = 0; r < N; r++) {
        __m128i b, res, shift, r6, ydx;
        __m128i resx, resy, resxy;
        __m128i a0_x128, a1_x128;
        int     y          = r + 1;
        int     base_x     = (-y * dx) >> frac_bits_x;
        int     base_shift = 0;
        if (base_x < (min_base_x - 1)) {
            base_shift = (min_base_x - base_x - 1) >> upsample_above;
        }
        int base_min_diff = (min_base_x - base_x + upsample_above) >> upsample_above;
        if (base_min_diff > 4) {
            base_min_diff = 4;
        } else {
            if (base_min_diff < 0)
                base_min_diff = 0;
        }

        if (base_shift > 3) {
            a0_x  = _mm_setzero_si128();
            a1_x  = _mm_setzero_si128();
            shift = _mm_setzero_si128();
        } else {
            a0_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift));
            ydx     = _mm_set1_epi16(y * dx);
            r6      = _mm_slli_epi16(c1234, 6);

            if (upsample_above) {
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)even_odd_mask_x[base_shift]);
                a1_x128 = _mm_srli_si128(a0_x128, 8);

                shift = _mm_srli_epi16(_mm_and_si128(_mm_slli_epi16(_mm_sub_epi16(r6, ydx), upsample_above), c3f), 1);
            } else {
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)load_mask_x[base_shift]);
                a1_x128 = _mm_srli_si128(a0_x128, 1);

                shift = _mm_srli_epi16(_mm_and_si128(_mm_sub_epi16(r6, ydx), c3f), 1);
            }
            a0_x = _mm_cvtepu8_epi16(a0_x128);
            a1_x = _mm_cvtepu8_epi16(a1_x128);
        }
        // y calc
        __m128i a0_y, a1_y, shifty;
        if (base_x < min_base_x) {
            DECLARE_ALIGNED(32, int16_t, base_y_c[8]);
            __m128i y_c128, base_y_c128, mask128, c1234_;
            c1234_      = _mm_srli_si128(c1234, 2);
            r6          = _mm_set1_epi16(r << 6);
            y_c128      = _mm_sub_epi16(r6, _mm_mullo_epi16(c1234_, dy128));
            base_y_c128 = _mm_srai_epi16(y_c128, frac_bits_y);
            mask128     = _mm_cmpgt_epi16(min_base_y128, base_y_c128);
            base_y_c128 = _mm_andnot_si128(mask128, base_y_c128);
            _mm_storeu_si128((__m128i *)base_y_c, base_y_c128);

            a0_y = _mm_setr_epi16(
                left[base_y_c[0]], left[base_y_c[1]], left[base_y_c[2]], left[base_y_c[3]], 0, 0, 0, 0);
            base_y_c128 = _mm_add_epi16(base_y_c128, _mm_srli_epi16(a16, 4));
            _mm_storeu_si128((__m128i *)base_y_c, base_y_c128);
            a1_y = _mm_setr_epi16(
                left[base_y_c[0]], left[base_y_c[1]], left[base_y_c[2]], left[base_y_c[3]], 0, 0, 0, 0);

            if (upsample_left) {
                shifty = _mm_srli_epi16(_mm_and_si128(_mm_slli_epi16(y_c128, upsample_left), c3f), 1);
            } else {
                shifty = _mm_srli_epi16(_mm_and_si128(y_c128, c3f), 1);
            }
            a0_x  = _mm_unpacklo_epi64(a0_x, a0_y);
            a1_x  = _mm_unpacklo_epi64(a1_x, a1_y);
            shift = _mm_unpacklo_epi64(shift, shifty);
        }

        diff = _mm_sub_epi16(a1_x, a0_x); // a[x+1] - a[x]
        a32  = _mm_slli_epi16(a0_x, 5); // a[x] * 32
        a32  = _mm_add_epi16(a32, a16); // a[x] * 32 + 16

        b   = _mm_mullo_epi16(diff, shift);
        res = _mm_add_epi16(a32, b);
        res = _mm_srli_epi16(res, 5);

        resx = _mm_packus_epi16(res, res);
        resy = _mm_srli_si128(resx, 4);

        resxy              = _mm_blendv_epi8(resx, resy, *(__m128i *)base_mask[base_min_diff]);
        *(uint32_t *)(dst) = _mm_cvtsi128_si32(resxy);
        dst += stride;
    }
}

static void dr_prediction_z2_nx8_avx2(int32_t N, uint8_t *dst, ptrdiff_t stride, const uint8_t *above,
                                      const uint8_t *left, int32_t upsample_above, int32_t upsample_left, int32_t dx,
                                      int32_t dy) {
    const int32_t min_base_x  = -(1 << upsample_above);
    const int32_t min_base_y  = -(1 << upsample_left);
    const int32_t frac_bits_x = 6 - upsample_above;
    const int32_t frac_bits_y = 6 - upsample_left;

    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be calculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i diff, a32, a16;
    __m256i a0_x, a1_x;
    __m128i a0_x128, a1_x128, min_base_y128, c3f;
    __m128i c1234, dy128;

    a16           = _mm256_set1_epi16(16);
    c3f           = _mm_set1_epi16(0x3f);
    min_base_y128 = _mm_set1_epi16(min_base_y);
    dy128         = _mm_set1_epi16(dy);
    c1234         = _mm_setr_epi16(1, 2, 3, 4, 5, 6, 7, 8);

    for (int r = 0; r < N; r++) {
        __m256i b, res, shift;
        __m128i resx, resy, resxy, r6, ydx;

        int32_t y          = r + 1;
        int32_t base_x     = (-y * dx) >> frac_bits_x;
        int32_t base_shift = 0;
        if (base_x < (min_base_x - 1)) {
            base_shift = (min_base_x - base_x - 1) >> upsample_above;
        }
        int32_t base_min_diff = (min_base_x - base_x + upsample_above) >> upsample_above;
        if (base_min_diff > 8) {
            base_min_diff = 8;
        } else {
            if (base_min_diff < 0)
                base_min_diff = 0;
        }

        if (base_shift > 7) {
            a0_x  = _mm256_setzero_si256();
            a1_x  = _mm256_setzero_si256();
            shift = _mm256_setzero_si256();
        } else {
            a0_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift));
            ydx     = _mm_set1_epi16(y * dx);
            r6      = _mm_slli_epi16(_mm_srli_si128(c1234, 2), 6);
            if (upsample_above) {
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)even_odd_mask_x[base_shift]);
                a1_x128 = _mm_srli_si128(a0_x128, 8);

                shift = _mm256_castsi128_si256(
                    _mm_srli_epi16(_mm_and_si128(_mm_slli_epi16(_mm_sub_epi16(r6, ydx), upsample_above), c3f), 1));
            } else {
                a1_x128 = _mm_srli_si128(a0_x128, 1);
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)load_mask_x[base_shift]);
                a1_x128 = _mm_shuffle_epi8(a1_x128, *(__m128i *)load_mask_x[base_shift]);

                shift = _mm256_castsi128_si256(_mm_srli_epi16(_mm_and_si128(_mm_sub_epi16(r6, ydx), c3f), 1));
            }
            a0_x = _mm256_castsi128_si256(_mm_cvtepu8_epi16(a0_x128));
            a1_x = _mm256_castsi128_si256(_mm_cvtepu8_epi16(a1_x128));
        }

        // y calc
        __m128i a0_y, a1_y, shifty;
        if (base_x < min_base_x) {
            DECLARE_ALIGNED(32, int16_t, base_y_c[16]);
            __m128i y_c128, base_y_c128, mask128;
            r6          = _mm_set1_epi16(r << 6);
            y_c128      = _mm_sub_epi16(r6, _mm_mullo_epi16(c1234, dy128));
            base_y_c128 = _mm_srai_epi16(y_c128, frac_bits_y);
            mask128     = _mm_cmpgt_epi16(min_base_y128, base_y_c128);
            base_y_c128 = _mm_andnot_si128(mask128, base_y_c128);
            _mm_storeu_si128((__m128i *)base_y_c, base_y_c128);

            a0_y        = _mm_setr_epi16(left[base_y_c[0]],
                                  left[base_y_c[1]],
                                  left[base_y_c[2]],
                                  left[base_y_c[3]],
                                  left[base_y_c[4]],
                                  left[base_y_c[5]],
                                  left[base_y_c[6]],
                                  left[base_y_c[7]]);
            base_y_c128 = _mm_add_epi16(base_y_c128, _mm_srli_epi16(_mm256_castsi256_si128(a16), 4));
            _mm_storeu_si128((__m128i *)base_y_c, base_y_c128);

            a1_y = _mm_setr_epi16(left[base_y_c[0]],
                                  left[base_y_c[1]],
                                  left[base_y_c[2]],
                                  left[base_y_c[3]],
                                  left[base_y_c[4]],
                                  left[base_y_c[5]],
                                  left[base_y_c[6]],
                                  left[base_y_c[7]]);

            if (upsample_left) {
                shifty = _mm_srli_epi16(_mm_and_si128(_mm_slli_epi16(y_c128, upsample_left), c3f), 1);
            } else {
                shifty = _mm_srli_epi16(_mm_and_si128(y_c128, c3f), 1);
            }

            a0_x  = _mm256_inserti128_si256(a0_x, a0_y, 1);
            a1_x  = _mm256_inserti128_si256(a1_x, a1_y, 1);
            shift = _mm256_inserti128_si256(shift, shifty, 1);
        }

        diff = _mm256_sub_epi16(a1_x, a0_x); // a[x+1] - a[x]
        a32  = _mm256_slli_epi16(a0_x, 5); // a[x] * 32
        a32  = _mm256_add_epi16(a32, a16); // a[x] * 32 + 16

        b   = _mm256_mullo_epi16(diff, shift);
        res = _mm256_add_epi16(a32, b);
        res = _mm256_srli_epi16(res, 5);

        resx = _mm_packus_epi16(_mm256_castsi256_si128(res), _mm256_castsi256_si128(res));
        resy = _mm256_extracti128_si256(res, 1);
        resy = _mm_packus_epi16(resy, resy);

        resxy = _mm_blendv_epi8(resx, resy, *(__m128i *)base_mask[base_min_diff]);
        _mm_storel_epi64((__m128i *)(dst), resxy);
        dst += stride;
    }
}

static void dr_prediction_z2_hxw_avx2(int32_t H, int32_t W, uint8_t *dst, ptrdiff_t stride, const uint8_t *above,
                                      const uint8_t *left, int32_t upsample_above, int32_t upsample_left, int32_t dx,
                                      int32_t dy) {
    // here upsample_above and upsample_left are 0 by design of
    // av1_use_intra_edge_upsample
    const int32_t min_base_x = -1;
    const int32_t min_base_y = -1;
    (void)upsample_above;
    (void)upsample_left;
    const int32_t frac_bits_x = 6;
    const int32_t frac_bits_y = 6;

    __m256i a0_x, a1_x, a0_y, a1_y, a32, a16, c1234, c0123;
    __m256i diff, min_base_y256, c3f, shifty, dy256, c1;
    __m128i a0_x128, a1_x128;

    DECLARE_ALIGNED(32, int16_t, base_y_c[16]);
    a16           = _mm256_set1_epi16(16);
    c1            = _mm256_srli_epi16(a16, 4);
    min_base_y256 = _mm256_set1_epi16(min_base_y);
    c3f           = _mm256_set1_epi16(0x3f);
    dy256         = _mm256_set1_epi16(dy);
    c0123         = _mm256_setr_epi16(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    c1234         = _mm256_add_epi16(c0123, c1);

    for (int r = 0; r < H; r++) {
        __m256i b, res, shift, j256, r6, ydx;
        __m128i resx, resy;
        __m128i resxy;
        int     y = r + 1;
        ydx       = _mm256_set1_epi16(y * dx);

        int base_x = (-y * dx) >> frac_bits_x;
        for (int j = 0; j < W; j += 16) {
            j256           = _mm256_set1_epi16(j);
            int base_shift = 0;
            if ((base_x + j) < (min_base_x - 1)) {
                base_shift = (min_base_x - (base_x + j) - 1);
            }
            int base_min_diff = (min_base_x - base_x - j);
            if (base_min_diff > 16) {
                base_min_diff = 16;
            } else {
                if (base_min_diff < 0)
                    base_min_diff = 0;
            }

            if (base_shift < 16) {
                a0_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift + j));
                a1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift + 1 + j));
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)load_mask_x[base_shift]);
                a1_x128 = _mm_shuffle_epi8(a1_x128, *(__m128i *)load_mask_x[base_shift]);

                a0_x = _mm256_cvtepu8_epi16(a0_x128);
                a1_x = _mm256_cvtepu8_epi16(a1_x128);

                r6    = _mm256_slli_epi16(_mm256_add_epi16(c0123, j256), 6);
                shift = _mm256_srli_epi16(_mm256_and_si256(_mm256_sub_epi16(r6, ydx), c3f), 1);

                diff = _mm256_sub_epi16(a1_x, a0_x); // a[x+1] - a[x]
                a32  = _mm256_slli_epi16(a0_x, 5); // a[x] * 32
                a32  = _mm256_add_epi16(a32, a16); // a[x] * 32 + 16

                b    = _mm256_mullo_epi16(diff, shift);
                res  = _mm256_add_epi16(a32, b);
                res  = _mm256_srli_epi16(res, 5); // 16 16-bit values
                resx = _mm256_castsi256_si128(
                    _mm256_packus_epi16(res, _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1))));
            } else {
                resx = _mm_setzero_si128();
            }

            // y calc
            if (base_x < min_base_x) {
                __m256i c256, y_c256, base_y_c256, mask256, mul16;
                r6     = _mm256_set1_epi16(r << 6);
                c256   = _mm256_add_epi16(j256, c1234);
                mul16  = _mm256_min_epu16(_mm256_mullo_epi16(c256, dy256), _mm256_srli_epi16(min_base_y256, 1));
                y_c256 = _mm256_sub_epi16(r6, mul16);

                base_y_c256         = _mm256_srai_epi16(y_c256, frac_bits_y);
                mask256             = _mm256_cmpgt_epi16(min_base_y256, base_y_c256);
                base_y_c256         = _mm256_blendv_epi8(base_y_c256, min_base_y256, mask256);
                int16_t min_y       = (int16_t)_mm_extract_epi16(_mm256_extracti128_si256(base_y_c256, 1), 7);
                int16_t max_y       = (int16_t)_mm_extract_epi16(_mm256_castsi256_si128(base_y_c256), 0);
                int16_t offset_diff = max_y - min_y;

                if (offset_diff < 16) {
                    __m256i min_y256 = _mm256_set1_epi16(min_y);

                    __m256i base_y_offset    = _mm256_sub_epi16(base_y_c256, min_y256);
                    __m128i base_y_offset128 = _mm_packs_epi16(_mm256_castsi256_si128(base_y_offset),
                                                               _mm256_extracti128_si256(base_y_offset, 1));

                    __m128i a0_y128 = _mm_maskload_epi32((int *)(left + min_y),
                                                         *(__m128i *)load_mask_z2[offset_diff / 4]);
                    __m128i a1_y128 = _mm_maskload_epi32((int *)(left + min_y + 1),
                                                         *(__m128i *)load_mask_z2[offset_diff / 4]);
                    a0_y128         = _mm_shuffle_epi8(a0_y128, base_y_offset128);
                    a1_y128         = _mm_shuffle_epi8(a1_y128, base_y_offset128);
                    a0_y            = _mm256_cvtepu8_epi16(a0_y128);
                    a1_y            = _mm256_cvtepu8_epi16(a1_y128);
                } else {
                    base_y_c256 = _mm256_andnot_si256(mask256, base_y_c256);
                    _mm256_store_si256((__m256i *)base_y_c, base_y_c256);

                    a0_y        = _mm256_setr_epi16(left[base_y_c[0]],
                                             left[base_y_c[1]],
                                             left[base_y_c[2]],
                                             left[base_y_c[3]],
                                             left[base_y_c[4]],
                                             left[base_y_c[5]],
                                             left[base_y_c[6]],
                                             left[base_y_c[7]],
                                             left[base_y_c[8]],
                                             left[base_y_c[9]],
                                             left[base_y_c[10]],
                                             left[base_y_c[11]],
                                             left[base_y_c[12]],
                                             left[base_y_c[13]],
                                             left[base_y_c[14]],
                                             left[base_y_c[15]]);
                    base_y_c256 = _mm256_add_epi16(base_y_c256, c1);
                    _mm256_store_si256((__m256i *)base_y_c, base_y_c256);

                    a1_y = _mm256_setr_epi16(left[base_y_c[0]],
                                             left[base_y_c[1]],
                                             left[base_y_c[2]],
                                             left[base_y_c[3]],
                                             left[base_y_c[4]],
                                             left[base_y_c[5]],
                                             left[base_y_c[6]],
                                             left[base_y_c[7]],
                                             left[base_y_c[8]],
                                             left[base_y_c[9]],
                                             left[base_y_c[10]],
                                             left[base_y_c[11]],
                                             left[base_y_c[12]],
                                             left[base_y_c[13]],
                                             left[base_y_c[14]],
                                             left[base_y_c[15]]);
                }

                shifty = _mm256_srli_epi16(_mm256_and_si256(y_c256, c3f), 1);

                diff = _mm256_sub_epi16(a1_y, a0_y); // a[x+1] - a[x]
                a32  = _mm256_slli_epi16(a0_y, 5); // a[x] * 32
                a32  = _mm256_add_epi16(a32, a16); // a[x] * 32 + 16

                b    = _mm256_mullo_epi16(diff, shifty);
                res  = _mm256_add_epi16(a32, b);
                res  = _mm256_srli_epi16(res, 5); // 16 16-bit values
                resy = _mm256_castsi256_si128(
                    _mm256_packus_epi16(res, _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1))));
            } else {
                resy = _mm_setzero_si128();
            }
            resxy = _mm_blendv_epi8(resx, resy, *(__m128i *)base_mask[base_min_diff]);
            _mm_storeu_si128((__m128i *)(dst + j), resxy);
        } // for j
        dst += stride;
    }
}

// Directional prediction, zone 2: 90 < angle < 180
void svt_av1_dr_prediction_z2_avx2(uint8_t *dst, ptrdiff_t stride, int32_t bw, int32_t bh, const uint8_t *above,
                                   const uint8_t *left, int32_t upsample_above, int32_t upsample_left, int32_t dx,
                                   int32_t dy) {
    assert(dx > 0);
    assert(dy > 0);
    switch (bw) {
    case 4: dr_prediction_z2_nx4_avx2(bh, dst, stride, above, left, upsample_above, upsample_left, dx, dy); break;
    case 8: dr_prediction_z2_nx8_avx2(bh, dst, stride, above, left, upsample_above, upsample_left, dx, dy); break;
    default: dr_prediction_z2_hxw_avx2(bh, bw, dst, stride, above, left, upsample_above, upsample_left, dx, dy); break;
    }
    return;
}

// z3 functions
static INLINE void transpose4x16_sse2(__m128i *x, __m128i *d) {
    __m128i w0, w1, w2, w3, ww0, ww1, ww2, ww3;
    w0 = _mm_unpacklo_epi8(x[0], x[1]);
    w1 = _mm_unpacklo_epi8(x[2], x[3]);
    w2 = _mm_unpackhi_epi8(x[0], x[1]);
    w3 = _mm_unpackhi_epi8(x[2], x[3]);

    ww0 = _mm_unpacklo_epi16(w0, w1);
    ww1 = _mm_unpacklo_epi16(w2, w3);
    ww2 = _mm_unpackhi_epi16(w0, w1);
    ww3 = _mm_unpackhi_epi16(w2, w3);

    w0 = _mm_unpacklo_epi32(ww0, ww1);
    w2 = _mm_unpacklo_epi32(ww2, ww3);
    w1 = _mm_unpackhi_epi32(ww0, ww1);
    w3 = _mm_unpackhi_epi32(ww2, ww3);

    d[0] = _mm_unpacklo_epi64(w0, w2);
    d[1] = _mm_unpackhi_epi64(w0, w2);
    d[2] = _mm_unpacklo_epi64(w1, w3);
    d[3] = _mm_unpackhi_epi64(w1, w3);

    d[4] = _mm_srli_si128(d[0], 8);
    d[5] = _mm_srli_si128(d[1], 8);
    d[6] = _mm_srli_si128(d[2], 8);
    d[7] = _mm_srli_si128(d[3], 8);

    d[8]  = _mm_srli_si128(d[0], 4);
    d[9]  = _mm_srli_si128(d[1], 4);
    d[10] = _mm_srli_si128(d[2], 4);
    d[11] = _mm_srli_si128(d[3], 4);

    d[12] = _mm_srli_si128(d[0], 12);
    d[13] = _mm_srli_si128(d[1], 12);
    d[14] = _mm_srli_si128(d[2], 12);
    d[15] = _mm_srli_si128(d[3], 12);
}

static INLINE void transpose16x32_avx2(__m256i *x, __m256i *d) {
    __m256i w0, w1, w2, w3, w4, w5, w6, w7, w8, w9;
    __m256i w10, w11, w12, w13, w14, w15;

    w0 = _mm256_unpacklo_epi8(x[0], x[1]);
    w1 = _mm256_unpacklo_epi8(x[2], x[3]);
    w2 = _mm256_unpacklo_epi8(x[4], x[5]);
    w3 = _mm256_unpacklo_epi8(x[6], x[7]);

    w8  = _mm256_unpacklo_epi8(x[8], x[9]);
    w9  = _mm256_unpacklo_epi8(x[10], x[11]);
    w10 = _mm256_unpacklo_epi8(x[12], x[13]);
    w11 = _mm256_unpacklo_epi8(x[14], x[15]);

    w4  = _mm256_unpacklo_epi16(w0, w1);
    w5  = _mm256_unpacklo_epi16(w2, w3);
    w12 = _mm256_unpacklo_epi16(w8, w9);
    w13 = _mm256_unpacklo_epi16(w10, w11);

    w6  = _mm256_unpacklo_epi32(w4, w5);
    w7  = _mm256_unpackhi_epi32(w4, w5);
    w14 = _mm256_unpacklo_epi32(w12, w13);
    w15 = _mm256_unpackhi_epi32(w12, w13);

    // Store first 4-line result
    d[0] = _mm256_unpacklo_epi64(w6, w14);
    d[1] = _mm256_unpackhi_epi64(w6, w14);
    d[2] = _mm256_unpacklo_epi64(w7, w15);
    d[3] = _mm256_unpackhi_epi64(w7, w15);

    w4  = _mm256_unpackhi_epi16(w0, w1);
    w5  = _mm256_unpackhi_epi16(w2, w3);
    w12 = _mm256_unpackhi_epi16(w8, w9);
    w13 = _mm256_unpackhi_epi16(w10, w11);

    w6  = _mm256_unpacklo_epi32(w4, w5);
    w7  = _mm256_unpackhi_epi32(w4, w5);
    w14 = _mm256_unpacklo_epi32(w12, w13);
    w15 = _mm256_unpackhi_epi32(w12, w13);

    // Store second 4-line result
    d[4] = _mm256_unpacklo_epi64(w6, w14);
    d[5] = _mm256_unpackhi_epi64(w6, w14);
    d[6] = _mm256_unpacklo_epi64(w7, w15);
    d[7] = _mm256_unpackhi_epi64(w7, w15);

    // upper half
    w0 = _mm256_unpackhi_epi8(x[0], x[1]);
    w1 = _mm256_unpackhi_epi8(x[2], x[3]);
    w2 = _mm256_unpackhi_epi8(x[4], x[5]);
    w3 = _mm256_unpackhi_epi8(x[6], x[7]);

    w8  = _mm256_unpackhi_epi8(x[8], x[9]);
    w9  = _mm256_unpackhi_epi8(x[10], x[11]);
    w10 = _mm256_unpackhi_epi8(x[12], x[13]);
    w11 = _mm256_unpackhi_epi8(x[14], x[15]);

    w4  = _mm256_unpacklo_epi16(w0, w1);
    w5  = _mm256_unpacklo_epi16(w2, w3);
    w12 = _mm256_unpacklo_epi16(w8, w9);
    w13 = _mm256_unpacklo_epi16(w10, w11);

    w6  = _mm256_unpacklo_epi32(w4, w5);
    w7  = _mm256_unpackhi_epi32(w4, w5);
    w14 = _mm256_unpacklo_epi32(w12, w13);
    w15 = _mm256_unpackhi_epi32(w12, w13);

    // Store first 4-line result
    d[8]  = _mm256_unpacklo_epi64(w6, w14);
    d[9]  = _mm256_unpackhi_epi64(w6, w14);
    d[10] = _mm256_unpacklo_epi64(w7, w15);
    d[11] = _mm256_unpackhi_epi64(w7, w15);

    w4  = _mm256_unpackhi_epi16(w0, w1);
    w5  = _mm256_unpackhi_epi16(w2, w3);
    w12 = _mm256_unpackhi_epi16(w8, w9);
    w13 = _mm256_unpackhi_epi16(w10, w11);

    w6  = _mm256_unpacklo_epi32(w4, w5);
    w7  = _mm256_unpackhi_epi32(w4, w5);
    w14 = _mm256_unpacklo_epi32(w12, w13);
    w15 = _mm256_unpackhi_epi32(w12, w13);

    // Store second 4-line result
    d[12] = _mm256_unpacklo_epi64(w6, w14);
    d[13] = _mm256_unpackhi_epi64(w6, w14);
    d[14] = _mm256_unpacklo_epi64(w7, w15);
    d[15] = _mm256_unpackhi_epi64(w7, w15);
}

static void transpose_tx_16x16(const uint8_t *src, ptrdiff_t pitchSrc, uint8_t *dst, ptrdiff_t pitchDst) {
    __m128i r[16];
    __m128i d[16];
    for (int j = 0; j < 16; j++) { r[j] = _mm_loadu_si128((__m128i *)(src + j * pitchSrc)); }
    transpose16x16_sse2(r, d);
    for (int j = 0; j < 16; j++) { _mm_storeu_si128((__m128i *)(dst + j * pitchDst), d[j]); }
}

static void transpose(const uint8_t *src, ptrdiff_t pitchSrc, uint8_t *dst, ptrdiff_t pitchDst, int32_t width,
                      int32_t height) {
    for (int j = 0; j < height; j += 16)
        for (int i = 0; i < width; i += 16)
            transpose_tx_16x16(src + i * pitchSrc + j, pitchSrc, dst + j * pitchDst + i, pitchDst);
}

static void dr_prediction_z3_4x4_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                      int32_t dy) {
    __m128i dstvec[4], d[4];

    dr_prediction_z1_hxw_internal_avx2(4, 4, dstvec, left, upsample_left, dy);
    transpose4x8_8x4_low_sse2(&dstvec[0], &dstvec[1], &dstvec[2], &dstvec[3], &d[0], &d[1], &d[2], &d[3]);

    *(uint32_t *)(dst + stride * 0) = _mm_cvtsi128_si32(d[0]);
    *(uint32_t *)(dst + stride * 1) = _mm_cvtsi128_si32(d[1]);
    *(uint32_t *)(dst + stride * 2) = _mm_cvtsi128_si32(d[2]);
    *(uint32_t *)(dst + stride * 3) = _mm_cvtsi128_si32(d[3]);
    return;
}

static void dr_prediction_z3_8x8_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                      int32_t dy) {
    __m128i dstvec[8], d[8];

    dr_prediction_z1_hxw_internal_avx2(8, 8, dstvec, left, upsample_left, dy);
    transpose8x8_sse2(&dstvec[0],
                      &dstvec[1],
                      &dstvec[2],
                      &dstvec[3],
                      &dstvec[4],
                      &dstvec[5],
                      &dstvec[6],
                      &dstvec[7],
                      &d[0],
                      &d[1],
                      &d[2],
                      &d[3]);

    _mm_storel_epi64((__m128i *)(dst + 0 * stride), d[0]);
    _mm_storel_epi64((__m128i *)(dst + 1 * stride), _mm_srli_si128(d[0], 8));
    _mm_storel_epi64((__m128i *)(dst + 2 * stride), d[1]);
    _mm_storel_epi64((__m128i *)(dst + 3 * stride), _mm_srli_si128(d[1], 8));
    _mm_storel_epi64((__m128i *)(dst + 4 * stride), d[2]);
    _mm_storel_epi64((__m128i *)(dst + 5 * stride), _mm_srli_si128(d[2], 8));
    _mm_storel_epi64((__m128i *)(dst + 6 * stride), d[3]);
    _mm_storel_epi64((__m128i *)(dst + 7 * stride), _mm_srli_si128(d[3], 8));
}

static void dr_prediction_z3_4x8_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                      int32_t dy) {
    __m128i dstvec[4], d[8];

    dr_prediction_z1_hxw_internal_avx2(8, 4, dstvec, left, upsample_left, dy);

    transpose4x8_8x4_sse2(
        &dstvec[0], &dstvec[1], &dstvec[2], &dstvec[3], &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
    for (int32_t i = 0; i < 8; i++) { *(uint32_t *)(dst + stride * i) = _mm_cvtsi128_si32(d[i]); }
}

static void dr_prediction_z3_8x4_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                      int32_t dy) {
    __m128i dstvec[8], d[4];

    dr_prediction_z1_hxw_internal_avx2(4, 8, dstvec, left, upsample_left, dy);
    transpose8x8_low_sse2(&dstvec[0],
                          &dstvec[1],
                          &dstvec[2],
                          &dstvec[3],
                          &dstvec[4],
                          &dstvec[5],
                          &dstvec[6],
                          &dstvec[7],
                          &d[0],
                          &d[1],
                          &d[2],
                          &d[3]);
    _mm_storel_epi64((__m128i *)(dst + 0 * stride), d[0]);
    _mm_storel_epi64((__m128i *)(dst + 1 * stride), d[1]);
    _mm_storel_epi64((__m128i *)(dst + 2 * stride), d[2]);
    _mm_storel_epi64((__m128i *)(dst + 3 * stride), d[3]);
}

static void dr_prediction_z3_8x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                       int32_t dy) {
    __m128i dstvec[8], d[8];

    dr_prediction_z1_hxw_internal_avx2(16, 8, dstvec, left, upsample_left, dy);

    transpose8x16_16x8_sse2(dstvec,
                            dstvec + 1,
                            dstvec + 2,
                            dstvec + 3,
                            dstvec + 4,
                            dstvec + 5,
                            dstvec + 6,
                            dstvec + 7,
                            d,
                            d + 1,
                            d + 2,
                            d + 3,
                            d + 4,
                            d + 5,
                            d + 6,
                            d + 7);
    for (int32_t i = 0; i < 8; i++) {
        _mm_storel_epi64((__m128i *)(dst + i * stride), d[i]);
        _mm_storel_epi64((__m128i *)(dst + (i + 8) * stride), _mm_srli_si128(d[i], 8));
    }
}

static void dr_prediction_z3_16x8_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                       int32_t dy) {
    __m128i dstvec[16], d[16];

    dr_prediction_z1_hxw_internal_avx2(8, 16, dstvec, left, upsample_left, dy);

    transpose16x8_8x16_sse2(&dstvec[0],
                            &dstvec[1],
                            &dstvec[2],
                            &dstvec[3],
                            &dstvec[4],
                            &dstvec[5],
                            &dstvec[6],
                            &dstvec[7],
                            &dstvec[8],
                            &dstvec[9],
                            &dstvec[10],
                            &dstvec[11],
                            &dstvec[12],
                            &dstvec[13],
                            &dstvec[14],
                            &dstvec[15],
                            &d[0],
                            &d[1],
                            &d[2],
                            &d[3],
                            &d[4],
                            &d[5],
                            &d[6],
                            &d[7]);

    for (int32_t i = 0; i < 8; i++) { _mm_storeu_si128((__m128i *)(dst + i * stride), d[i]); }
}

static void dr_prediction_z3_4x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                       int32_t dy) {
    __m128i dstvec[4], d[16];

    dr_prediction_z1_hxw_internal_avx2(16, 4, dstvec, left, upsample_left, dy);

    transpose4x16_sse2(dstvec, d);
    for (int32_t i = 0; i < 16; i++) { *(uint32_t *)(dst + stride * i) = _mm_cvtsi128_si32(d[i]); }
}

static void dr_prediction_z3_16x4_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                       int32_t dy) {
    __m128i dstvec[16], d[8];
    dr_prediction_z1_hxw_internal_avx2(4, 16, dstvec, left, upsample_left, dy);

    for (int32_t i = 4; i < 8; i++) { d[i] = _mm_setzero_si128(); }
    transpose16x8_8x16_sse2(&dstvec[0],
                            &dstvec[1],
                            &dstvec[2],
                            &dstvec[3],
                            &dstvec[4],
                            &dstvec[5],
                            &dstvec[6],
                            &dstvec[7],
                            &dstvec[8],
                            &dstvec[9],
                            &dstvec[10],
                            &dstvec[11],
                            &dstvec[12],
                            &dstvec[13],
                            &dstvec[14],
                            &dstvec[15],
                            &d[0],
                            &d[1],
                            &d[2],
                            &d[3],
                            &d[4],
                            &d[5],
                            &d[6],
                            &d[7]);

    for (int32_t i = 0; i < 4; i++) { _mm_storeu_si128((__m128i *)(dst + i * stride), d[i]); }
}

static void dr_prediction_z3_8x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                       int32_t dy) {
    __m256i dstvec[16], d[16];

    dr_prediction_z1_32xn_internal_avx2(8, dstvec, left, upsample_left, dy);
    for (int32_t i = 8; i < 16; i++) { dstvec[i] = _mm256_setzero_si256(); }
    transpose16x32_avx2(dstvec, d);

    for (int32_t i = 0; i < 16; i++) { _mm_storel_epi64((__m128i *)(dst + i * stride), _mm256_castsi256_si128(d[i])); }
    for (int32_t i = 0; i < 16; i++) {
        _mm_storel_epi64((__m128i *)(dst + (i + 16) * stride), _mm256_extracti128_si256(d[i], 1));
    }
}

static void dr_prediction_z3_32x8_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                       int32_t dy) {
    __m128i dstvec[32], d[16];

    dr_prediction_z1_hxw_internal_avx2(8, 32, dstvec, left, upsample_left, dy);

    transpose16x8_8x16_sse2(&dstvec[0],
                            &dstvec[1],
                            &dstvec[2],
                            &dstvec[3],
                            &dstvec[4],
                            &dstvec[5],
                            &dstvec[6],
                            &dstvec[7],
                            &dstvec[8],
                            &dstvec[9],
                            &dstvec[10],
                            &dstvec[11],
                            &dstvec[12],
                            &dstvec[13],
                            &dstvec[14],
                            &dstvec[15],
                            &d[0],
                            &d[1],
                            &d[2],
                            &d[3],
                            &d[4],
                            &d[5],
                            &d[6],
                            &d[7]);
    transpose16x8_8x16_sse2(&dstvec[0 + 16],
                            &dstvec[1 + 16],
                            &dstvec[2 + 16],
                            &dstvec[3 + 16],
                            &dstvec[4 + 16],
                            &dstvec[5 + 16],
                            &dstvec[6 + 16],
                            &dstvec[7 + 16],
                            &dstvec[8 + 16],
                            &dstvec[9 + 16],
                            &dstvec[10 + 16],
                            &dstvec[11 + 16],
                            &dstvec[12 + 16],
                            &dstvec[13 + 16],
                            &dstvec[14 + 16],
                            &dstvec[15 + 16],
                            &d[0 + 8],
                            &d[1 + 8],
                            &d[2 + 8],
                            &d[3 + 8],
                            &d[4 + 8],
                            &d[5 + 8],
                            &d[6 + 8],
                            &d[7 + 8]);

    for (int32_t i = 0; i < 8; i++) {
        _mm_storeu_si128((__m128i *)(dst + i * stride), d[i]);
        _mm_storeu_si128((__m128i *)(dst + i * stride + 16), d[i + 8]);
    }
}

static void dr_prediction_z3_16x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                        int32_t dy) {
    __m128i dstvec[16], d[16];

    dr_prediction_z1_hxw_internal_avx2(16, 16, dstvec, left, upsample_left, dy);
    transpose16x16_sse2(dstvec, d);

    for (int32_t i = 0; i < 16; i++) { _mm_storeu_si128((__m128i *)(dst + i * stride), d[i]); }
}

static void dr_prediction_z3_32x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                        int32_t dy) {
    __m256i dstvec[32], d[32];

    dr_prediction_z1_32xn_internal_avx2(32, dstvec, left, upsample_left, dy);
    transpose16x32_avx2(dstvec, d);
    transpose16x32_avx2(dstvec + 16, d + 16);
    for (int32_t j = 0; j < 16; j++) {
        _mm_storeu_si128((__m128i *)(dst + j * stride), _mm256_castsi256_si128(d[j]));
        _mm_storeu_si128((__m128i *)(dst + j * stride + 16), _mm256_castsi256_si128(d[j + 16]));
    }
    for (int32_t j = 0; j < 16; j++) {
        _mm_storeu_si128((__m128i *)(dst + (j + 16) * stride), _mm256_extracti128_si256(d[j], 1));
        _mm_storeu_si128((__m128i *)(dst + (j + 16) * stride + 16), _mm256_extracti128_si256(d[j + 16], 1));
    }
}

static void dr_prediction_z3_64x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                        int32_t dy) {
    DECLARE_ALIGNED(16, uint8_t, dst_t[64 * 64]);
    dr_prediction_z1_64xn_avx2(64, dst_t, 64, left, upsample_left, dy);
    transpose(dst_t, 64, dst, stride, 64, 64);
}

static void dr_prediction_z3_16x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                        int32_t dy) {
    __m256i dstvec[16], d[16];

    dr_prediction_z1_32xn_internal_avx2(16, dstvec, left, upsample_left, dy);
    transpose16x32_avx2(dstvec, d);
    // store
    for (int32_t j = 0; j < 16; j++) {
        _mm_storeu_si128((__m128i *)(dst + j * stride), _mm256_castsi256_si128(d[j]));
        _mm_storeu_si128((__m128i *)(dst + (j + 16) * stride), _mm256_extracti128_si256(d[j], 1));
    }
}

static void dr_prediction_z3_32x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                        int32_t dy) {
    __m128i dstvec[32], d[16];

    dr_prediction_z1_hxw_internal_avx2(16, 32, dstvec, left, upsample_left, dy);

    for (int32_t i = 0; i < 32; i += 16) {
        transpose16x16_sse2((dstvec + i), d);
        for (int32_t j = 0; j < 16; j++) { _mm_storeu_si128((__m128i *)(dst + j * stride + i), d[j]); }
    }
}

static void dr_prediction_z3_32x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                        int32_t dy) {
    EB_ALIGN(32) uint8_t dst_t[64 * 32];
    dr_prediction_z1_64xn_avx2(32, dst_t, 64, left, upsample_left, dy);
    transpose(dst_t, 64, dst, stride, 32, 64);
}

static void dr_prediction_z3_64x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                        int32_t dy) {
    EB_ALIGN(32) uint8_t dst_t[32 * 64];
    dr_prediction_z1_32xn_avx2(64, dst_t, 32, left, upsample_left, dy);
    transpose(dst_t, 32, dst, stride, 64, 32);
    return;
}

static void dr_prediction_z3_16x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                        int32_t dy) {
    EB_ALIGN(32) uint8_t dst_t[64 * 16];
    dr_prediction_z1_64xn_avx2(16, dst_t, 64, left, upsample_left, dy);
    transpose(dst_t, 64, dst, stride, 16, 64);
}

static void dr_prediction_z3_64x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *left, int32_t upsample_left,
                                        int32_t dy) {
    __m128i dstvec[64], d[16];

    dr_prediction_z1_hxw_internal_avx2(16, 64, dstvec, left, upsample_left, dy);

    for (int32_t i = 0; i < 64; i += 16) {
        transpose16x16_sse2((dstvec + i), d);
        for (int32_t j = 0; j < 16; j++) { _mm_storeu_si128((__m128i *)(dst + j * stride + i), d[j]); }
    }
}

void svt_av1_dr_prediction_z3_avx2(uint8_t *dst, ptrdiff_t stride, int32_t bw, int32_t bh, const uint8_t *above,
                                   const uint8_t *left, int32_t upsample_left, int32_t dx, int32_t dy) {
    (void)above;
    (void)dx;
    assert(dx == 1);
    assert(dy > 0);

    if (bw == bh) {
        switch (bw) {
        case 4: dr_prediction_z3_4x4_avx2(dst, stride, left, upsample_left, dy); break;
        case 8: dr_prediction_z3_8x8_avx2(dst, stride, left, upsample_left, dy); break;
        case 16: dr_prediction_z3_16x16_avx2(dst, stride, left, upsample_left, dy); break;
        case 32: dr_prediction_z3_32x32_avx2(dst, stride, left, upsample_left, dy); break;
        case 64: dr_prediction_z3_64x64_avx2(dst, stride, left, upsample_left, dy); break;
        }
    } else {
        if (bw < bh) {
            if (bw + bw == bh) {
                switch (bw) {
                case 4: dr_prediction_z3_4x8_avx2(dst, stride, left, upsample_left, dy); break;
                case 8: dr_prediction_z3_8x16_avx2(dst, stride, left, upsample_left, dy); break;
                case 16: dr_prediction_z3_16x32_avx2(dst, stride, left, upsample_left, dy); break;
                case 32: dr_prediction_z3_32x64_avx2(dst, stride, left, upsample_left, dy); break;
                }
            } else {
                switch (bw) {
                case 4: dr_prediction_z3_4x16_avx2(dst, stride, left, upsample_left, dy); break;
                case 8: dr_prediction_z3_8x32_avx2(dst, stride, left, upsample_left, dy); break;
                case 16: dr_prediction_z3_16x64_avx2(dst, stride, left, upsample_left, dy); break;
                }
            }
        } else {
            if (bh + bh == bw) {
                switch (bh) {
                case 4: dr_prediction_z3_8x4_avx2(dst, stride, left, upsample_left, dy); break;
                case 8: dr_prediction_z3_16x8_avx2(dst, stride, left, upsample_left, dy); break;
                case 16: dr_prediction_z3_32x16_avx2(dst, stride, left, upsample_left, dy); break;
                case 32: dr_prediction_z3_64x32_avx2(dst, stride, left, upsample_left, dy); break;
                }
            } else {
                switch (bh) {
                case 4: dr_prediction_z3_16x4_avx2(dst, stride, left, upsample_left, dy); break;
                case 8: dr_prediction_z3_32x8_avx2(dst, stride, left, upsample_left, dy); break;
                case 16: dr_prediction_z3_64x16_avx2(dst, stride, left, upsample_left, dy); break;
                }
            }
        }
    }
    return;
}

static DECLARE_ALIGNED(16, uint8_t, highbd_load_maskx[8][16]) = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13},
    {0, 1, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
    {0, 1, 0, 1, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 3, 4, 5, 6, 7},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 3, 4, 5},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 3},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1},
};

static DECLARE_ALIGNED(16, uint8_t, highbd_even_odd_maskx4[4][16]) = {
    {0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15},
    {0, 1, 2, 3, 6, 7, 10, 11, 14, 15, 4, 5, 8, 9, 12, 13},
    {0, 1, 0, 1, 4, 5, 8, 9, 12, 13, 0, 1, 6, 7, 10, 11},
    {0, 1, 0, 1, 0, 1, 6, 7, 10, 11, 14, 15, 0, 1, 8, 9}};

static DECLARE_ALIGNED(16, uint8_t, highbd_even_odd_maskx[8][32]) = {
    {0, 1, 4, 5, 8,  9,  12, 13, 16, 17, 20, 21, 24, 25, 28, 29,
     2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27, 30, 31},
    {0, 1, 2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27,
     0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29},
    {0, 1, 0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 0, 1, 0, 1, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27},
    {0, 1, 0, 1, 0, 1, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 0, 1, 0, 1, 0, 1, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25},
    {0, 1, 0, 1, 0, 1, 0, 1, 8, 9, 12, 13, 16, 17, 20, 21, 0, 1, 0, 1, 0, 1, 0, 1, 10, 11, 14, 15, 18, 19, 22, 23},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 10, 11, 14, 15, 18, 19, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 12, 13, 16, 17, 20, 21},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 12, 13, 16, 17, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 14, 15, 18, 19},
    {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 14, 15, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 16, 17}};

static DECLARE_ALIGNED(32, uint16_t, highbd_base_mask[17][16]) = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xffff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0, 0},
    {0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0, 0, 0},
    {0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0,
     0},
    {0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0},
    {0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff,
     0xffff}};

static void highbd_dr_prediction_z2_nx4_avx2(int32_t N, uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                             const uint16_t *left, int32_t upsample_above, int32_t upsample_left,
                                             int32_t dx, int32_t dy) {
    const int32_t min_base_x  = -(1 << upsample_above);
    const int32_t min_base_y  = -(1 << upsample_left);
    const int32_t frac_bits_x = 6 - upsample_above;
    const int32_t frac_bits_y = 6 - upsample_left;

    assert(dx > 0);
    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be calculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0_x, a1_x, a32, a16;
    __m256i diff;
    __m128i c3f, min_base_y128;

    a16           = _mm256_set1_epi16(16);
    c3f           = _mm_set1_epi16(0x3f);
    min_base_y128 = _mm_set1_epi16(min_base_y);

    for (int32_t r = 0; r < N; r++) {
        __m256i b, res, shift;
        __m128i resx, resy, resxy;
        __m128i a0_x128, a1_x128;
        int32_t y          = r + 1;
        int32_t base_x     = (-y * dx) >> frac_bits_x;
        int32_t base_shift = 0;
        if (base_x < (min_base_x - 1)) {
            base_shift = (min_base_x - base_x) >> upsample_above;
        }
        int32_t base_min_diff = (min_base_x - base_x + upsample_above) >> upsample_above;
        if (base_min_diff > 4) {
            base_min_diff = 4;
        } else {
            if (base_min_diff < 0)
                base_min_diff = 0;
        }

        if (base_shift > 3) {
            a0_x  = _mm256_setzero_si256();
            a1_x  = _mm256_setzero_si256();
            shift = _mm256_setzero_si256();
        } else {
            a0_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift));
            if (upsample_above) {
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)highbd_even_odd_maskx4[base_shift]);
                a1_x128 = _mm_srli_si128(a0_x128, 8);

                shift = _mm256_castsi128_si256(_mm_srli_epi16(
                    _mm_and_si128(_mm_slli_epi16(
                                      _mm_setr_epi16(
                                          -y * dx, (1 << 6) - y * dx, (2 << 6) - y * dx, (3 << 6) - y * dx, 0, 0, 0, 0),
                                      upsample_above),
                                  c3f),
                    1));
            } else {
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)highbd_load_maskx[base_shift]);
                a1_x128 = _mm_srli_si128(a0_x128, 2);

                shift = _mm256_castsi128_si256(_mm_srli_epi16(
                    _mm_and_si128(
                        _mm_setr_epi16(-y * dx, (1 << 6) - y * dx, (2 << 6) - y * dx, (3 << 6) - y * dx, 0, 0, 0, 0),
                        c3f),
                    1));
            }
            a0_x = _mm256_castsi128_si256(a0_x128);
            a1_x = _mm256_castsi128_si256(a1_x128);
        }
        // y calc
        __m128i a0_y, a1_y, shifty;
        if (base_x < min_base_x) {
            __m128i r6, c1234, dy128, y_c128, base_y_c128, mask128;
            DECLARE_ALIGNED(32, int16_t, base_y_c[8]);
            r6          = _mm_set1_epi16(r << 6);
            dy128       = _mm_set1_epi16(dy);
            c1234       = _mm_setr_epi16(1, 2, 3, 4, 0, 0, 0, 0);
            y_c128      = _mm_sub_epi16(r6, _mm_mullo_epi16(c1234, dy128));
            base_y_c128 = _mm_srai_epi16(y_c128, frac_bits_y);
            mask128     = _mm_cmpgt_epi16(min_base_y128, base_y_c128);
            base_y_c128 = _mm_andnot_si128(mask128, base_y_c128);
            _mm_storeu_si128((__m128i *)base_y_c, base_y_c128);

            a0_y = _mm_setr_epi16(
                left[base_y_c[0]], left[base_y_c[1]], left[base_y_c[2]], left[base_y_c[3]], 0, 0, 0, 0);
            a1_y = _mm_setr_epi16(
                left[base_y_c[0] + 1], left[base_y_c[1] + 1], left[base_y_c[2] + 1], left[base_y_c[3] + 1], 0, 0, 0, 0);

            if (upsample_left) {
                shifty = _mm_srli_epi16(_mm_and_si128(_mm_slli_epi16(y_c128, upsample_left), c3f), 1);
            } else {
                shifty = _mm_srli_epi16(_mm_and_si128(y_c128, c3f), 1);
            }
            a0_x  = _mm256_inserti128_si256(a0_x, a0_y, 1);
            a1_x  = _mm256_inserti128_si256(a1_x, a1_y, 1);
            shift = _mm256_inserti128_si256(shift, shifty, 1);
        }

        diff = _mm256_sub_epi16(a1_x, a0_x); // a[x+1] - a[x]
        a32  = _mm256_slli_epi16(a0_x, 5); // a[x] * 32
        a32  = _mm256_add_epi16(a32, a16); // a[x] * 32 + 16

        b   = _mm256_mullo_epi16(diff, shift);
        res = _mm256_add_epi16(a32, b);
        res = _mm256_srli_epi16(res, 5);

        resx  = _mm256_castsi256_si128(res);
        resy  = _mm256_extracti128_si256(res, 1);
        resxy = _mm_blendv_epi8(resx, resy, *(__m128i *)highbd_base_mask[base_min_diff]);
        _mm_storel_epi64((__m128i *)(dst), resxy);
        dst += stride;
    }
}

static void highbd_dr_prediction_z2_nx4_32bit_avx2(int N, uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                                   const uint16_t *left, int upsample_above, int upsample_left, int dx,
                                                   int dy) {
    const int min_base_x  = -(1 << upsample_above);
    const int min_base_y  = -(1 << upsample_left);
    const int frac_bits_x = 6 - upsample_above;
    const int frac_bits_y = 6 - upsample_left;

    assert(dx > 0);
    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be calculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0_x, a1_x, a32, a16;
    __m256i diff;
    __m128i c3f, min_base_y128;

    a16           = _mm256_set1_epi32(16);
    c3f           = _mm_set1_epi32(0x3f);
    min_base_y128 = _mm_set1_epi32(min_base_y);

    for (int r = 0; r < N; r++) {
        __m256i b, res, shift;
        __m128i resx, resy, resxy;
        __m128i a0_x128, a1_x128;
        int     y          = r + 1;
        int     base_x     = (-y * dx) >> frac_bits_x;
        int     base_shift = 0;
        if (base_x < (min_base_x - 1)) {
            base_shift = (min_base_x - base_x) >> upsample_above;
        }
        int base_min_diff = (min_base_x - base_x + upsample_above) >> upsample_above;
        if (base_min_diff > 4) {
            base_min_diff = 4;
        } else {
            if (base_min_diff < 0)
                base_min_diff = 0;
        }

        if (base_shift > 3) {
            a0_x  = _mm256_setzero_si256();
            a1_x  = _mm256_setzero_si256();
            shift = _mm256_setzero_si256();
        } else {
            a0_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift));
            if (upsample_above) {
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)highbd_even_odd_maskx4[base_shift]);
                a1_x128 = _mm_srli_si128(a0_x128, 8);

                shift = _mm256_castsi128_si256(_mm_srli_epi32(
                    _mm_and_si128(
                        _mm_slli_epi32(_mm_setr_epi32(-y * dx, (1 << 6) - y * dx, (2 << 6) - y * dx, (3 << 6) - y * dx),
                                       upsample_above),
                        c3f),
                    1));
            } else {
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)highbd_even_odd_maskx[base_shift]);
                a1_x128 = _mm_srli_si128(a0_x128, 2);

                shift = _mm256_castsi128_si256(_mm_srli_epi32(
                    _mm_and_si128(_mm_setr_epi32(-y * dx, (1 << 6) - y * dx, (2 << 6) - y * dx, (3 << 6) - y * dx),
                                  c3f),
                    1));
            }
            a0_x = _mm256_cvtepu16_epi32(a0_x128);
            a1_x = _mm256_cvtepu16_epi32(a1_x128);
        }
        // y calc
        __m128i a0_y, a1_y, shifty;
        if (base_x < min_base_x) {
            __m128i r6, c1234, dy128, y_c128, base_y_c128, mask128;
            DECLARE_ALIGNED(32, int, base_y_c[4]);
            r6          = _mm_set1_epi32(r << 6);
            dy128       = _mm_set1_epi32(dy);
            c1234       = _mm_setr_epi32(1, 2, 3, 4);
            y_c128      = _mm_sub_epi32(r6, _mm_mullo_epi32(c1234, dy128));
            base_y_c128 = _mm_srai_epi32(y_c128, frac_bits_y);
            mask128     = _mm_cmpgt_epi32(min_base_y128, base_y_c128);
            base_y_c128 = _mm_andnot_si128(mask128, base_y_c128);
            _mm_store_si128((__m128i *)base_y_c, base_y_c128);

            a0_y = _mm_setr_epi32(left[base_y_c[0]], left[base_y_c[1]], left[base_y_c[2]], left[base_y_c[3]]);
            a1_y = _mm_setr_epi32(
                left[base_y_c[0] + 1], left[base_y_c[1] + 1], left[base_y_c[2] + 1], left[base_y_c[3] + 1]);

            if (upsample_left) {
                shifty = _mm_srli_epi32(_mm_and_si128(_mm_slli_epi32(y_c128, upsample_left), c3f), 1);
            } else {
                shifty = _mm_srli_epi32(_mm_and_si128(y_c128, c3f), 1);
            }
            a0_x  = _mm256_inserti128_si256(a0_x, a0_y, 1);
            a1_x  = _mm256_inserti128_si256(a1_x, a1_y, 1);
            shift = _mm256_inserti128_si256(shift, shifty, 1);
        }

        diff = _mm256_sub_epi32(a1_x, a0_x); // a[x+1] - a[x]
        a32  = _mm256_slli_epi32(a0_x, 5); // a[x] * 32
        a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16

        b   = _mm256_mullo_epi32(diff, shift);
        res = _mm256_add_epi32(a32, b);
        res = _mm256_srli_epi32(res, 5);

        resx = _mm256_castsi256_si128(res);
        resx = _mm_packus_epi32(resx, resx);

        resy = _mm256_extracti128_si256(res, 1);
        resy = _mm_packus_epi32(resy, resy);

        resxy = _mm_blendv_epi8(resx, resy, *(__m128i *)highbd_base_mask[base_min_diff]);
        _mm_storel_epi64((__m128i *)(dst), resxy);
        dst += stride;
    }
}

static void highbd_dr_prediction_z2_nx8_avx2(int32_t N, uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                             const uint16_t *left, int32_t upsample_above, int32_t upsample_left,
                                             int32_t dx, int32_t dy) {
    const int min_base_x  = -(1 << upsample_above);
    const int min_base_y  = -(1 << upsample_left);
    const int frac_bits_x = 6 - upsample_above;
    const int frac_bits_y = 6 - upsample_left;

    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be calculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m128i c3f, min_base_y128;
    __m256i a0_x, a1_x, diff, a32, a16;
    __m128i a0_x128, a1_x128;

    a16           = _mm256_set1_epi16(16);
    c3f           = _mm_set1_epi16(0x3f);
    min_base_y128 = _mm_set1_epi16(min_base_y);

    for (int r = 0; r < N; r++) {
        __m256i b, res, shift;
        __m128i resx, resy, resxy;
        int     y          = r + 1;
        int     base_x     = (-y * dx) >> frac_bits_x;
        int     base_shift = 0;
        if (base_x < (min_base_x - 1)) {
            base_shift = (min_base_x - base_x) >> upsample_above;
        }
        int base_min_diff = (min_base_x - base_x + upsample_above) >> upsample_above;
        if (base_min_diff > 8) {
            base_min_diff = 8;
        } else {
            if (base_min_diff < 0)
                base_min_diff = 0;
        }

        if (base_shift > 7) {
            a0_x  = _mm256_setzero_si256();
            a1_x  = _mm256_setzero_si256();
            shift = _mm256_setzero_si256();
        } else {
            a0_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift));
            if (upsample_above) {
                __m128i mask, atmp0, atmp1, atmp2, atmp3;
                a1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + 8 + base_shift));
                atmp0   = _mm_shuffle_epi8(a0_x128, *(__m128i *)highbd_even_odd_maskx[base_shift]);
                atmp1   = _mm_shuffle_epi8(a1_x128, *(__m128i *)highbd_even_odd_maskx[base_shift]);
                atmp2   = _mm_shuffle_epi8(a0_x128, *(__m128i *)(highbd_even_odd_maskx[base_shift] + 16));
                atmp3   = _mm_shuffle_epi8(a1_x128, *(__m128i *)(highbd_even_odd_maskx[base_shift] + 16));
                mask    = _mm_cmpgt_epi8(*(__m128i *)highbd_even_odd_maskx[base_shift], _mm_set1_epi8(15));
                a0_x128 = _mm_blendv_epi8(atmp0, atmp1, mask);
                mask    = _mm_cmpgt_epi8(*(__m128i *)(highbd_even_odd_maskx[base_shift] + 16), _mm_set1_epi8(15));
                a1_x128 = _mm_blendv_epi8(atmp2, atmp3, mask);

                shift = _mm256_castsi128_si256(
                    _mm_srli_epi16(_mm_and_si128(_mm_slli_epi16(_mm_setr_epi16(-y * dx,
                                                                               (1 << 6) - y * dx,
                                                                               (2 << 6) - y * dx,
                                                                               (3 << 6) - y * dx,
                                                                               (4 << 6) - y * dx,
                                                                               (5 << 6) - y * dx,
                                                                               (6 << 6) - y * dx,
                                                                               (7 << 6) - y * dx),
                                                                upsample_above),
                                                 c3f),
                                   1));
            } else {
                a1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + 1 + base_shift));
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)highbd_load_maskx[base_shift]);
                a1_x128 = _mm_shuffle_epi8(a1_x128, *(__m128i *)highbd_load_maskx[base_shift]);

                shift = _mm256_castsi128_si256(_mm_srli_epi16(_mm_and_si128(_mm_setr_epi16(-y * dx,
                                                                                           (1 << 6) - y * dx,
                                                                                           (2 << 6) - y * dx,
                                                                                           (3 << 6) - y * dx,
                                                                                           (4 << 6) - y * dx,
                                                                                           (5 << 6) - y * dx,
                                                                                           (6 << 6) - y * dx,
                                                                                           (7 << 6) - y * dx),
                                                                            c3f),
                                                              1));
            }
            a0_x = _mm256_castsi128_si256(a0_x128);
            a1_x = _mm256_castsi128_si256(a1_x128);
        }

        // y calc
        __m128i a0_y, a1_y, shifty;
        if (base_x < min_base_x) {
            DECLARE_ALIGNED(32, int16_t, base_y_c[8]);
            __m128i r6, c1234, dy128, y_c128, base_y_c128, mask128;
            r6          = _mm_set1_epi16(r << 6);
            dy128       = _mm_set1_epi16(dy);
            c1234       = _mm_setr_epi16(1, 2, 3, 4, 5, 6, 7, 8);
            y_c128      = _mm_sub_epi16(r6, _mm_mullo_epi16(c1234, dy128));
            base_y_c128 = _mm_srai_epi16(y_c128, frac_bits_y);
            mask128     = _mm_cmpgt_epi16(min_base_y128, base_y_c128);
            base_y_c128 = _mm_andnot_si128(mask128, base_y_c128);
            _mm_storeu_si128((__m128i *)base_y_c, base_y_c128);

            a0_y = _mm_setr_epi16(left[base_y_c[0]],
                                  left[base_y_c[1]],
                                  left[base_y_c[2]],
                                  left[base_y_c[3]],
                                  left[base_y_c[4]],
                                  left[base_y_c[5]],
                                  left[base_y_c[6]],
                                  left[base_y_c[7]]);
            a1_y = _mm_setr_epi16(left[base_y_c[0] + 1],
                                  left[base_y_c[1] + 1],
                                  left[base_y_c[2] + 1],
                                  left[base_y_c[3] + 1],
                                  left[base_y_c[4] + 1],
                                  left[base_y_c[5] + 1],
                                  left[base_y_c[6] + 1],
                                  left[base_y_c[7] + 1]);

            if (upsample_left) {
                shifty = _mm_srli_epi16(_mm_and_si128(_mm_slli_epi16((y_c128), upsample_left), c3f), 1);
            } else {
                shifty = _mm_srli_epi16(_mm_and_si128(y_c128, c3f), 1);
            }
            a0_x  = _mm256_inserti128_si256(a0_x, a0_y, 1);
            a1_x  = _mm256_inserti128_si256(a1_x, a1_y, 1);
            shift = _mm256_inserti128_si256(shift, shifty, 1);
        }

        diff = _mm256_sub_epi16(a1_x, a0_x); // a[x+1] - a[x]
        a32  = _mm256_slli_epi16(a0_x, 5); // a[x] * 32
        a32  = _mm256_add_epi16(a32, a16); // a[x] * 32 + 16

        b   = _mm256_mullo_epi16(diff, shift);
        res = _mm256_add_epi16(a32, b);
        res = _mm256_srli_epi16(res, 5);

        resx = _mm256_castsi256_si128(res);
        resy = _mm256_extracti128_si256(res, 1);

        resxy = _mm_blendv_epi8(resx, resy, *(__m128i *)highbd_base_mask[base_min_diff]);
        _mm_storeu_si128((__m128i *)(dst), resxy);
        dst += stride;
    }
}

static void highbd_dr_prediction_z2_nx8_32bit_avx2(int32_t N, uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                                   const uint16_t *left, int32_t upsample_above, int32_t upsample_left,
                                                   int32_t dx, int32_t dy) {
    const int32_t min_base_x  = -(1 << upsample_above);
    const int32_t min_base_y  = -(1 << upsample_left);
    const int32_t frac_bits_x = 6 - upsample_above;
    const int32_t frac_bits_y = 6 - upsample_left;

    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be calculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0_x, a1_x, a0_y, a1_y, a32, a16, c3f, min_base_y256;
    __m256i diff;
    __m128i a0_x128, a1_x128;

    a16           = _mm256_set1_epi32(16);
    c3f           = _mm256_set1_epi32(0x3f);
    min_base_y256 = _mm256_set1_epi32(min_base_y);

    for (int32_t r = 0; r < N; r++) {
        __m256i b, res, shift;
        __m128i resx, resy, resxy;
        int32_t y          = r + 1;
        int32_t base_x     = (-y * dx) >> frac_bits_x;
        int32_t base_shift = 0;
        if (base_x < (min_base_x - 1)) {
            base_shift = (min_base_x - base_x) >> upsample_above;
        }
        int32_t base_min_diff = (min_base_x - base_x + upsample_above) >> upsample_above;
        if (base_min_diff > 8) {
            base_min_diff = 8;
        } else {
            if (base_min_diff < 0)
                base_min_diff = 0;
        }

        if (base_shift > 7) {
            resx = _mm_setzero_si128();
        } else {
            a0_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift));
            if (upsample_above) {
                __m128i mask, atmp0, atmp1, atmp2, atmp3;
                a1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + 8 + base_shift));
                atmp0   = _mm_shuffle_epi8(a0_x128, *(__m128i *)highbd_even_odd_maskx[base_shift]);
                atmp1   = _mm_shuffle_epi8(a1_x128, *(__m128i *)highbd_even_odd_maskx[base_shift]);
                atmp2   = _mm_shuffle_epi8(a0_x128, *(__m128i *)(highbd_even_odd_maskx[base_shift] + 16));
                atmp3   = _mm_shuffle_epi8(a1_x128, *(__m128i *)(highbd_even_odd_maskx[base_shift] + 16));
                mask    = _mm_cmpgt_epi8(*(__m128i *)highbd_even_odd_maskx[base_shift], _mm_set1_epi8(15));
                a0_x128 = _mm_blendv_epi8(atmp0, atmp1, mask);
                mask    = _mm_cmpgt_epi8(*(__m128i *)(highbd_even_odd_maskx[base_shift] + 16), _mm_set1_epi8(15));
                a1_x128 = _mm_blendv_epi8(atmp2, atmp3, mask);
                shift   = _mm256_srli_epi32(_mm256_and_si256(_mm256_slli_epi32(_mm256_setr_epi32(-y * dx,
                                                                                               (1 << 6) - y * dx,
                                                                                               (2 << 6) - y * dx,
                                                                                               (3 << 6) - y * dx,
                                                                                               (4 << 6) - y * dx,
                                                                                               (5 << 6) - y * dx,
                                                                                               (6 << 6) - y * dx,
                                                                                               (7 << 6) - y * dx),
                                                                             upsample_above),
                                                           c3f),
                                          1);
            } else {
                a1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + 1 + base_shift));
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)highbd_load_maskx[base_shift]);
                a1_x128 = _mm_shuffle_epi8(a1_x128, *(__m128i *)highbd_load_maskx[base_shift]);

                shift = _mm256_srli_epi32(_mm256_and_si256(_mm256_setr_epi32(-y * dx,
                                                                             (1 << 6) - y * dx,
                                                                             (2 << 6) - y * dx,
                                                                             (3 << 6) - y * dx,
                                                                             (4 << 6) - y * dx,
                                                                             (5 << 6) - y * dx,
                                                                             (6 << 6) - y * dx,
                                                                             (7 << 6) - y * dx),
                                                           c3f),
                                          1);
            }

            a0_x = _mm256_cvtepu16_epi32(a0_x128);
            a1_x = _mm256_cvtepu16_epi32(a1_x128);

            diff = _mm256_sub_epi32(a1_x, a0_x); // a[x+1] - a[x]
            a32  = _mm256_slli_epi32(a0_x, 5); // a[x] * 32
            a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16

            b   = _mm256_mullo_epi32(diff, shift);
            res = _mm256_add_epi32(a32, b);
            res = _mm256_srli_epi32(res, 5);

            resx = _mm256_castsi256_si128(
                _mm256_packus_epi32(res, _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1))));
        }
        // y calc
        if (base_x < min_base_x) {
            DECLARE_ALIGNED(32, int32_t, base_y_c[8]);
            __m256i r6, c256, dy256, y_c256, base_y_c256, mask256;
            r6          = _mm256_set1_epi32(r << 6);
            dy256       = _mm256_set1_epi32(dy);
            c256        = _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 8);
            y_c256      = _mm256_sub_epi32(r6, _mm256_mullo_epi32(c256, dy256));
            base_y_c256 = _mm256_srai_epi32(y_c256, frac_bits_y);
            mask256     = _mm256_cmpgt_epi32(min_base_y256, base_y_c256);
            base_y_c256 = _mm256_andnot_si256(mask256, base_y_c256);
            _mm256_storeu_si256((__m256i *)base_y_c, base_y_c256);

            a0_y = _mm256_cvtepu16_epi32(_mm_setr_epi16(left[base_y_c[0]],
                                                        left[base_y_c[1]],
                                                        left[base_y_c[2]],
                                                        left[base_y_c[3]],
                                                        left[base_y_c[4]],
                                                        left[base_y_c[5]],
                                                        left[base_y_c[6]],
                                                        left[base_y_c[7]]));
            a1_y = _mm256_cvtepu16_epi32(_mm_setr_epi16(left[base_y_c[0] + 1],
                                                        left[base_y_c[1] + 1],
                                                        left[base_y_c[2] + 1],
                                                        left[base_y_c[3] + 1],
                                                        left[base_y_c[4] + 1],
                                                        left[base_y_c[5] + 1],
                                                        left[base_y_c[6] + 1],
                                                        left[base_y_c[7] + 1]));

            if (upsample_left) {
                shift = _mm256_srli_epi32(_mm256_and_si256(_mm256_slli_epi32((y_c256), upsample_left), c3f), 1);
            } else {
                shift = _mm256_srli_epi32(_mm256_and_si256(y_c256, c3f), 1);
            }
            diff = _mm256_sub_epi32(a1_y, a0_y); // a[x+1] - a[x]
            a32  = _mm256_slli_epi32(a0_y, 5); // a[x] * 32
            a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16

            b   = _mm256_mullo_epi32(diff, shift);
            res = _mm256_add_epi32(a32, b);
            res = _mm256_srli_epi32(res, 5);

            resy = _mm256_castsi256_si128(
                _mm256_packus_epi32(res, _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1))));
        } else {
            resy = resx;
        }
        resxy = _mm_blendv_epi8(resx, resy, *(__m128i *)highbd_base_mask[base_min_diff]);
        _mm_storeu_si128((__m128i *)(dst), resxy);
        dst += stride;
    }
}

static void highbd_dr_prediction_z2_hxw_avx2(int32_t H, int32_t W, uint16_t *dst, ptrdiff_t stride,
                                             const uint16_t *above, const uint16_t *left, int32_t upsample_above,
                                             int32_t upsample_left, int32_t dx, int32_t dy) {
    // here upsample_above and upsample_left are 0 by design of
    // av1_use_intra_edge_upsample
    const int min_base_x = -1;
    const int min_base_y = -1;
    (void)upsample_above;
    (void)upsample_left;
    const int frac_bits_x = 6;
    const int frac_bits_y = 6;

    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be calculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0_x, a1_x, a32, a16, c3f, c1;
    __m256i diff, min_base_y256, dy256, c1234, c0123;
    DECLARE_ALIGNED(32, int16_t, base_y_c[16]);

    a16           = _mm256_set1_epi16(16);
    c1            = _mm256_srli_epi16(a16, 4);
    min_base_y256 = _mm256_set1_epi16(min_base_y);
    c3f           = _mm256_set1_epi16(0x3f);
    dy256         = _mm256_set1_epi16(dy);
    c0123         = _mm256_setr_epi16(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    c1234         = _mm256_add_epi16(c0123, c1);

    for (int r = 0; r < H; r++) {
        __m256i b, res, shift;
        __m256i resx, resy, ydx;
        __m256i resxy, j256, r6;
        __m128i a0_x128, a1_x128, a0_1_x128, a1_1_x128;
        int     y = r + 1;
        ydx       = _mm256_set1_epi16(y * dx);

        for (int j = 0; j < W; j += 16) {
            j256           = _mm256_set1_epi16(j);
            int base_x     = ((j << 6) - y * dx) >> frac_bits_x;
            int base_shift = 0;
            if ((base_x) < (min_base_x - 1)) {
                base_shift = (min_base_x - (base_x)-1);
            }
            int base_min_diff = (min_base_x - base_x);
            if (base_min_diff > 16) {
                base_min_diff = 16;
            } else {
                if (base_min_diff < 0)
                    base_min_diff = 0;
            }

            if (base_shift < 8) {
                a0_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift));
                a1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift + 1));
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)highbd_load_maskx[base_shift]);
                a1_x128 = _mm_shuffle_epi8(a1_x128, *(__m128i *)highbd_load_maskx[base_shift]);

                a0_x = _mm256_castsi128_si256(a0_x128);
                a1_x = _mm256_castsi128_si256(a1_x128);
            } else {
                a0_x = _mm256_setzero_si256();
                a1_x = _mm256_setzero_si256();
            }

            int base_shift1 = 0;
            if (base_shift > 8) {
                base_shift1 = base_shift - 8;
            }
            if (base_shift1 < 8) {
                a0_1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift1 + 8));
                a1_1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift1 + 9));
                a0_1_x128 = _mm_shuffle_epi8(a0_1_x128, *(__m128i *)highbd_load_maskx[base_shift1]);
                a1_1_x128 = _mm_shuffle_epi8(a1_1_x128, *(__m128i *)highbd_load_maskx[base_shift1]);

                a0_x = _mm256_inserti128_si256(a0_x, a0_1_x128, 1);
                a1_x = _mm256_inserti128_si256(a1_x, a1_1_x128, 1);
            }
            r6    = _mm256_slli_epi16(_mm256_add_epi16(c0123, j256), 6);
            shift = _mm256_srli_epi16(_mm256_and_si256(_mm256_sub_epi16(r6, ydx), c3f), 1);

            diff = _mm256_sub_epi16(a1_x, a0_x); // a[x+1] - a[x]
            a32  = _mm256_slli_epi16(a0_x, 5); // a[x] * 32
            a32  = _mm256_add_epi16(a32, a16); // a[x] * 32 + 16

            b    = _mm256_mullo_epi16(diff, shift);
            res  = _mm256_add_epi16(a32, b);
            resx = _mm256_srli_epi16(res, 5); // 16 16-bit values

            // y calc
            resy = _mm256_setzero_si256();
            __m256i a0_y, a1_y, shifty;
            if ((base_x < min_base_x)) {
                __m256i c256, y_c256, base_y_c256, mask256, mul16;
                r6          = _mm256_set1_epi16(r << 6);
                c256        = _mm256_add_epi16(j256, c1234);
                mul16       = _mm256_min_epu16(_mm256_mullo_epi16(c256, dy256), _mm256_srli_epi16(min_base_y256, 1));
                y_c256      = _mm256_sub_epi16(r6, mul16);
                base_y_c256 = _mm256_srai_epi16(y_c256, frac_bits_y);
                mask256     = _mm256_cmpgt_epi16(min_base_y256, base_y_c256);
                base_y_c256 = _mm256_andnot_si256(mask256, base_y_c256);
                _mm256_storeu_si256((__m256i *)base_y_c, base_y_c256);

                a0_y        = _mm256_setr_epi16(left[base_y_c[0]],
                                         left[base_y_c[1]],
                                         left[base_y_c[2]],
                                         left[base_y_c[3]],
                                         left[base_y_c[4]],
                                         left[base_y_c[5]],
                                         left[base_y_c[6]],
                                         left[base_y_c[7]],
                                         left[base_y_c[8]],
                                         left[base_y_c[9]],
                                         left[base_y_c[10]],
                                         left[base_y_c[11]],
                                         left[base_y_c[12]],
                                         left[base_y_c[13]],
                                         left[base_y_c[14]],
                                         left[base_y_c[15]]);
                base_y_c256 = _mm256_add_epi16(base_y_c256, c1);
                _mm256_storeu_si256((__m256i *)base_y_c, base_y_c256);

                a1_y = _mm256_setr_epi16(left[base_y_c[0]],
                                         left[base_y_c[1]],
                                         left[base_y_c[2]],
                                         left[base_y_c[3]],
                                         left[base_y_c[4]],
                                         left[base_y_c[5]],
                                         left[base_y_c[6]],
                                         left[base_y_c[7]],
                                         left[base_y_c[8]],
                                         left[base_y_c[9]],
                                         left[base_y_c[10]],
                                         left[base_y_c[11]],
                                         left[base_y_c[12]],
                                         left[base_y_c[13]],
                                         left[base_y_c[14]],
                                         left[base_y_c[15]]);

                shifty = _mm256_srli_epi16(_mm256_and_si256(y_c256, c3f), 1);

                diff = _mm256_sub_epi16(a1_y, a0_y); // a[x+1] - a[x]
                a32  = _mm256_slli_epi16(a0_y, 5); // a[x] * 32
                a32  = _mm256_add_epi16(a32, a16); // a[x] * 32 + 16

                b    = _mm256_mullo_epi16(diff, shifty);
                res  = _mm256_add_epi16(a32, b);
                resy = _mm256_srli_epi16(res, 5);
            }

            resxy = _mm256_blendv_epi8(resx, resy, *(__m256i *)highbd_base_mask[base_min_diff]);
            _mm256_storeu_si256((__m256i *)(dst + j), resxy);
        } // for j
        dst += stride;
    }
}

static void highbd_dr_prediction_z2_hxw_32bit_avx2(int32_t H, int32_t W, uint16_t *dst, ptrdiff_t stride,
                                                   const uint16_t *above, const uint16_t *left, int32_t upsample_above,
                                                   int32_t upsample_left, int32_t dx, int32_t dy) {
    // here upsample_above and upsample_left are 0 by design of
    // av1_use_intra_edge_upsample
    const int32_t min_base_x = -1;
    const int32_t min_base_y = -1;
    (void)upsample_above;
    (void)upsample_left;
    const int32_t frac_bits_x = 6;
    const int32_t frac_bits_y = 6;

    // pre-filter above pixels
    // store in temp buffers:
    //   above[x] * 32 + 16
    //   above[x+1] - above[x]
    // final pixels will be calculated as:
    //   (above[x] * 32 + 16 + (above[x+1] - above[x]) * shift) >> 5
    __m256i a0_x, a1_x, a0_y, a1_y, a32, a0_1_x, a1_1_x, a16, c1;
    __m256i diff, min_base_y256, c3f, dy256, c1234, c0123, c8;
    __m128i a0_x128, a1_x128, a0_1_x128, a1_1_x128;
    DECLARE_ALIGNED(32, int, base_y_c[16]);

    a16           = _mm256_set1_epi32(16);
    c1            = _mm256_srli_epi32(a16, 4);
    c8            = _mm256_srli_epi32(a16, 1);
    min_base_y256 = _mm256_set1_epi16(min_base_y);
    c3f           = _mm256_set1_epi32(0x3f);
    dy256         = _mm256_set1_epi32(dy);
    c0123         = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    c1234         = _mm256_add_epi32(c0123, c1);
    for (int32_t r = 0; r < H; r++) {
        __m256i b, res, shift, ydx;
        __m256i resx[2], resy[2];
        __m256i resxy, j256, r6;
        for (int32_t j = 0; j < W; j += 16) {
            j256  = _mm256_set1_epi32(j);
            int y = r + 1;
            ydx   = _mm256_set1_epi32(y * dx);

            int base_x     = ((j << 6) - y * dx) >> frac_bits_x;
            int base_shift = 0;
            if ((base_x) < (min_base_x - 1)) {
                base_shift = (min_base_x - base_x - 1);
            }
            int base_min_diff = (min_base_x - base_x);
            if (base_min_diff > 16) {
                base_min_diff = 16;
            } else {
                if (base_min_diff < 0)
                    base_min_diff = 0;
            }

            if (base_shift > 7) {
                resx[0] = _mm256_setzero_si256();
            } else {
                a0_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift));
                a1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift + 1));
                a0_x128 = _mm_shuffle_epi8(a0_x128, *(__m128i *)highbd_load_maskx[base_shift]);
                a1_x128 = _mm_shuffle_epi8(a1_x128, *(__m128i *)highbd_load_maskx[base_shift]);

                a0_x = _mm256_cvtepu16_epi32(a0_x128);
                a1_x = _mm256_cvtepu16_epi32(a1_x128);

                r6    = _mm256_slli_epi32(_mm256_add_epi32(c0123, j256), 6);
                shift = _mm256_srli_epi32(_mm256_and_si256(_mm256_sub_epi32(r6, ydx), c3f), 1);

                diff = _mm256_sub_epi32(a1_x, a0_x); // a[x+1] - a[x]
                a32  = _mm256_slli_epi32(a0_x, 5); // a[x] * 32
                a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16

                b   = _mm256_mullo_epi32(diff, shift);
                res = _mm256_add_epi32(a32, b);
                res = _mm256_srli_epi32(res, 5);

                resx[0] = _mm256_packus_epi32(res, _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1)));
            }
            int base_shift8 = 0;
            if ((base_x + 8) < (min_base_x - 1)) {
                base_shift8 = (min_base_x - (base_x + 8) - 1);
            }
            if (base_shift8 > 7) {
                resx[1] = _mm256_setzero_si256();
            } else {
                a0_1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift8 + 8));
                a1_1_x128 = _mm_loadu_si128((__m128i *)(above + base_x + base_shift8 + 9));
                a0_1_x128 = _mm_shuffle_epi8(a0_1_x128, *(__m128i *)highbd_load_maskx[base_shift8]);
                a1_1_x128 = _mm_shuffle_epi8(a1_1_x128, *(__m128i *)highbd_load_maskx[base_shift8]);

                a0_1_x = _mm256_cvtepu16_epi32(a0_1_x128);
                a1_1_x = _mm256_cvtepu16_epi32(a1_1_x128);

                r6    = _mm256_slli_epi32(_mm256_add_epi32(c0123, _mm256_add_epi32(j256, c8)), 6);
                shift = _mm256_srli_epi32(_mm256_and_si256(_mm256_sub_epi32(r6, ydx), c3f), 1);

                diff = _mm256_sub_epi32(a1_1_x, a0_1_x); // a[x+1] - a[x]
                a32  = _mm256_slli_epi32(a0_1_x, 5); // a[x] * 32
                a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16
                b    = _mm256_mullo_epi32(diff, shift);

                resx[1] = _mm256_add_epi32(a32, b);
                resx[1] = _mm256_srli_epi32(resx[1], 5);
                resx[1] = _mm256_packus_epi32(resx[1], _mm256_castsi128_si256(_mm256_extracti128_si256(resx[1], 1)));
            }
            resx[0] = _mm256_inserti128_si256(resx[0], _mm256_castsi256_si128(resx[1]),
                                              1); // 16 16bit values

            // y calc
            resy[0] = _mm256_setzero_si256();
            if ((base_x < min_base_x)) {
                __m256i c256, y_c256, y_c_1_256, base_y_c256, mask256;
                r6          = _mm256_set1_epi32(r << 6);
                c256        = _mm256_add_epi32(j256, c1234);
                y_c256      = _mm256_sub_epi32(r6, _mm256_mullo_epi32(c256, dy256));
                base_y_c256 = _mm256_srai_epi32(y_c256, frac_bits_y);
                mask256     = _mm256_cmpgt_epi32(min_base_y256, base_y_c256);
                base_y_c256 = _mm256_andnot_si256(mask256, base_y_c256);
                _mm256_storeu_si256((__m256i *)base_y_c, base_y_c256);
                c256        = _mm256_add_epi32(c256, c8);
                y_c_1_256   = _mm256_sub_epi32(r6, _mm256_mullo_epi32(c256, dy256));
                base_y_c256 = _mm256_srai_epi32(y_c_1_256, frac_bits_y);
                mask256     = _mm256_cmpgt_epi32(min_base_y256, base_y_c256);
                base_y_c256 = _mm256_andnot_si256(mask256, base_y_c256);
                _mm256_storeu_si256((__m256i *)(base_y_c + 8), base_y_c256);

                a0_y = _mm256_cvtepu16_epi32(_mm_setr_epi16(left[base_y_c[0]],
                                                            left[base_y_c[1]],
                                                            left[base_y_c[2]],
                                                            left[base_y_c[3]],
                                                            left[base_y_c[4]],
                                                            left[base_y_c[5]],
                                                            left[base_y_c[6]],
                                                            left[base_y_c[7]]));
                a1_y = _mm256_cvtepu16_epi32(_mm_setr_epi16(left[base_y_c[0] + 1],
                                                            left[base_y_c[1] + 1],
                                                            left[base_y_c[2] + 1],
                                                            left[base_y_c[3] + 1],
                                                            left[base_y_c[4] + 1],
                                                            left[base_y_c[5] + 1],
                                                            left[base_y_c[6] + 1],
                                                            left[base_y_c[7] + 1]));

                shift = _mm256_srli_epi32(_mm256_and_si256(y_c256, c3f), 1);

                diff = _mm256_sub_epi32(a1_y, a0_y); // a[x+1] - a[x]
                a32  = _mm256_slli_epi32(a0_y, 5); // a[x] * 32
                a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16

                b   = _mm256_mullo_epi32(diff, shift);
                res = _mm256_add_epi32(a32, b);
                res = _mm256_srli_epi32(res, 5);

                resy[0] = _mm256_packus_epi32(res, _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1)));

                a0_y  = _mm256_cvtepu16_epi32(_mm_setr_epi16(left[base_y_c[8]],
                                                            left[base_y_c[9]],
                                                            left[base_y_c[10]],
                                                            left[base_y_c[11]],
                                                            left[base_y_c[12]],
                                                            left[base_y_c[13]],
                                                            left[base_y_c[14]],
                                                            left[base_y_c[15]]));
                a1_y  = _mm256_cvtepu16_epi32(_mm_setr_epi16(left[base_y_c[8] + 1],
                                                            left[base_y_c[9] + 1],
                                                            left[base_y_c[10] + 1],
                                                            left[base_y_c[11] + 1],
                                                            left[base_y_c[12] + 1],
                                                            left[base_y_c[13] + 1],
                                                            left[base_y_c[14] + 1],
                                                            left[base_y_c[15] + 1]));
                shift = _mm256_srli_epi32(_mm256_and_si256(y_c_1_256, c3f), 1);

                diff = _mm256_sub_epi32(a1_y, a0_y); // a[x+1] - a[x]
                a32  = _mm256_slli_epi32(a0_y, 5); // a[x] * 32
                a32  = _mm256_add_epi32(a32, a16); // a[x] * 32 + 16

                b   = _mm256_mullo_epi32(diff, shift);
                res = _mm256_add_epi32(a32, b);
                res = _mm256_srli_epi32(res, 5);

                resy[1] = _mm256_packus_epi32(res, _mm256_castsi128_si256(_mm256_extracti128_si256(res, 1)));

                resy[0] = _mm256_inserti128_si256(resy[0], _mm256_castsi256_si128(resy[1]),
                                                  1); // 16 16bit values
            }
            resxy = _mm256_blendv_epi8(resx[0], resy[0], *(__m256i *)highbd_base_mask[base_min_diff]);
            _mm256_storeu_si256((__m256i *)(dst + j), resxy);
        } // for j
        dst += stride;
    }
}

// Directional prediction, zone 2: 90 < angle < 180
void svt_av1_highbd_dr_prediction_z2_avx2(uint16_t *dst, ptrdiff_t stride, int32_t bw, int32_t bh,
                                          const uint16_t *above, const uint16_t *left, int32_t upsample_above,
                                          int32_t upsample_left, int32_t dx, int32_t dy, int32_t bd) {
    (void)bd;
    assert(dx > 0);
    assert(dy > 0);
    switch (bw) {
    case 4:
        if (bd < 12) {
            highbd_dr_prediction_z2_nx4_avx2(bh, dst, stride, above, left, upsample_above, upsample_left, dx, dy);
        } else {
            highbd_dr_prediction_z2_nx4_32bit_avx2(bh, dst, stride, above, left, upsample_above, upsample_left, dx, dy);
        }
        break;
    case 8:
        if (bd < 12) {
            highbd_dr_prediction_z2_nx8_avx2(bh, dst, stride, above, left, upsample_above, upsample_left, dx, dy);
        } else {
            highbd_dr_prediction_z2_nx8_32bit_avx2(bh, dst, stride, above, left, upsample_above, upsample_left, dx, dy);
        }
        break;
    default:
        if (bd < 12) {
            highbd_dr_prediction_z2_hxw_avx2(bh, bw, dst, stride, above, left, upsample_above, upsample_left, dx, dy);
        } else {
            highbd_dr_prediction_z2_hxw_32bit_avx2(
                bh, bw, dst, stride, above, left, upsample_above, upsample_left, dx, dy);
        }
        break;
    }
    return;
}

static void highbd_transpose_tx_16x16(const uint16_t *src, ptrdiff_t pitchSrc, uint16_t *dst, ptrdiff_t pitchDst) {
    __m256i r[16];
    __m256i d[16];
    for (int j = 0; j < 16; j++) { r[j] = _mm256_loadu_si256((__m256i *)(src + j * pitchSrc)); }
    transpose_16bit_16x16_avx2(r, d);
    for (int j = 0; j < 16; j++) { _mm256_storeu_si256((__m256i *)(dst + j * pitchDst), d[j]); }
}

static void highbd_transpose(const uint16_t *src, ptrdiff_t pitchSrc, uint16_t *dst, ptrdiff_t pitchDst, int32_t width,
                             int32_t height) {
    for (int j = 0; j < height; j += 16)
        for (int i = 0; i < width; i += 16)
            highbd_transpose_tx_16x16(src + i * pitchSrc + j, pitchSrc, dst + j * pitchDst + i, pitchDst);
}

static void highbd_dr_prediction_z3_4x4_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                             int32_t upsample_left, int32_t dy) {
    __m128i dstvec[4], d[4];

    highbd_dr_prediction_z1_4xn_internal_avx2(4, dstvec, left, upsample_left, dy);
    highbd_transpose4x8_8x4_low_sse2(&dstvec[0], &dstvec[1], &dstvec[2], &dstvec[3], &d[0], &d[1], &d[2], &d[3]);
    _mm_storel_epi64((__m128i *)(dst + 0 * stride), d[0]);
    _mm_storel_epi64((__m128i *)(dst + 1 * stride), d[1]);
    _mm_storel_epi64((__m128i *)(dst + 2 * stride), d[2]);
    _mm_storel_epi64((__m128i *)(dst + 3 * stride), d[3]);
    return;
}

static void highbd_dr_prediction_z3_8x8_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                             int32_t upsample_left, int32_t dy) {
    __m128i dstvec[8], d[8];

    highbd_dr_prediction_z1_8xn_internal_avx2(8, dstvec, left, upsample_left, dy);
    highbd_transpose8x8_sse2(&dstvec[0],
                             &dstvec[1],
                             &dstvec[2],
                             &dstvec[3],
                             &dstvec[4],
                             &dstvec[5],
                             &dstvec[6],
                             &dstvec[7],
                             &d[0],
                             &d[1],
                             &d[2],
                             &d[3],
                             &d[4],
                             &d[5],
                             &d[6],
                             &d[7]);
    for (int32_t i = 0; i < 8; i++) { _mm_storeu_si128((__m128i *)(dst + i * stride), d[i]); }
}

static void highbd_dr_prediction_z3_4x8_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                             int32_t upsample_left, int32_t dy) {
    __m128i dstvec[4], d[8];

    highbd_dr_prediction_z1_8xn_internal_avx2(4, dstvec, left, upsample_left, dy);
    highbd_transpose4x8_8x4_sse2(
        &dstvec[0], &dstvec[1], &dstvec[2], &dstvec[3], &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
    for (int32_t i = 0; i < 8; i++) { _mm_storel_epi64((__m128i *)(dst + i * stride), d[i]); }
}

static void highbd_dr_prediction_z3_8x4_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                             int32_t upsample_left, int32_t dy) {
    __m128i dstvec[8], d[4];

    highbd_dr_prediction_z1_4xn_internal_avx2(8, dstvec, left, upsample_left, dy);
    highbd_transpose8x8_low_sse2(&dstvec[0],
                                 &dstvec[1],
                                 &dstvec[2],
                                 &dstvec[3],
                                 &dstvec[4],
                                 &dstvec[5],
                                 &dstvec[6],
                                 &dstvec[7],
                                 &d[0],
                                 &d[1],
                                 &d[2],
                                 &d[3]);
    _mm_storeu_si128((__m128i *)(dst + 0 * stride), d[0]);
    _mm_storeu_si128((__m128i *)(dst + 1 * stride), d[1]);
    _mm_storeu_si128((__m128i *)(dst + 2 * stride), d[2]);
    _mm_storeu_si128((__m128i *)(dst + 3 * stride), d[3]);
}

static void highbd_dr_prediction_z3_8x16_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                              int32_t upsample_left, int32_t dy) {
    __m256i dstvec[8], d[16];

    highbd_dr_prediction_z1_16xn_internal_avx2(8, dstvec, left, upsample_left, dy);
    highbd_transpose8x16_16x8_avx2(dstvec, d);
    for (int32_t i = 0; i < 8; i++) { _mm_storeu_si128((__m128i *)(dst + i * stride), _mm256_castsi256_si128(d[i])); }
    for (int32_t i = 8; i < 16; i++) {
        _mm_storeu_si128((__m128i *)(dst + i * stride), _mm256_extracti128_si256(d[i - 8], 1));
    }
}

static void highbd_dr_prediction_z3_16x8_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                              int32_t upsample_left, int32_t dy) {
    __m128i dstvec[16], d[16];

    highbd_dr_prediction_z1_8xn_internal_avx2(16, dstvec, left, upsample_left, dy);
    for (int32_t i = 0; i < 16; i += 8) {
        highbd_transpose8x8_sse2(&dstvec[0 + i],
                                 &dstvec[1 + i],
                                 &dstvec[2 + i],
                                 &dstvec[3 + i],
                                 &dstvec[4 + i],
                                 &dstvec[5 + i],
                                 &dstvec[6 + i],
                                 &dstvec[7 + i],
                                 &d[0 + i],
                                 &d[1 + i],
                                 &d[2 + i],
                                 &d[3 + i],
                                 &d[4 + i],
                                 &d[5 + i],
                                 &d[6 + i],
                                 &d[7 + i]);
    }
    for (int32_t i = 0; i < 8; i++) {
        _mm_storeu_si128((__m128i *)(dst + i * stride), d[i]);
        _mm_storeu_si128((__m128i *)(dst + i * stride + 8), d[i + 8]);
    }
}

static void highbd_dr_prediction_z3_4x16_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                              int32_t upsample_left, int32_t dy) {
    __m256i dstvec[4], d[4], d1;

    highbd_dr_prediction_z1_16xn_internal_avx2(4, dstvec, left, upsample_left, dy);
    highbd_transpose4x16_avx2(dstvec, d);
    for (int32_t i = 0; i < 4; i++) {
        _mm_storel_epi64((__m128i *)(dst + i * stride), _mm256_castsi256_si128(d[i]));
        d1 = _mm256_srli_si256(d[i], 8);
        _mm_storel_epi64((__m128i *)(dst + (i + 4) * stride), _mm256_castsi256_si128(d1));
        _mm_storel_epi64((__m128i *)(dst + (i + 8) * stride), _mm256_extracti128_si256(d[i], 1));
        _mm_storel_epi64((__m128i *)(dst + (i + 12) * stride), _mm256_extracti128_si256(d1, 1));
    }
}

static void highbd_dr_prediction_z3_16x4_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                              int32_t upsample_left, int32_t dy) {
    __m128i dstvec[16], d[8];

    highbd_dr_prediction_z1_4xn_internal_avx2(16, dstvec, left, upsample_left, dy);
    highbd_transpose16x4_8x8_sse2(dstvec, d);

    _mm_storeu_si128((__m128i *)(dst + 0 * stride), d[0]);
    _mm_storeu_si128((__m128i *)(dst + 0 * stride + 8), d[1]);
    _mm_storeu_si128((__m128i *)(dst + 1 * stride), d[2]);
    _mm_storeu_si128((__m128i *)(dst + 1 * stride + 8), d[3]);
    _mm_storeu_si128((__m128i *)(dst + 2 * stride), d[4]);
    _mm_storeu_si128((__m128i *)(dst + 2 * stride + 8), d[5]);
    _mm_storeu_si128((__m128i *)(dst + 3 * stride), d[6]);
    _mm_storeu_si128((__m128i *)(dst + 3 * stride + 8), d[7]);
}

static void highbd_dr_prediction_z3_8x32_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                              int32_t upsample_left, int32_t dy) {
    __m256i dstvec[16], d[16];

    highbd_dr_prediction_z1_32xn_internal_avx2(8, dstvec, left, upsample_left, dy);
    for (int32_t i = 0; i < 16; i += 8) { highbd_transpose8x16_16x8_avx2(dstvec + i, d + i); }

    for (int32_t i = 0; i < 8; i++) { _mm_storeu_si128((__m128i *)(dst + i * stride), _mm256_castsi256_si128(d[i])); }
    for (int32_t i = 0; i < 8; i++) {
        _mm_storeu_si128((__m128i *)(dst + (i + 8) * stride), _mm256_extracti128_si256(d[i], 1));
    }
    for (int32_t i = 8; i < 16; i++) {
        _mm_storeu_si128((__m128i *)(dst + (i + 8) * stride), _mm256_castsi256_si128(d[i]));
    }
    for (int32_t i = 8; i < 16; i++) {
        _mm_storeu_si128((__m128i *)(dst + (i + 16) * stride), _mm256_extracti128_si256(d[i], 1));
    }
}

static void highbd_dr_prediction_z3_32x8_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                              int32_t upsample_left, int32_t dy) {
    __m128i dstvec[32], d[32];

    highbd_dr_prediction_z1_8xn_internal_avx2(32, dstvec, left, upsample_left, dy);
    for (int32_t i = 0; i < 32; i += 8) {
        highbd_transpose8x8_sse2(&dstvec[0 + i],
                                 &dstvec[1 + i],
                                 &dstvec[2 + i],
                                 &dstvec[3 + i],
                                 &dstvec[4 + i],
                                 &dstvec[5 + i],
                                 &dstvec[6 + i],
                                 &dstvec[7 + i],
                                 &d[0 + i],
                                 &d[1 + i],
                                 &d[2 + i],
                                 &d[3 + i],
                                 &d[4 + i],
                                 &d[5 + i],
                                 &d[6 + i],
                                 &d[7 + i]);
    }
    for (int32_t i = 0; i < 8; i++) {
        _mm_storeu_si128((__m128i *)(dst + i * stride), d[i]);
        _mm_storeu_si128((__m128i *)(dst + i * stride + 8), d[i + 8]);
        _mm_storeu_si128((__m128i *)(dst + i * stride + 16), d[i + 16]);
        _mm_storeu_si128((__m128i *)(dst + i * stride + 24), d[i + 24]);
    }
}

static void highbd_dr_prediction_z3_16x16_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                               int32_t upsample_left, int32_t dy) {
    __m256i dstvec[16], d[16];

    highbd_dr_prediction_z1_16xn_internal_avx2(16, dstvec, left, upsample_left, dy);
    transpose_16bit_16x16_avx2(dstvec, d);

    for (int32_t i = 0; i < 16; i++) { _mm256_storeu_si256((__m256i *)(dst + i * stride), d[i]); }
}

static void highbd_dr_prediction_z3_32x32_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                               int32_t upsample_left, int32_t dy) {
    __m256i dstvec[64], d[16];

    highbd_dr_prediction_z1_32xn_internal_avx2(32, dstvec, left, upsample_left, dy);

    transpose_16bit_16x16_avx2(dstvec, d);
    for (int32_t j = 0; j < 16; j++) { _mm256_storeu_si256((__m256i *)(dst + j * stride), d[j]); }
    transpose_16bit_16x16_avx2(dstvec + 16, d);
    for (int32_t j = 0; j < 16; j++) { _mm256_storeu_si256((__m256i *)(dst + j * stride + 16), d[j]); }
    transpose_16bit_16x16_avx2(dstvec + 32, d);
    for (int32_t j = 0; j < 16; j++) { _mm256_storeu_si256((__m256i *)(dst + (j + 16) * stride), d[j]); }
    transpose_16bit_16x16_avx2(dstvec + 48, d);
    for (int32_t j = 0; j < 16; j++) { _mm256_storeu_si256((__m256i *)(dst + (j + 16) * stride + 16), d[j]); }
}

static void highbd_dr_prediction_z3_64x64_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                               int32_t upsample_left, int32_t dy) {
    DECLARE_ALIGNED(16, uint16_t, dst_t[64 * 64]);
    highbd_dr_prediction_z1_64xn_avx2(64, dst_t, 64, left, upsample_left, dy);
    highbd_transpose(dst_t, 64, dst, stride, 64, 64);
}

static void highbd_dr_prediction_z3_16x32_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                               int32_t upsample_left, int32_t dy) {
    __m256i dstvec[32], d[32];

    highbd_dr_prediction_z1_32xn_internal_avx2(16, dstvec, left, upsample_left, dy);
    for (int32_t i = 0; i < 32; i += 8) { highbd_transpose8x16_16x8_avx2(dstvec + i, d + i); }
    // store
    for (int32_t j = 0; j < 32; j += 16) {
        for (int32_t i = 0; i < 8; i++) {
            _mm_storeu_si128((__m128i *)(dst + (i + j) * stride), _mm256_castsi256_si128(d[(i + j)]));
        }
        for (int32_t i = 0; i < 8; i++) {
            _mm_storeu_si128((__m128i *)(dst + (i + j) * stride + 8), _mm256_castsi256_si128(d[(i + j) + 8]));
        }
        for (int32_t i = 8; i < 16; i++) {
            _mm256_storeu_si256((__m256i *)(dst + (i + j) * stride), yy_unpackhi_epi128(d[(i + j) - 8], d[(i + j)]));
        }
    }
}

static void highbd_dr_prediction_z3_32x16_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                               int32_t upsample_left, int32_t dy) {
    __m256i dstvec[32], d[16];

    highbd_dr_prediction_z1_16xn_internal_avx2(32, dstvec, left, upsample_left, dy);
    for (int32_t i = 0; i < 32; i += 16) {
        transpose_16bit_16x16_avx2((dstvec + i), d);
        for (int32_t j = 0; j < 16; j++) { _mm256_storeu_si256((__m256i *)(dst + j * stride + i), d[j]); }
    }
}

static void highbd_dr_prediction_z3_32x64_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                               int32_t upsample_left, int32_t dy) {
    uint16_t dst_t[64 * 32];
    highbd_dr_prediction_z1_64xn_avx2(32, dst_t, 64, left, upsample_left, dy);
    highbd_transpose(dst_t, 64, dst, stride, 32, 64);
}

static void highbd_dr_prediction_z3_64x32_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                               int32_t upsample_left, int32_t dy) {
    DECLARE_ALIGNED(16, uint16_t, dst_t[32 * 64]);
    highbd_dr_prediction_z1_32xn_avx2(64, dst_t, 32, left, upsample_left, dy);
    highbd_transpose(dst_t, 32, dst, stride, 64, 32);
    return;
}

static void highbd_dr_prediction_z3_16x64_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                               int32_t upsample_left, int32_t dy) {
    DECLARE_ALIGNED(16, uint16_t, dst_t[64 * 16]);
    highbd_dr_prediction_z1_64xn_avx2(16, dst_t, 64, left, upsample_left, dy);
    highbd_transpose(dst_t, 64, dst, stride, 16, 64);
}

static void highbd_dr_prediction_z3_64x16_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *left,
                                               int32_t upsample_left, int32_t dy) {
    __m256i dstvec[64], d[16];

    highbd_dr_prediction_z1_16xn_internal_avx2(64, dstvec, left, upsample_left, dy);
    for (int32_t i = 0; i < 64; i += 16) {
        transpose_16bit_16x16_avx2((dstvec + i), d);
        for (int32_t j = 0; j < 16; j++) { _mm256_storeu_si256((__m256i *)(dst + j * stride + i), d[j]); }
    }
}

void svt_av1_highbd_dr_prediction_z3_avx2(uint16_t *dst, ptrdiff_t stride, int32_t bw, int32_t bh,
                                          const uint16_t *above, const uint16_t *left, int32_t upsample_left,
                                          int32_t dx, int32_t dy, int32_t bd) {
    (void)above;
    (void)dx;
    (void)bd;
    assert(dx == 1);
    assert(dy > 0);
    if (bw == bh) {
        switch (bw) {
        case 4: highbd_dr_prediction_z3_4x4_avx2(dst, stride, left, upsample_left, dy); break;
        case 8: highbd_dr_prediction_z3_8x8_avx2(dst, stride, left, upsample_left, dy); break;
        case 16: highbd_dr_prediction_z3_16x16_avx2(dst, stride, left, upsample_left, dy); break;
        case 32: highbd_dr_prediction_z3_32x32_avx2(dst, stride, left, upsample_left, dy); break;
        case 64: highbd_dr_prediction_z3_64x64_avx2(dst, stride, left, upsample_left, dy); break;
        }
    } else {
        if (bw < bh) {
            if (bw + bw == bh) {
                switch (bw) {
                case 4: highbd_dr_prediction_z3_4x8_avx2(dst, stride, left, upsample_left, dy); break;
                case 8: highbd_dr_prediction_z3_8x16_avx2(dst, stride, left, upsample_left, dy); break;
                case 16: highbd_dr_prediction_z3_16x32_avx2(dst, stride, left, upsample_left, dy); break;
                case 32: highbd_dr_prediction_z3_32x64_avx2(dst, stride, left, upsample_left, dy); break;
                }
            } else {
                switch (bw) {
                case 4: highbd_dr_prediction_z3_4x16_avx2(dst, stride, left, upsample_left, dy); break;
                case 8: highbd_dr_prediction_z3_8x32_avx2(dst, stride, left, upsample_left, dy); break;
                case 16: highbd_dr_prediction_z3_16x64_avx2(dst, stride, left, upsample_left, dy); break;
                }
            }
        } else {
            if (bh + bh == bw) {
                switch (bh) {
                case 4: highbd_dr_prediction_z3_8x4_avx2(dst, stride, left, upsample_left, dy); break;
                case 8: highbd_dr_prediction_z3_16x8_avx2(dst, stride, left, upsample_left, dy); break;
                case 16: highbd_dr_prediction_z3_32x16_avx2(dst, stride, left, upsample_left, dy); break;
                case 32: highbd_dr_prediction_z3_64x32_avx2(dst, stride, left, upsample_left, dy); break;
                }
            } else {
                switch (bh) {
                case 4: highbd_dr_prediction_z3_16x4_avx2(dst, stride, left, upsample_left, dy); break;
                case 8: highbd_dr_prediction_z3_32x8_avx2(dst, stride, left, upsample_left, dy); break;
                case 16: highbd_dr_prediction_z3_64x16_avx2(dst, stride, left, upsample_left, dy); break;
                }
            }
        }
    }
    return;
}

static INLINE __m256i paeth_pred(const __m256i *left, const __m256i *top, const __m256i *topleft) {
    __m256i pl  = _mm256_sub_epi16(*top, *topleft);
    __m256i pt  = _mm256_sub_epi16(*left, *topleft);
    __m256i ptl = _mm256_abs_epi16(_mm256_add_epi16(pl, pt));
    pl          = _mm256_abs_epi16(pl);
    pt          = _mm256_abs_epi16(pt);

    __m256i mask1 = _mm256_cmpgt_epi16(pl, pt);
    mask1         = _mm256_or_si256(mask1, _mm256_cmpgt_epi16(pl, ptl));
    __m256i mask2 = _mm256_cmpgt_epi16(pt, ptl);

    pl = _mm256_andnot_si256(mask1, *left);

    ptl = _mm256_and_si256(mask2, *topleft);
    pt  = _mm256_andnot_si256(mask2, *top);
    pt  = _mm256_or_si256(pt, ptl);
    pt  = _mm256_and_si256(mask1, pt);

    return _mm256_or_si256(pt, pl);
}

// Return 16 8-bit pixels in one row (__m128i)
static INLINE __m128i paeth_16x1_pred(const __m256i *left, const __m256i *top, const __m256i *topleft) {
    const __m256i p0 = paeth_pred(left, top, topleft);
    const __m256i p1 = _mm256_permute4x64_epi64(p0, 0xe);
    const __m256i p  = _mm256_packus_epi16(p0, p1);
    return _mm256_castsi256_si128(p);
}

static INLINE __m256i get_top_vector(const uint8_t *above) {
    const __m128i x    = _mm_loadu_si128((const __m128i *)above);
    const __m128i zero = _mm_setzero_si128();
    const __m128i t0   = _mm_unpacklo_epi8(x, zero);
    const __m128i t1   = _mm_unpackhi_epi8(x, zero);
    return _mm256_inserti128_si256(_mm256_castsi128_si256(t0), t1, 1);
}

void svt_aom_paeth_predictor_16x8_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m128i       x    = _mm_loadl_epi64((const __m128i *)left);
    const __m256i l    = _mm256_inserti128_si256(_mm256_castsi128_si256(x), x, 1);
    const __m256i tl16 = _mm256_set1_epi16((uint16_t)above[-1]);
    __m256i       rep  = _mm256_set1_epi16(0x8000);
    const __m256i one  = _mm256_set1_epi16(1);
    const __m256i top  = get_top_vector(above);

    int i;
    for (i = 0; i < 8; ++i) {
        const __m256i l16 = _mm256_shuffle_epi8(l, rep);
        const __m128i row = paeth_16x1_pred(&l16, &top, &tl16);

        _mm_storeu_si128((__m128i *)dst, row);
        dst += stride;
        rep = _mm256_add_epi16(rep, one);
    }
}

static INLINE __m256i get_left_vector(const uint8_t *left) {
    const __m128i x = _mm_loadu_si128((const __m128i *)left);
    return _mm256_inserti128_si256(_mm256_castsi128_si256(x), x, 1);
}

void svt_aom_paeth_predictor_16x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i l    = get_left_vector(left);
    const __m256i tl16 = _mm256_set1_epi16((uint16_t)above[-1]);
    __m256i       rep  = _mm256_set1_epi16(0x8000);
    const __m256i one  = _mm256_set1_epi16(1);
    const __m256i top  = get_top_vector(above);

    int i;
    for (i = 0; i < 16; ++i) {
        const __m256i l16 = _mm256_shuffle_epi8(l, rep);
        const __m128i row = paeth_16x1_pred(&l16, &top, &tl16);

        _mm_storeu_si128((__m128i *)dst, row);
        dst += stride;
        rep = _mm256_add_epi16(rep, one);
    }
}

void svt_aom_paeth_predictor_16x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i       l    = get_left_vector(left);
    const __m256i tl16 = _mm256_set1_epi16((uint16_t)above[-1]);
    __m256i       rep  = _mm256_set1_epi16(0x8000);
    const __m256i one  = _mm256_set1_epi16(1);
    const __m256i top  = get_top_vector(above);

    int i;
    for (i = 0; i < 16; ++i) {
        const __m256i l16 = _mm256_shuffle_epi8(l, rep);
        const __m128i row = paeth_16x1_pred(&l16, &top, &tl16);

        _mm_storeu_si128((__m128i *)dst, row);
        dst += stride;
        rep = _mm256_add_epi16(rep, one);
    }

    l   = get_left_vector(left + 16);
    rep = _mm256_set1_epi16(0x8000);
    for (i = 0; i < 16; ++i) {
        const __m256i l16 = _mm256_shuffle_epi8(l, rep);
        const __m128i row = paeth_16x1_pred(&l16, &top, &tl16);

        _mm_storeu_si128((__m128i *)dst, row);
        dst += stride;
        rep = _mm256_add_epi16(rep, one);
    }
}

void svt_aom_paeth_predictor_16x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i tl16 = _mm256_set1_epi16((uint16_t)above[-1]);
    const __m256i one  = _mm256_set1_epi16(1);
    const __m256i top  = get_top_vector(above);

    for (int j = 0; j < 4; ++j) {
        const __m256i l   = get_left_vector(left + j * 16);
        __m256i       rep = _mm256_set1_epi16(0x8000);
        for (int i = 0; i < 16; ++i) {
            const __m256i l16 = _mm256_shuffle_epi8(l, rep);
            const __m128i row = paeth_16x1_pred(&l16, &top, &tl16);

            _mm_storeu_si128((__m128i *)dst, row);
            dst += stride;
            rep = _mm256_add_epi16(rep, one);
        }
    }
}

// Return 32 8-bit pixels in one row (__m256i)
static INLINE __m256i paeth_32x1_pred(const __m256i *left, const __m256i *top0, const __m256i *top1,
                                      const __m256i *topleft) {
    __m256i       p0 = paeth_pred(left, top0, topleft);
    __m256i       p1 = _mm256_permute4x64_epi64(p0, 0xe);
    const __m256i x0 = _mm256_packus_epi16(p0, p1);

    p0               = paeth_pred(left, top1, topleft);
    p1               = _mm256_permute4x64_epi64(p0, 0xe);
    const __m256i x1 = _mm256_packus_epi16(p0, p1);

    return yy_unpacklo_epi128(x0, x1);
}

void svt_aom_paeth_predictor_32x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i l   = get_left_vector(left);
    const __m256i t0  = get_top_vector(above);
    const __m256i t1  = get_top_vector(above + 16);
    const __m256i tl  = _mm256_set1_epi16((uint16_t)above[-1]);
    __m256i       rep = _mm256_set1_epi16(0x8000);
    const __m256i one = _mm256_set1_epi16(1);

    int i;
    for (i = 0; i < 16; ++i) {
        const __m256i l16 = _mm256_shuffle_epi8(l, rep);

        const __m256i r = paeth_32x1_pred(&l16, &t0, &t1, &tl);

        _mm256_storeu_si256((__m256i *)dst, r);

        dst += stride;
        rep = _mm256_add_epi16(rep, one);
    }
}

void svt_aom_paeth_predictor_32x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    __m256i       l   = get_left_vector(left);
    const __m256i t0  = get_top_vector(above);
    const __m256i t1  = get_top_vector(above + 16);
    const __m256i tl  = _mm256_set1_epi16((uint16_t)above[-1]);
    __m256i       rep = _mm256_set1_epi16(0x8000);
    const __m256i one = _mm256_set1_epi16(1);

    int i;
    for (i = 0; i < 16; ++i) {
        const __m256i l16 = _mm256_shuffle_epi8(l, rep);

        const __m128i r0 = paeth_16x1_pred(&l16, &t0, &tl);
        const __m128i r1 = paeth_16x1_pred(&l16, &t1, &tl);

        _mm_storeu_si128((__m128i *)dst, r0);
        _mm_storeu_si128((__m128i *)(dst + 16), r1);

        dst += stride;
        rep = _mm256_add_epi16(rep, one);
    }

    l   = get_left_vector(left + 16);
    rep = _mm256_set1_epi16(0x8000);
    for (i = 0; i < 16; ++i) {
        const __m256i l16 = _mm256_shuffle_epi8(l, rep);

        const __m128i r0 = paeth_16x1_pred(&l16, &t0, &tl);
        const __m128i r1 = paeth_16x1_pred(&l16, &t1, &tl);

        _mm_storeu_si128((__m128i *)dst, r0);
        _mm_storeu_si128((__m128i *)(dst + 16), r1);

        dst += stride;
        rep = _mm256_add_epi16(rep, one);
    }
}

void svt_aom_paeth_predictor_32x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i t0  = get_top_vector(above);
    const __m256i t1  = get_top_vector(above + 16);
    const __m256i tl  = _mm256_set1_epi16((uint16_t)above[-1]);
    const __m256i one = _mm256_set1_epi16(1);

    int i, j;
    for (j = 0; j < 4; ++j) {
        const __m256i l   = get_left_vector(left + j * 16);
        __m256i       rep = _mm256_set1_epi16(0x8000);
        for (i = 0; i < 16; ++i) {
            const __m256i l16 = _mm256_shuffle_epi8(l, rep);

            const __m128i r0 = paeth_16x1_pred(&l16, &t0, &tl);
            const __m128i r1 = paeth_16x1_pred(&l16, &t1, &tl);

            _mm_storeu_si128((__m128i *)dst, r0);
            _mm_storeu_si128((__m128i *)(dst + 16), r1);

            dst += stride;
            rep = _mm256_add_epi16(rep, one);
        }
    }
}

void svt_aom_paeth_predictor_64x32_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i t0  = get_top_vector(above);
    const __m256i t1  = get_top_vector(above + 16);
    const __m256i t2  = get_top_vector(above + 32);
    const __m256i t3  = get_top_vector(above + 48);
    const __m256i tl  = _mm256_set1_epi16((uint16_t)above[-1]);
    const __m256i one = _mm256_set1_epi16(1);

    int i, j;
    for (j = 0; j < 2; ++j) {
        const __m256i l   = get_left_vector(left + j * 16);
        __m256i       rep = _mm256_set1_epi16(0x8000);
        for (i = 0; i < 16; ++i) {
            const __m256i l16 = _mm256_shuffle_epi8(l, rep);

            const __m128i r0 = paeth_16x1_pred(&l16, &t0, &tl);
            const __m128i r1 = paeth_16x1_pred(&l16, &t1, &tl);
            const __m128i r2 = paeth_16x1_pred(&l16, &t2, &tl);
            const __m128i r3 = paeth_16x1_pred(&l16, &t3, &tl);

            _mm_storeu_si128((__m128i *)dst, r0);
            _mm_storeu_si128((__m128i *)(dst + 16), r1);
            _mm_storeu_si128((__m128i *)(dst + 32), r2);
            _mm_storeu_si128((__m128i *)(dst + 48), r3);

            dst += stride;
            rep = _mm256_add_epi16(rep, one);
        }
    }
}

void svt_aom_paeth_predictor_64x64_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i t0  = get_top_vector(above);
    const __m256i t1  = get_top_vector(above + 16);
    const __m256i t2  = get_top_vector(above + 32);
    const __m256i t3  = get_top_vector(above + 48);
    const __m256i tl  = _mm256_set1_epi16((uint16_t)above[-1]);
    const __m256i one = _mm256_set1_epi16(1);

    int i, j;
    for (j = 0; j < 4; ++j) {
        const __m256i l   = get_left_vector(left + j * 16);
        __m256i       rep = _mm256_set1_epi16(0x8000);
        for (i = 0; i < 16; ++i) {
            const __m256i l16 = _mm256_shuffle_epi8(l, rep);

            const __m128i r0 = paeth_16x1_pred(&l16, &t0, &tl);
            const __m128i r1 = paeth_16x1_pred(&l16, &t1, &tl);
            const __m128i r2 = paeth_16x1_pred(&l16, &t2, &tl);
            const __m128i r3 = paeth_16x1_pred(&l16, &t3, &tl);

            _mm_storeu_si128((__m128i *)dst, r0);
            _mm_storeu_si128((__m128i *)(dst + 16), r1);
            _mm_storeu_si128((__m128i *)(dst + 32), r2);
            _mm_storeu_si128((__m128i *)(dst + 48), r3);

            dst += stride;
            rep = _mm256_add_epi16(rep, one);
        }
    }
}

void svt_aom_paeth_predictor_64x16_avx2(uint8_t *dst, ptrdiff_t stride, const uint8_t *above, const uint8_t *left) {
    const __m256i t0  = get_top_vector(above);
    const __m256i t1  = get_top_vector(above + 16);
    const __m256i t2  = get_top_vector(above + 32);
    const __m256i t3  = get_top_vector(above + 48);
    const __m256i tl  = _mm256_set1_epi16((uint16_t)above[-1]);
    const __m256i one = _mm256_set1_epi16(1);

    int           i;
    const __m256i l   = get_left_vector(left);
    __m256i       rep = _mm256_set1_epi16(0x8000);
    for (i = 0; i < 16; ++i) {
        const __m256i l16 = _mm256_shuffle_epi8(l, rep);

        const __m128i r0 = paeth_16x1_pred(&l16, &t0, &tl);
        const __m128i r1 = paeth_16x1_pred(&l16, &t1, &tl);
        const __m128i r2 = paeth_16x1_pred(&l16, &t2, &tl);
        const __m128i r3 = paeth_16x1_pred(&l16, &t3, &tl);

        _mm_storeu_si128((__m128i *)dst, r0);
        _mm_storeu_si128((__m128i *)(dst + 16), r1);
        _mm_storeu_si128((__m128i *)(dst + 32), r2);
        _mm_storeu_si128((__m128i *)(dst + 48), r3);

        dst += stride;
        rep = _mm256_add_epi16(rep, one);
    }
}

void svt_aom_highbd_paeth_predictor_16x4_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                              const uint16_t *left, int bd) {
    (void)bd;
    const __m256i tl16 = _mm256_set1_epi16(above[-1]);
    const __m256i top  = _mm256_loadu_si256((const __m256i *)above);
    __m256i       l16, row;
    int           i;

    for (i = 0; i < 4; ++i) {
        l16 = _mm256_set1_epi16(left[i]);
        row = paeth_pred(&l16, &top, &tl16);
        _mm256_storeu_si256((__m256i *)dst, row);
        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_16x8_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                              const uint16_t *left, int bd) {
    const __m256i tl16 = _mm256_set1_epi16(above[-1]);
    const __m256i top  = _mm256_loadu_si256((const __m256i *)above);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 8; ++i) {
        l16 = _mm256_set1_epi16(left[i]);
        row = paeth_pred(&l16, &top, &tl16);
        _mm256_storeu_si256((__m256i *)dst, row);
        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_16x16_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                               const uint16_t *left, int bd) {
    const __m256i tl16 = _mm256_set1_epi16(above[-1]);
    const __m256i top  = _mm256_loadu_si256((const __m256i *)above);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 16; ++i) {
        l16 = _mm256_set1_epi16(left[i]);
        row = paeth_pred(&l16, &top, &tl16);
        _mm256_storeu_si256((__m256i *)dst, row);
        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_16x32_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                               const uint16_t *left, int bd) {
    const __m256i tl16 = _mm256_set1_epi16(above[-1]);
    const __m256i top  = _mm256_loadu_si256((const __m256i *)above);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 32; ++i) {
        l16 = _mm256_set1_epi16(left[i]);
        row = paeth_pred(&l16, &top, &tl16);
        _mm256_storeu_si256((__m256i *)dst, row);
        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_16x64_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                               const uint16_t *left, int bd) {
    const __m256i tl16 = _mm256_set1_epi16(above[-1]);
    const __m256i top  = _mm256_loadu_si256((const __m256i *)above);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 64; ++i) {
        l16 = _mm256_set1_epi16(left[i]);
        row = paeth_pred(&l16, &top, &tl16);
        _mm256_storeu_si256((__m256i *)dst, row);
        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_32x8_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                              const uint16_t *left, int bd) {
    const __m256i t0 = _mm256_loadu_si256((const __m256i *)above);
    const __m256i t1 = _mm256_loadu_si256((const __m256i *)(above + 16));
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 8; ++i) {
        l16 = _mm256_set1_epi16(left[i]);

        row = paeth_pred(&l16, &t0, &tl);
        _mm256_storeu_si256((__m256i *)dst, row);

        row = paeth_pred(&l16, &t1, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 16), row);

        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_32x16_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                               const uint16_t *left, int bd) {
    const __m256i t0 = _mm256_loadu_si256((const __m256i *)above);
    const __m256i t1 = _mm256_loadu_si256((const __m256i *)(above + 16));
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 16; ++i) {
        l16 = _mm256_set1_epi16(left[i]);

        row = paeth_pred(&l16, &t0, &tl);
        _mm256_storeu_si256((__m256i *)dst, row);

        row = paeth_pred(&l16, &t1, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 16), row);

        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_32x32_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                               const uint16_t *left, int bd) {
    const __m256i t0 = _mm256_loadu_si256((const __m256i *)above);
    const __m256i t1 = _mm256_loadu_si256((const __m256i *)(above + 16));
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 32; ++i) {
        l16 = _mm256_set1_epi16(left[i]);

        row = paeth_pred(&l16, &t0, &tl);
        _mm256_storeu_si256((__m256i *)dst, row);

        row = paeth_pred(&l16, &t1, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 16), row);

        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_32x64_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                               const uint16_t *left, int bd) {
    const __m256i t0 = _mm256_loadu_si256((const __m256i *)above);
    const __m256i t1 = _mm256_loadu_si256((const __m256i *)(above + 16));
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 64; ++i) {
        l16 = _mm256_set1_epi16(left[i]);

        row = paeth_pred(&l16, &t0, &tl);
        _mm256_storeu_si256((__m256i *)dst, row);

        row = paeth_pred(&l16, &t1, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 16), row);

        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_64x16_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                               const uint16_t *left, int bd) {
    const __m256i t0 = _mm256_loadu_si256((const __m256i *)above);
    const __m256i t1 = _mm256_loadu_si256((const __m256i *)(above + 16));
    const __m256i t2 = _mm256_loadu_si256((const __m256i *)(above + 32));
    const __m256i t3 = _mm256_loadu_si256((const __m256i *)(above + 48));
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 16; ++i) {
        l16 = _mm256_set1_epi16(left[i]);

        row = paeth_pred(&l16, &t0, &tl);
        _mm256_storeu_si256((__m256i *)dst, row);

        row = paeth_pred(&l16, &t1, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 16), row);

        row = paeth_pred(&l16, &t2, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 32), row);

        row = paeth_pred(&l16, &t3, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 48), row);

        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_64x32_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                               const uint16_t *left, int bd) {
    const __m256i t0 = _mm256_loadu_si256((const __m256i *)above);
    const __m256i t1 = _mm256_loadu_si256((const __m256i *)(above + 16));
    const __m256i t2 = _mm256_loadu_si256((const __m256i *)(above + 32));
    const __m256i t3 = _mm256_loadu_si256((const __m256i *)(above + 48));
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 32; ++i) {
        l16 = _mm256_set1_epi16(left[i]);

        row = paeth_pred(&l16, &t0, &tl);
        _mm256_storeu_si256((__m256i *)dst, row);

        row = paeth_pred(&l16, &t1, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 16), row);

        row = paeth_pred(&l16, &t2, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 32), row);

        row = paeth_pred(&l16, &t3, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 48), row);

        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_64x64_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                               const uint16_t *left, int bd) {
    const __m256i t0 = _mm256_loadu_si256((const __m256i *)above);
    const __m256i t1 = _mm256_loadu_si256((const __m256i *)(above + 16));
    const __m256i t2 = _mm256_loadu_si256((const __m256i *)(above + 32));
    const __m256i t3 = _mm256_loadu_si256((const __m256i *)(above + 48));
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 64; ++i) {
        l16 = _mm256_set1_epi16(left[i]);

        row = paeth_pred(&l16, &t0, &tl);
        _mm256_storeu_si256((__m256i *)dst, row);

        row = paeth_pred(&l16, &t1, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 16), row);

        row = paeth_pred(&l16, &t2, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 32), row);

        row = paeth_pred(&l16, &t3, &tl);
        _mm256_storeu_si256((__m256i *)(dst + 48), row);

        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_8x4_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                             const uint16_t *left, int bd) {
    const __m128i t  = _mm_loadu_si128((const __m128i *)above);
    const __m256i t0 = _mm256_setr_m128i(t, t);
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 4; i += 2) {
        l16 = _mm256_setr_m128i(_mm_set1_epi16(left[i]), _mm_set1_epi16(left[i + 1]));

        row = paeth_pred(&l16, &t0, &tl);
        _mm_storeu_si128((__m128i *)dst, _mm256_castsi256_si128(row));
        dst += stride;
        _mm_storeu_si128((__m128i *)dst, _mm256_extractf128_si256(row, 1));
        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_8x8_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                             const uint16_t *left, int bd) {
    const __m128i t  = _mm_loadu_si128((const __m128i *)above);
    const __m256i t0 = _mm256_setr_m128i(t, t);
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 8; i += 2) {
        l16 = _mm256_setr_m128i(_mm_set1_epi16(left[i]), _mm_set1_epi16(left[i + 1]));

        row = paeth_pred(&l16, &t0, &tl);
        _mm_storeu_si128((__m128i *)dst, _mm256_castsi256_si128(row));
        dst += stride;
        _mm_storeu_si128((__m128i *)dst, _mm256_extractf128_si256(row, 1));
        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_8x16_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                              const uint16_t *left, int bd) {
    const __m128i t  = _mm_loadu_si128((const __m128i *)above);
    const __m256i t0 = _mm256_setr_m128i(t, t);
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 16; i += 2) {
        l16 = _mm256_setr_m128i(_mm_set1_epi16(left[i]), _mm_set1_epi16(left[i + 1]));

        row = paeth_pred(&l16, &t0, &tl);
        _mm_storeu_si128((__m128i *)dst, _mm256_castsi256_si128(row));
        dst += stride;
        _mm_storeu_si128((__m128i *)dst, _mm256_extractf128_si256(row, 1));
        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_8x32_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                              const uint16_t *left, int bd) {
    const __m128i t  = _mm_loadu_si128((const __m128i *)above);
    const __m256i t0 = _mm256_setr_m128i(t, t);
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 32; i += 2) {
        l16 = _mm256_setr_m128i(_mm_set1_epi16(left[i]), _mm_set1_epi16(left[i + 1]));

        row = paeth_pred(&l16, &t0, &tl);
        _mm_storeu_si128((__m128i *)dst, _mm256_castsi256_si128(row));
        dst += stride;
        _mm_storeu_si128((__m128i *)dst, _mm256_extractf128_si256(row, 1));
        dst += stride;
    }
}

void svt_aom_highbd_paeth_predictor_4x4_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                             const uint16_t *left, int bd) {
    const __m256i t0 = _mm256_set1_epi64x(((uint64_t *)above)[0]);
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    (void)bd;

    /* l16 = left: 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 */
    __m256i t1  = _mm256_cvtepi16_epi64(_mm_lddqu_si128((__m128i const *)left));
    __m256i t1s = _mm256_slli_epi64(t1, 16);
    t1          = _mm256_or_si256(t1s, t1);
    t1s         = _mm256_slli_epi64(t1, 32);
    l16         = _mm256_or_si256(t1s, t1);

    row = paeth_pred(&l16, &t0, &tl);

    *(uint64_t *)&dst[0 * stride] = _mm256_extract_epi64(row, 0);
    *(uint64_t *)&dst[1 * stride] = _mm256_extract_epi64(row, 1);
    *(uint64_t *)&dst[2 * stride] = _mm256_extract_epi64(row, 2);
    *(uint64_t *)&dst[3 * stride] = _mm256_extract_epi64(row, 3);
}

void svt_aom_highbd_paeth_predictor_4x8_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                             const uint16_t *left, int bd) {
    const __m256i t0 = _mm256_set1_epi64x(((uint64_t *)above)[0]);
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;
    int           i;
    (void)bd;

    for (i = 0; i < 8; i += 4) {
        /* l16 = left: 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 */
        __m256i t1  = _mm256_cvtepi16_epi64(_mm_lddqu_si128((__m128i const *)&left[i]));
        __m256i t1s = _mm256_slli_epi64(t1, 16);
        t1          = _mm256_or_si256(t1s, t1);
        t1s         = _mm256_slli_epi64(t1, 32);
        l16         = _mm256_or_si256(t1s, t1);

        row = paeth_pred(&l16, &t0, &tl);

        *(uint64_t *)&dst[0 * stride] = _mm256_extract_epi64(row, 0);
        *(uint64_t *)&dst[1 * stride] = _mm256_extract_epi64(row, 1);
        *(uint64_t *)&dst[2 * stride] = _mm256_extract_epi64(row, 2);
        *(uint64_t *)&dst[3 * stride] = _mm256_extract_epi64(row, 3);
        dst += 4 * stride;
    }
}

void svt_aom_highbd_paeth_predictor_4x16_avx2(uint16_t *dst, ptrdiff_t stride, const uint16_t *above,
                                              const uint16_t *left, int bd) {
    const __m256i t0 = _mm256_set1_epi64x(((uint64_t *)above)[0]);
    const __m256i tl = _mm256_set1_epi16(above[-1]);
    __m256i       l16, row;

    (void)bd;
    int i;
    for (i = 0; i < 16; i += 4) {
        /* l16 = left: 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3 */
        __m256i t1  = _mm256_cvtepi16_epi64(_mm_lddqu_si128((__m128i const *)&left[i]));
        __m256i t1s = _mm256_slli_epi64(t1, 16);
        t1          = _mm256_or_si256(t1s, t1);
        t1s         = _mm256_slli_epi64(t1, 32);
        l16         = _mm256_or_si256(t1s, t1);

        row = paeth_pred(&l16, &t0, &tl);

        *(uint64_t *)&dst[0 * stride] = _mm256_extract_epi64(row, 0);
        *(uint64_t *)&dst[1 * stride] = _mm256_extract_epi64(row, 1);
        *(uint64_t *)&dst[2 * stride] = _mm256_extract_epi64(row, 2);
        *(uint64_t *)&dst[3 * stride] = _mm256_extract_epi64(row, 3);
        dst += 4 * stride;
    }
}
