/*
 * a very simple circular buffer FIFO implementation
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
 * Copyright (c) 2006 Roman Shaposhnik
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

#include "avassert.h"
#include "common.h"
#include "fifo.h"

static AVFifoBuffer *fifo_alloc_common(void *buffer, size_t size)
{
    AVFifoBuffer *f;
    if (!buffer)
        return NULL;
    f = av_mallocz(sizeof(AVFifoBuffer));
    if (!f) {
        av_free(buffer);
        return NULL;
    }
    f->buffer = buffer;
    f->end    = f->buffer + size;
    av_fifo_reset(f);
    return f;
}

AVFifoBuffer *av_fifo_alloc(unsigned int size)
{
    void *buffer = av_malloc(size);
    return fifo_alloc_common(buffer, size);
}

AVFifoBuffer *av_fifo_alloc_array(size_t nmemb, size_t size)
{
    void *buffer = av_malloc_array(nmemb, size);
    return fifo_alloc_common(buffer, nmemb * size);
}

void av_fifo_free(AVFifoBuffer *f)
{
    if (f) {
        av_freep(&f->buffer);
        av_free(f);
    }
}

void av_fifo_freep(AVFifoBuffer **f)
{
    if (f) {
        av_fifo_free(*f);
        *f = NULL;
    }
}

void av_fifo_reset(AVFifoBuffer *f)
{
    f->wptr = f->rptr = f->buffer;
    f->wndx = f->rndx = 0;
}

int av_fifo_size(const AVFifoBuffer *f)
{
    return (uint32_t)(f->wndx - f->rndx);
}

int av_fifo_space(const AVFifoBuffer *f)
{
    return f->end - f->buffer - av_fifo_size(f);
}

int av_fifo_realloc2(AVFifoBuffer *f, unsigned int new_size)
{
    unsigned int old_size = f->end - f->buffer;

    if (old_size < new_size) {
        int len          = av_fifo_size(f);
        AVFifoBuffer *f2 = av_fifo_alloc(new_size);

        if (!f2)
            return AVERROR(ENOMEM);
        av_fifo_generic_read(f, f2->buffer, len, NULL);
        f2->wptr += len;
        f2->wndx += len;
        av_free(f->buffer);
        *f = *f2;
        av_free(f2);
    }
    return 0;
}

int av_fifo_grow(AVFifoBuffer *f, unsigned int size)
{
    unsigned int old_size = f->end - f->buffer;
    if(size + (unsigned)av_fifo_size(f) < size)
        return AVERROR(EINVAL);

    size += av_fifo_size(f);

    if (old_size < size)
        return av_fifo_realloc2(f, FFMAX(size, 2*size));
    return 0;
}

/* src must NOT be const as it can be a context for func that may need
 * updating (like a pointer or byte counter) */
int av_fifo_generic_write(AVFifoBuffer *f, void *src, int size,
                          int (*func)(void *, void *, int))
{
    int total = size;
    uint32_t wndx= f->wndx;
    uint8_t *wptr= f->wptr;

    do {
        int len = FFMIN(f->end - wptr, size);
        if (func) {
            len = func(src, wptr, len);
            if (len <= 0)
                break;
        } else {
            memcpy(wptr, src, len);
            src = (uint8_t *)src + len;
        }
// Write memory barrier needed for SMP here in theory
        wptr += len;
        if (wptr >= f->end)
            wptr = f->buffer;
        wndx    += len;
        size    -= len;
    } while (size > 0);
    f->wndx= wndx;
    f->wptr= wptr;
    return total - size;
}

int av_fifo_generic_peek(AVFifoBuffer *f, void *dest, int buf_size,
                         void (*func)(void *, void *, int))
{
// Read memory barrier needed for SMP here in theory
    uint8_t *rptr = f->rptr;
    uint32_t rndx = f->rndx;

    do {
        int len = FFMIN(f->end - f->rptr, buf_size);
        if (func)
            func(dest, f->rptr, len);
        else {
            memcpy(dest, f->rptr, len);
            dest = (uint8_t *)dest + len;
        }
// memory barrier needed for SMP here in theory
        av_fifo_drain(f, len);
        buf_size -= len;
    } while (buf_size > 0);

    f->rptr = rptr;
    f->rndx = rndx;

    return 0;
}

int av_fifo_generic_read(AVFifoBuffer *f, void *dest, int buf_size,
                         void (*func)(void *, void *, int))
{
// Read memory barrier needed for SMP here in theory
    do {
        int len = FFMIN(f->end - f->rptr, buf_size);
        if (func)
            func(dest, f->rptr, len);
        else {
            memcpy(dest, f->rptr, len);
            dest = (uint8_t *)dest + len;
        }
// memory barrier needed for SMP here in theory
        av_fifo_drain(f, len);
        buf_size -= len;
    } while (buf_size > 0);
    return 0;
}

/** Discard data from the FIFO. */
void av_fifo_drain(AVFifoBuffer *f, int size)
{
    av_assert2(av_fifo_size(f) >= size);
    f->rptr += size;
    if (f->rptr >= f->end)
        f->rptr -= f->end - f->buffer;
    f->rndx += size;
}

static int av_fifo_mcpy_peek(/*AVFifoBuffer **/ void *f, void *dest, int buf_size)
{
    buf_size = FFMIN(buf_size, av_fifo_size(f));
    //printf("mcpy_peek %d\n", buf_size);
    av_fifo_generic_peek(f, dest, buf_size, 0);
    return buf_size;
}

static int av_fifo_mcpy_read(/*AVFifoBuffer**/ void *f, void *dest, int buf_size)
{
    buf_size = FFMIN(buf_size, av_fifo_size(f));
    //printf("mcpy_read %d\n", buf_size);
    av_fifo_generic_read(f, dest, buf_size, 0);
    return buf_size;
}

void av_fifo_peek_copy(AVFifoBuffer *dst, AVFifoBuffer *src, int size)
{
    size = FFMIN(size, av_fifo_space(dst));
    //printf("peek_copy %d\n", size);
    av_fifo_generic_write(dst, src, size, av_fifo_mcpy_peek);
}

void av_fifo_read_copy(AVFifoBuffer *dst, AVFifoBuffer *src, int size)
{
    size = FFMIN(size, av_fifo_space(dst));
    //printf("read_copy %d\n", size);
    av_fifo_generic_write(dst, src, size, av_fifo_mcpy_read);
}


#ifdef TEST

/**
 * $ under ffmpeg/libavutil/
 * cc -DTEST fifo.c -I ../ -L???/ffmpeg/libavutil/ -lavutil
 */
static int copy_test()
{
    /* create a FIFO buffer */
    AVFifoBuffer *fifo1 = av_fifo_alloc(12 * sizeof(int));
    AVFifoBuffer *fifo2 = av_fifo_alloc(12 * sizeof(int));
    int i, j, n;

    /* fill data */
    for (i = 0; av_fifo_space(fifo1) >= sizeof(int); i++)
        av_fifo_generic_write(fifo1, &i, sizeof(int), NULL);
    for (i++; av_fifo_size(fifo2) < av_fifo_size(fifo1)/3; i++)
        av_fifo_generic_write(fifo2, &i, sizeof(int), NULL);

    av_fifo_peek_copy(fifo2, fifo1, av_fifo_size(fifo1)/3);
    /* self copy should be fine */
    av_fifo_read_copy(fifo2, fifo2, av_fifo_size(fifo1)/3);
    av_fifo_peek_copy(fifo2, fifo1, av_fifo_size(fifo1)/3);
        
    printf("FIFO_1: ");
    for (i = 0; av_fifo_size(fifo1) >= sizeof(int); i++) {
        av_fifo_generic_read(fifo1, &j, sizeof(int), NULL);
        printf("%2d ", j);
    }
    printf("\n");

    printf("FIFO_2: ");
    for (i = 0; av_fifo_size(fifo2) >= sizeof(int); i++) {
        av_fifo_generic_read(fifo2, &j, sizeof(int), NULL);
        printf("%2d ", j);
    }
    printf("\n");

    av_fifo_free(fifo1);
    av_fifo_free(fifo2);
}

int main(void)
{
    /* create a FIFO buffer */
    AVFifoBuffer *fifo = av_fifo_alloc(13 * sizeof(int));
    int i, j, n;

    /* fill data */
    for (i = 0; av_fifo_space(fifo) >= sizeof(int); i++)
        av_fifo_generic_write(fifo, &i, sizeof(int), NULL);

    /* peek at FIFO */
    n = av_fifo_size(fifo) / sizeof(int);
    for (i = -n + 1; i < n; i++) {
        int *v = (int *)av_fifo_peek2(fifo, i * sizeof(int));
        printf("%d: %d\n", i, *v);
    }
    printf("\n");

    /* read data */
    for (i = 0; av_fifo_size(fifo) >= sizeof(int); i++) {
        av_fifo_generic_read(fifo, &j, sizeof(int), NULL);
        printf("%d ", j);
    }
    printf("\n");

    av_fifo_free(fifo);

    printf("\n\ncopy_test:\n");
    copy_test();

    return 0;
}

#endif
