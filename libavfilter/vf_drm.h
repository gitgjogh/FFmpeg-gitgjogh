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

/*
 * embedding:
 * > ffmpeg -i main.mp4 -i drmIn.bmp -filter_complex drmEmb -c:v libx264 -b:v 2M -y emb.mp4
 * extract drm to video:
 * > ffmpeg -i emb.mp4 -an -vf drmDec -c:v libx264 -b:v 200k -y drmOut.mp4
 * or extract drm to images
 * > ffmpeg -i emb.mp4 -an -vf drmDec -f image2 -y 'drmOut-%d.jpg'
 */

enum {
    DRM_DECODING,
    DRM_EMBEDDING,
};

typedef struct drm_plane_t {
    uint8_t *base;
    int     stride;
    int     w, h;
} drm_plane_t;

void drm_plane_set(uint8_t *base, int linesize, int h, int val);


#define DEFAULT_DCDM_STEP 32

void dcdm(int b_emb, int dm_step, int xshift, int yshift,
        /*main*/    uint8_t *base1, int stride1, int w1, int h1,
        /*drm */    uint8_t *base2, int stride2, int w2, int h2);

void dcdm2(int b_emb, drm_plane_t *mainpl, drm_plane_t *drmpl,
        int dm_step, int xshift, int yshift);
