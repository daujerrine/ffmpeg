/*
 * Range coder for FLIF16
 * Copyright (c) 2004, Michael Niedermayer,
 *               2010-2016, Jon Sneyers & Pieter Wuille,
 *               2020, Anamitra Ghorui <aghorui@teknik.io>
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

 /**
  * @file
  * Range coder for FLIF16
  */

#include "avcodec.h"
#include "libavutil/common.h"
#include "flif16_rangecoder.h"
#include "flif16.h"

// TODO write separate function for RAC encoder

// The coder requires a certain number of bytes for initiialization. buf
// provides it. gb is used by the coder functions for actual coding.
void ff_flif16_rac_init(FLIF16RangeCoder *rc, GetByteContext *gb, uint8_t *buf,
                        uint8_t buf_size)
{
    GetByteContext gbi;
    if(!rc)
        return;

    if(buf_size < FLIF16_RAC_MAX_RANGE_BYTES)
        return;
    
    bytestream2_init(&gbi, buf, buf_size);

    rc->range  = FLIF16_RAC_MAX_RANGE;
    rc->gb     = gb;

    for (uint32_t r = FLIF16_RAC_MAX_RANGE; r > 1; r >>= 8) {
        rc->low <<= 8;
        rc->low |= bytestream2_get_byte(&gbi);
    }
}

void ff_flif16_rac_free(FLIF16RangeCoder *rc)
{
    if (!rc)
        return;
    av_freep(rc);
}

// TODO Maybe restructure rangecoder.c/h to fit a more generic case
static void build_table(uint16_t *zero_state, uint16_t *one_state, size_t size,
                        uint32_t factor, unsigned int max_p)
{
    const int64_t one = 1LL << 32;
    int64_t p = one / 2;
    unsigned int last_p8 = 0, p8;
    unsigned int i;

    for (i = 0; i < size / 2; i++) {
        p8 = (size * p + one / 2) >> 32;
        if (p8 <= last_p8) 
            p8 = last_p8 + 1;
        if (last_p8 && last_p8 < size && p8 <= max_p)
            one_state[last_p8] = p8;
        p += ((one - p) * factor + one / 2) >> 32;
        last_p8 = p8;
    }

    for (i = size - max_p; i <= max_p; i++) {
        if (one_state[i])
            continue;
        p = (i * one + size / 2) / size;
        p += ((one - p) * factor + one / 2) >> 32;
        p8 = (size * p + one / 2) >> 32; //FIXME try without the one
        if (p8 <= i) 
            p8 = i + 1;
        if (p8 > max_p) 
            p8 = max_p;
        one_state[i] = p8;
    }

    for (i = 1; i < size; i++)
        zero_state[i] = size - one_state[size - i];
}

static inline uint32_t log4kf(int x, uint32_t base)
{
    int bits     = 8 * sizeof(int) - ff_clz(x);
    uint64_t y   = ((uint64_t)x) << (32 - bits);
    uint32_t res = base * (13 - bits);
    uint32_t add = base;
    while ((add > 1) && ((y & 0x7FFFFFFF) != 0)) {
        y = (((uint64_t)y) * y + 0x40000000) >> 31;
        add >>= 1;
        if ((y >> 32) != 0) {
            res -= add;
            y >>= 1;
        }
    }
    return res;
}

void ff_flif16_build_log4k_table(FLIF16Log4kTable *log4k)
{
    log4k->table[0] = 0;
    for (int i = 1; i < 4096; i++)
        log4k->table[i] = (log4kf(i, (65535UL << 16) / 12) + 
                          (1 << 15)) >> 16;
    log4k->scale = 65535 / 12;
}

void ff_flif16_chancetable_init(FLIF16ChanceTable *ct, int alpha, int cut)
{
    build_table(ct->zero_state, ct->one_state, 4096, alpha, 4096 - cut);
}

void ff_flif16_chancecontext_init(FLIF16ChanceContext *ctx)
{
    if(!ctx)
        return;
    memcpy(&ctx->data, &flif16_nz_int_chances, sizeof(flif16_nz_int_chances));
}

#ifdef MULTISCALE_CHANCES_ENABLED
FLIF16MultiscaleChanceTable *ff_flif16_multiscale_chancetable_init(void)
{
    unsigned int len = MULTISCALE_CHANCETABLE_DEFAULT_SIZE;
    FLIF16MultiscaleChanceTable *ct = av_malloc(sizeof(*ct));
    for (int i = 0; i < len; ++i) {
        ff_flif16_chancetable_init(&ct->sub_table[i],
                                   flif16_multiscale_alphas[i],
                                   MULTISCALE_CHANCETABLE_DEFAULT_CUT);
    }
    return ct;
}

/**
 * Allocate and set all chances according to flif16_nz_int_chances
 */
void ff_flif16_multiscale_chancecontext_init(FLIF16MultiscaleChanceContext *ctx)
{
    for (int i = 0; i < sizeof(flif16_nz_int_chances) /
                        sizeof(flif16_nz_int_chances[0]); ++i)
        ff_flif16_multiscale_chance_set(&ctx->data[i], flif16_nz_int_chances[i]);
    return ctx;
}

#endif

int ff_flif16_read_maniac_tree(FLIF16RangeCoder *rc,
                               FLIF16MANIACContext *m,
                               int32_t (*prop_ranges)[2],
                               unsigned int prop_ranges_size,
                               unsigned int channel)
{
    int oldp = 0, p = 0, split_val = 0, temp;

    switch (rc->segment2) {
        default: case 0:
            rc->segment2 = 0;
            if (!(m->forest[channel])) {
                m->forest[channel] = av_mallocz(sizeof(*(m->forest[channel])));
                if (!(m->forest[channel]))
                    return AVERROR(ENOMEM);
                m->forest[channel]->data  = av_mallocz(MANIAC_TREE_BASE_SIZE *
                                                       sizeof(*(m->forest[channel]->data)));
                if (!m->forest[channel]->data)
                    return AVERROR(ENOMEM);

                m->stack = av_mallocz(MANIAC_TREE_BASE_SIZE * sizeof(*(m->stack)));
            
                if (!(m->stack))
                    return AVERROR(ENOMEM);

                for (int i = 0; i < 3; ++i) {
                    #ifdef MULTISCALE_CHANCES_ENABLED
                    ff_flif16_multiscale_chancecontext_init(&m->ctx[i]);
                    #else
                    ff_flif16_chancecontext_init(&m->ctx[i]);
                    #endif
                }
                m->stack_top = m->tree_top = 0;
                m->forest[channel]->size    = MANIAC_TREE_BASE_SIZE;
                m->stack_size = MANIAC_TREE_BASE_SIZE;
                m->stack[m->stack_top].id   = m->tree_top;
                m->stack[m->stack_top].mode = 0;
                ++m->stack_top;
                ++m->tree_top;
            }
            ++rc->segment2;
        
        case 1:
            start:
            if(!m->stack_top)
                goto end;

            oldp = m->stack[m->stack_top - 1].p;
            if (!m->stack[m->stack_top - 1].visited) {
                switch (m->stack[m->stack_top - 1].mode) {
                    case 1:
                        prop_ranges[oldp][0] = m->stack[m->stack_top - 1].min;
                        prop_ranges[oldp][1] = m->stack[m->stack_top - 1].max;
                        break;

                    case 2:
                        prop_ranges[oldp][0] = m->stack[m->stack_top - 1].min;
                        break;
                }
            } else {
                prop_ranges[oldp][1] = m->stack[m->stack_top - 1].max2;
                --m->stack_top;
                rc->segment2 = 1;
                goto start;
            }
            m->stack[m->stack_top - 1].visited = 1;
            ++rc->segment2;

        case 2:
            #ifdef MULTISCALE_CHANCES_ENABLED
            RAC_GET(rc, &m->ctx[0], 0, prop_ranges_size,
                    &m->forest[channel]->data[m->stack[m->stack_top - 1].id].property,
                    FLIF16_RAC_GNZ_MULTISCALE_INT);
            #else
            RAC_GET(rc, &m->ctx[0], 0, prop_ranges_size,
                    &m->forest[channel]->data[m->stack[m->stack_top - 1].id].property,
                    FLIF16_RAC_GNZ_INT);
            #endif
            p = --(m->forest[channel]->data[m->stack[m->stack_top - 1].id].property);
            if (p == -1) {
                --m->stack_top;
                rc->segment2 = 1;
                goto start;
            }

            m->forest[channel]->data[m->stack[m->stack_top - 1].id].child_id = m->tree_top;
            rc->oldmin = prop_ranges[p][0];
            rc->oldmax = prop_ranges[p][1];
            if (rc->oldmin >= rc->oldmax) {
                printf("!!! rc->oldmin >= rc->oldmax\n");
                return AVERROR(EINVAL);
            }
            ++rc->segment2;

        case 3:
            #ifdef MULTISCALE_CHANCES_ENABLED
            RAC_GET(rc, &m->ctx[1], MANIAC_TREE_MIN_COUNT, MANIAC_TREE_MAX_COUNT,
                    &m->forest[channel]->data[m->stack[m->stack_top - 1].id].count,
                    FLIF16_RAC_GNZ_MULTISCALE_INT);
            #else
            RAC_GET(rc, &m->ctx[1], MANIAC_TREE_MIN_COUNT, MANIAC_TREE_MAX_COUNT,
                    &m->forest[channel]->data[m->stack[m->stack_top - 1].id].count,
                    FLIF16_RAC_GNZ_INT);
            #endif
            ++rc->segment2;

        case 4:
            #ifdef MULTISCALE_CHANCES_ENABLED
            RAC_GET(rc, &m->ctx[2], rc->oldmin, rc->oldmax - 1,
                    &m->forest[channel]->data[m->stack[m->stack_top - 1].id].split_val,
                    FLIF16_RAC_GNZ_MULTISCALE_INT);
            #else
            RAC_GET(rc, &m->ctx[2], rc->oldmin, rc->oldmax - 1,
                    &m->forest[channel]->data[m->stack[m->stack_top - 1].id].split_val,
                    FLIF16_RAC_GNZ_INT);
            #endif
            split_val = m->forest[channel]->data[m->stack[m->stack_top - 1].id].split_val;
            ++rc->segment2;

        case 5:
            if ((m->tree_top + 2) >= m->forest[channel]->size) {
                m->forest[channel]->data = av_realloc(m->forest[channel]->data,
                (m->forest[channel]->size) * 2 * sizeof(*(m->forest[channel]->data)));
                if(!(m->forest[channel]->data))
                    return AVERROR(ENOMEM);
                m->forest[channel]->size *= 2;
            }

            if ((m->stack_top + 2) >= m->stack_size) {
                m->stack = av_realloc(m->stack, (m->stack_size) * 2 * sizeof(*(m->stack)));
                if(!(m->stack))
                    return AVERROR(ENOMEM);
                m->stack_size *= 2;
            }

            temp = m->forest[channel]->data[m->stack[m->stack_top - 1].id].property;
            
            // Parent
            m->stack[m->stack_top - 1].p    = temp;
            m->stack[m->stack_top - 1].max2 = rc->oldmax;

            // Right child
            m->stack[m->stack_top].id      = m->tree_top + 1;
            m->stack[m->stack_top].p       = temp;
            m->stack[m->stack_top].min     = rc->oldmin;
            m->stack[m->stack_top].max     = split_val;
            m->stack[m->stack_top].mode    = 1;
            m->stack[m->stack_top].visited = 0;
            ++m->stack_top;
            
            // Left Child
            m->stack[m->stack_top].id      = m->tree_top;
            m->stack[m->stack_top].p       = temp;
            m->stack[m->stack_top].min     = split_val + 1;
            m->stack[m->stack_top].mode    = 2;
            m->stack[m->stack_top].visited = 0;
            ++m->stack_top;

            m->tree_top += 2;
            rc->segment2 = 1;
            goto start;
    }

    end:
    m->forest[channel]->data = av_realloc(m->forest[channel]->data, m->tree_top * sizeof(*m->forest[channel]->data)); // Maybe replace by fast realloc
    if (!m->forest[channel]->data)
        return AVERROR(ENOMEM);
    m->forest[channel]->size = m->tree_top;
    av_freep(&m->stack);
    m->stack_top = 0;
    rc->segment2 = 0;
    return 0;

    need_more_data:
    return AVERROR(EAGAIN);
}

void ff_flif16_maniac_close(FLIF16MANIACContext *m, uint8_t num_planes) 
{
    for (int i = 0; i < num_planes; ++i) {
        if (!m->forest[i])
            continue;
        if (m->forest[i]->data)
            av_freep(&m->forest[i]->data);
        if (m->forest[i]->leaves)
            av_freep(&m->forest[i]->leaves);
        av_freep(&m->forest[i]);
    }

    av_freep(&m->forest);
    // Should be already freed in maniac reading, but checking anyway.
    if(m->stack)
        av_freep(&m->stack);
}

#ifdef MULTISCALE_CHANCES_ENABLED
FLIF16MultiscaleChanceContext *ff_flif16_maniac_findleaf(FLIF16MANIACContext *m,
                                                         uint8_t channel,
                                                         int32_t *properties)
#else
FLIF16ChanceContext *ff_flif16_maniac_findleaf(FLIF16MANIACContext *m,
                                               uint8_t channel,
                                               int32_t *properties)
#endif
{
    unsigned int pos = 0;
    uint32_t old_leaf;
    uint32_t new_leaf;
    FLIF16MANIACTree *tree = m->forest[channel];
    FLIF16MANIACNode *nodes = tree->data;

    if (!m->forest[channel]->leaves) {
        m->forest[channel]->leaves = av_mallocz(MANIAC_TREE_BASE_SIZE *
                                                sizeof(*m->forest[channel]->leaves));
        m->forest[channel]->leaves_size = MANIAC_TREE_BASE_SIZE;
        if(!m->forest[channel]->leaves)
            return NULL;
        ff_flif16_chancecontext_init(&m->forest[channel]->leaves[0]);
        tree->leaves_top = 1;
    }

    while (nodes[pos].property != -1) {
        if (nodes[pos].count < 0) {
            if (properties[nodes[pos].property] > nodes[pos].split_val)
                pos = nodes[pos].child_id;
            else
                pos = nodes[pos].child_id + 1;
        } else if (nodes[pos].count > 0) {
            --nodes[pos].count;
            break;
        } else { // count == 0
            --nodes[pos].count;
            if ((tree->leaves_top) >= tree->leaves_size) {
                m->forest[channel]->leaves = av_realloc(m->forest[channel]->leaves,
                                                        sizeof(*m->forest[channel]->leaves) *
                                                        m->forest[channel]->leaves_size * 2);
                if (!m->forest[channel]->leaves)
                    return NULL;
                m->forest[channel]->leaves_size *= 2;
            }
            old_leaf = nodes[pos].leaf_id;
            new_leaf = tree->leaves_top;
            memcpy(&m->forest[channel]->leaves[tree->leaves_top],
                   &m->forest[channel]->leaves[nodes[pos].leaf_id],
                   sizeof(*m->forest[channel]->leaves));
            ++tree->leaves_top;
            nodes[nodes[pos].child_id].leaf_id = old_leaf;
            nodes[nodes[pos].child_id + 1].leaf_id = new_leaf;

            if (properties[nodes[pos].property] > nodes[pos].split_val)
                return &m->forest[channel]->leaves[old_leaf];
            else
                return &m->forest[channel]->leaves[new_leaf];
        }
    }
    return &m->forest[channel]->leaves[m->forest[channel]->data[pos].leaf_id];
}

int ff_flif16_maniac_read_int(FLIF16RangeCoder *rc,
                              FLIF16MANIACContext *m,
                              int32_t *properties,
                              uint8_t channel,
                              int min, int max, int *target)
{
    if (!rc->maniac_ctx)
        rc->segment2 = 0;

    switch(rc->segment2) {
        case 0:
            if (min == max) {
                *target = min;
                goto end;
            }
            rc->maniac_ctx = ff_flif16_maniac_findleaf(m, channel, properties);
            if(!rc->maniac_ctx) {
                return AVERROR(ENOMEM);
            }
            ++rc->segment2;

        case 1:
            #ifdef MULTISCALE_CHANCES_ENABLED
            RAC_GET(rc, rc->maniac_ctx, min, max, target, FLIF16_RAC_NZ_MULTISCALE_INT);
            #else
            RAC_GET(rc, rc->maniac_ctx, min, max, target, FLIF16_RAC_NZ_INT);
            #endif
            
    }

    end:
    rc->maniac_ctx = NULL;
    rc->segment2 = 0;
    return 1;

    need_more_data:
    return 0;
}
