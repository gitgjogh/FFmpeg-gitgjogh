/**
 * Copyright (c) 2012~2017 Jeff <163jogh@163.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdint.h>
#include <string.h>
#include "vf_drm.h"

#define bool int
#ifndef abs
#define abs(x) ((x)<0 ? -(x) : (x))
#endif


/****************************************************************************
 * 8x8 Quant matrix init
 ****************************************************************************/

static int quant8_mf[6][8][8];
static int dequant8_mf[6][8][8];

static void x264m_8x8_cqm_init()
{
    static const int quant8_scan[16] = {
        0,3,4,3, 3,1,5,1, 4,5,2,5, 3,1,5,1
    };
    static const int dequant8_scale[6][6] = {
        { 20, 18, 32, 19, 25, 24 },
        { 22, 19, 35, 21, 28, 26 },
        { 26, 23, 42, 24, 33, 31 },
        { 28, 25, 45, 26, 35, 33 },
        { 32, 28, 51, 30, 40, 38 },
        { 36, 32, 58, 34, 46, 43 },
    };
    static const int quant8_scale[6][6] = {
        { 13107, 11428, 20972, 12222, 16777, 15481 },
        { 11916, 10826, 19174, 11058, 14980, 14290 },
        { 10082,  8943, 15978,  9675, 12710, 11985 },
        {  9362,  8228, 14913,  8931, 11984, 11259 },
        {  8192,  7346, 13159,  7740, 10486,  9777 },
        {  7282,  6428, 11570,  6830,  9118,  8640 }
    };

    for( int q = 0; q < 6; q++ ) {
        for( int i = 0; i < 64; i++ ) {
            int j = quant8_scan[((i>>1)&12) | (i&3)];
            dequant8_mf[q][0][i] = dequant8_scale[q][j];
            quant8_mf[q][0][i] = quant8_scale[q][j];
        }
    }
}

/****************************************************************************
 * 8x8 integer DCT transform:
 ****************************************************************************/

#define DCT8_1D {\
    const int s07 = SRC(0) + SRC(7);\
    const int s16 = SRC(1) + SRC(6);\
    const int s25 = SRC(2) + SRC(5);\
    const int s34 = SRC(3) + SRC(4);\
    const int a0 = s07 + s34;\
    const int a1 = s16 + s25;\
    const int a2 = s07 - s34;\
    const int a3 = s16 - s25;\
    const int d07 = SRC(0) - SRC(7);\
    const int d16 = SRC(1) - SRC(6);\
    const int d25 = SRC(2) - SRC(5);\
    const int d34 = SRC(3) - SRC(4);\
    const int a4 = d16 + d25 + (d07 + (d07>>1));\
    const int a5 = d07 - d34 - (d25 + (d25>>1));\
    const int a6 = d07 + d34 - (d16 + (d16>>1));\
    const int a7 = d16 - d25 + (d34 + (d34>>1));\
    DST(0) =  a0 + a1     ;\
    DST(1) =  a4 + (a7>>2);\
    DST(2) =  a2 + (a3>>1);\
    DST(3) =  a5 + (a6>>2);\
    DST(4) =  a0 - a1     ;\
    DST(5) =  a6 - (a5>>2);\
    DST(6) = (a2>>1) - a3 ;\
    DST(7) = (a4>>2) - a7 ;\
}

static void x264m_8x8_dct( int16_t pix[8][8], int16_t dct[8][8] )
{
    int i;

#define SRC(x) pix[x][i]
#define DST(x) pix[x][i]
    for( i = 0; i < 8; i++ )
        DCT8_1D
#undef SRC
#undef DST

#define SRC(x) pix[i][x]
#define DST(x) dct[x][i]
    for( i = 0; i < 8; i++ )
        DCT8_1D
#undef SRC
#undef DST
}

/****************************************************************************
 * 8X8 Quant
 ****************************************************************************/

#define QUANT_ONE( coef, mf ) \
{ \
    if( (coef) > 0 ) \
        (coef) = ( f + (coef) * (mf) ) >> i_qbits; \
    else \
        (coef) = - ( ( f - (coef) * (mf) ) >> i_qbits ); \
}

static void quant_8x8_core( int16_t dct[8][8], int quant_mf[8][8], int i_qbits, int f )
{
    for(int i = 0; i < 64; i++ )
        QUANT_ONE( dct[0][i], quant_mf[0][i] );
}

static void x264m_8x8_quant( int16_t dct[8][8], int i_qp )
{
    const int i_qbits = 16 + i_qp / 6;
    const int i_mf = i_qp % 6;
    const int f = ( 1 << i_qp ) / 6 ;
    quant_8x8_core( dct, quant8_mf[i_mf], i_qbits, f );
}

/****************************************************************************
 * 8x8 dequant
 ****************************************************************************/

#define DEQUANT_SHL( x ) \
    dct[y][x] = ( dct[y][x] * dequant8_mf[i_mf][y][x] ) << i_qbits

#define DEQUANT_SHR( x ) \
    dct[y][x] = ( dct[y][x] * dequant8_mf[i_mf][y][x] + f ) >> (-i_qbits)

static void x264m_8x8_dequant( int16_t dct[8][8], int i_qp )
{
    const int i_mf = i_qp%6;
    const int i_qbits = i_qp/6-2;
    int y;

    if( i_qbits >= 0 )
    {
        for( y = 0; y < 8; y++ )
        {
            DEQUANT_SHL( 0 );
            DEQUANT_SHL( 1 );
            DEQUANT_SHL( 2 );
            DEQUANT_SHL( 3 );
            DEQUANT_SHL( 4 );
            DEQUANT_SHL( 5 );
            DEQUANT_SHL( 6 );
            DEQUANT_SHL( 7 );
        }
    }
    else
    {
        const int f = 1 << (-i_qbits-1);
        for( y = 0; y < 8; y++ )
        {
            DEQUANT_SHR( 0 );
            DEQUANT_SHR( 1 );
            DEQUANT_SHR( 2 );
            DEQUANT_SHR( 3 );
            DEQUANT_SHR( 4 );
            DEQUANT_SHR( 5 );
            DEQUANT_SHR( 6 );
            DEQUANT_SHR( 7 );
        }
    }
}

/****************************************************************************
 * 8x8 inverse integer DCT transform
 ****************************************************************************/

#define IDCT8_1D {\
    const int a0 =  SRC(0) + SRC(4);\
    const int a2 =  SRC(0) - SRC(4);\
    const int a4 = (SRC(2)>>1) - SRC(6);\
    const int a6 = (SRC(6)>>1) + SRC(2);\
    const int b0 = a0 + a6;\
    const int b2 = a2 + a4;\
    const int b4 = a2 - a4;\
    const int b6 = a0 - a6;\
    const int a1 = -SRC(3) + SRC(5) - SRC(7) - (SRC(7)>>1);\
    const int a3 =  SRC(1) + SRC(7) - SRC(3) - (SRC(3)>>1);\
    const int a5 = -SRC(1) + SRC(7) + SRC(5) + (SRC(5)>>1);\
    const int a7 =  SRC(3) + SRC(5) + SRC(1) + (SRC(1)>>1);\
    const int b1 = (a7>>2) + a1;\
    const int b3 =  a3 + (a5>>2);\
    const int b5 = (a3>>2) - a5;\
    const int b7 =  a7 - (a1>>2);\
    DST(0, b0 + b7);\
    DST(1, b2 + b5);\
    DST(2, b4 + b3);\
    DST(3, b6 + b1);\
    DST(4, b6 - b1);\
    DST(5, b4 - b3);\
    DST(6, b2 - b5);\
    DST(7, b0 - b7);\
}

static inline int16_t clip_uint8( int16_t a )
{
    return (a&(~255)) ? (-a)>>31 : a;
}

static void x264m_8x8_idct( int16_t dct[8][8], int16_t pix[8][8] )
{
    int i;

    //dct[0][0] += 32; // rounding for the >>6 at the end

#define SRC(x)     dct[x][i]
#define DST(x,rhs) dct[x][i] = (rhs)
    for( i = 0; i < 8; i++ )
        IDCT8_1D
#undef SRC
#undef DST

#define SRC(x)     dct[i][x]
#define DST(x,rhs) pix[x][i] = clip_uint8( (rhs) >> 6 );
    for( i = 0; i < 8; i++ )
        IDCT8_1D
#undef SRC
#undef DST
}

/****************************************************************************
 * x264 wrapper
 ****************************************************************************/

static void dct_init()
{
    x264m_8x8_cqm_init();
}

static void dct8x8(int16_t pix[8][8], int16_t dct[8][8])
{
    x264m_8x8_dct( pix, dct );
    x264m_8x8_quant( dct, 4 );
}

static void idct8x8(int16_t dct[8][8], int16_t pix[8][8])
{
    x264m_8x8_dequant( dct, 4 );
    x264m_8x8_idct( dct, pix );
}

/****************************************************************************
 * dither modulation
 ****************************************************************************/

/**
 * 设x为载体信号，m(0 or 1)为调制量（即嵌入信息），则
 *  x' = round( (x+d[m])/q ) * q - d[m]
 */
inline int dither_modulate(int x, int q, int d)
{
    return (x + d + q/2)/q * q - d;
}

inline int binary_dm(int x, bool emb_bit, int q /*dm_step*/)
{
    int d = q / 4;
    return dither_modulate(x, q, emb_bit ? -d : d);
}

inline int binary_dm_decision(int x, int dm_step)
{
    int v_dm0 = binary_dm(x, 0, dm_step);
    int v_dm1 = binary_dm(x, 1, dm_step);
    return abs(v_dm1-x) < abs(v_dm0-x);
}

static void dct8_dcdm_emb(int16_t pix[8][8], bool emb_bit, int dm_step)
{
    int16_t dct[8][8];
    dct8x8(pix, dct);
    dct[0][0] = binary_dm(dct[0][0], emb_bit, dm_step);
    idct8x8(dct, pix);
}

static int dct8_dcdm_dec(int16_t pix[8][8], int dm_step)
{
    int16_t dct[8][8];

    dct8x8(pix, dct);
    return binary_dm_decision(dct[0][0], dm_step);
}

static void read_one_8x8(uint8_t *ptr, int stride, int16_t pix[8][8])
{
    for (int y=0; y<8; y++) {
        int16_t *dst = pix[y];
        uint8_t *src = ptr + y * stride;
        for (int x=0; x<8; x++) {
            *dst++ = *src++;
        }
    }
}

static void write_one_8x8(uint8_t *ptr, int stride, int16_t pix[8][8])
{
    for (int y=0; y<8; y++) {
        int16_t *src = pix[y];
        uint8_t *dst = ptr + y * stride;
        for (int x=0; x<8; x++) {
            *dst++ = *src++;
        }
    }
}

void dcdm(int b_emb, int dm_step, int xshift, int yshift,
        /*main*/    uint8_t *base1, int stride1, int w1, int h1,
        /*drm */    uint8_t *base2, int stride2, int w2, int h2)
{
    const int N = 8;
    int16_t pix[N][N];

    dct_init();

    for (int y1=0; y1<h1; y1+=N) 
    {    
        for (int x1=0; x1<w1; x1+=N) 
        {
            int y2 = ((yshift + y1) / N) % w2;
            int x2 = ((xshift + x1) / N) % w2;
            uint8_t *ptr2 = base2 + y2 * stride2 + x2;
            uint8_t *ptr1 = base1 + y1 * stride1 + x1;

            read_one_8x8(ptr1, stride1, pix);
            if (b_emb == DRM_EMBEDDING) {
                bool emb_bit = (*ptr2) > 128;
                dct8_dcdm_emb(pix, emb_bit, dm_step);
                write_one_8x8(ptr1, stride1, pix);
            } else {
                bool emb_bit = dct8_dcdm_dec(pix, dm_step);
                *ptr2 = emb_bit ? 255 : 0;
            }
        }
    }
}

void dcdm2(int b_emb, drm_plane_t *main, drm_plane_t *drm,
        int dm_step, int xshift, int yshift)
{
   return dcdm(b_emb, dm_step, xshift, yshift,
        main->base, main->stride, main->w, main->h,
        drm->base, drm->stride, drm->w, drm->h);
}
