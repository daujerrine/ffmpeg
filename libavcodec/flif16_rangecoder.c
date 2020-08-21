/*
 * Range coder for FLIF16
 * Copyright (c) 2020, Anamitra Ghorui <aghorui@teknik.io>
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

/**
 * Initializes the range decoder
 * @param rc Pointer to the rangecoder struct
 * @param gb Pointer to the encoded bytestream
 * @returns AVERROR(EAGAIN) on insufficient buffer, 0 on success.
 */
int ff_flif16_rac_init(FLIF16RangeCoder *rc, GetByteContext *gb)
{
    int ret = 0;

    if(bytestream2_get_bytes_left(gb) < FLIF16_RAC_MAX_RANGE_BYTES)
        ret = AVERROR(EAGAIN);

    // This is used to check whether the function is being run for the first
    // time or not.
    if (!rc->bytestream)
        rc->range = FLIF16_RAC_MAX_RANGE;
    rc->bytestream = gb;

    for (; rc->range > 1 && bytestream2_get_bytes_left(rc->bytestream) > 0;
         rc->range >>= 8) {
        rc->low <<= 8;
        rc->low |= bytestream2_get_byte(rc->bytestream);
    }

    if (rc->range <= 1)
        rc->range = FLIF16_RAC_MAX_RANGE;

    return ret;
}

static uint8_t ff_flif16_rac_get(FLIF16RangeCoder *rc, uint32_t chance,
                                 uint8_t *target)
{
    if (rc->low >= rc->range - chance) {
        rc->low -= rc->range - chance;
        rc->range = chance;
        *target = 1;
    } else {
        rc->range -= chance;
        *target = 0;
    }

    return 1;
}

uint8_t ff_flif16_rac_read_bit(FLIF16RangeCoder *rc,
                               uint8_t *target)
{
    return ff_flif16_rac_get(rc, rc->range >> 1, target);
}

uint32_t ff_flif16_rac_read_chance(FLIF16RangeCoder *rc,
                                   uint64_t b12, uint8_t *target)
{
    uint32_t ret = ((rc->range) * b12 + 0x800) >> 12;
    return ff_flif16_rac_get(rc, ret, target);
}

/**
 * Reads a Uniform Symbol Coded Integer.
 */
int ff_flif16_rac_read_uni_int(FLIF16RangeCoder *rc,
                               int min, int len,
                               void *target, int type)
{
    int med;
    uint8_t bit;

    if (!rc->active) {
        rc->min = min;
        rc->len = len;
        rc->active = 1;
    }

    if (rc->len > 0) {
        ff_flif16_rac_read_bit(rc, &bit);
        med = (rc->len) / 2;
        if (bit) {
            rc->min += med + 1;
            rc->len -= med + 1;
        } else {
            rc->len = med;
        }
        return 0;
    } else {
        switch (type) {
        case FLIF16_RAC_UNI_INT8:
            *((int8_t *) target) = rc->min;
            break;

        case FLIF16_RAC_UNI_INT16:
            *((int16_t *) target) = rc->min;
            break;

        case FLIF16_RAC_UNI_INT32:
            *((int32_t *) target) = rc->min;
            break;
        }
        rc->active = 0;
        return 1;
    }
}

// Nearzero integer definitions

#ifdef MULTISCALE_CHANCES_ENABLED
static inline void ff_flif16_chance_estim(FLIF16RangeCoder *rc,
                                          uint16_t chance, uint8_t bit,
                                          uint64_t *total)
{
    *total += rc->log4k.table[bit ? chance : 4096 - chance];
}
#endif

// NearZero Integer Coder

static inline int ff_flif16_rac_nz_read_internal(FLIF16RangeCoder *rc,
                                                 FLIF16ChanceContext *ctx,
                                                 uint16_t type, uint8_t *target)
{
    if(!ff_flif16_rac_renorm(rc))
        return 0; // EAGAIN condition
    ff_flif16_rac_read_chance(rc, ctx->data[type], target);
    ctx->data[type] = (!*target) ? rc->ct.zero_state[ctx->data[type]]
                                 : rc->ct.one_state[ctx->data[type]];
    return 1;
}

#define RAC_NZ_GET(rc, ctx, chance, target)                                    \
    if (!ff_flif16_rac_nz_read_internal((rc), (ctx), (chance),                 \
                                        (uint8_t *) (target))) {               \
        goto need_more_data;                                                   \
    }

int ff_flif16_rac_read_nz_int(FLIF16RangeCoder *rc,
                              FLIF16ChanceContext *ctx,
                              int min, int max, int *target)
{
    uint8_t temp = 0;
    if (min == max) {
        *target = min;
        rc->active = 0;
        return 1;
    }

    if (!rc->active) {
        rc->segment = 0;
        rc->amin    = 1;
        rc->active  = 1;
        rc->sign    = 0;
        rc->pos     = 0;
    }

    switch (rc->segment) {
    case 0:
        RAC_NZ_GET(rc, ctx, NZ_INT_ZERO, &temp);
        if (temp) {
            *target = 0;
            rc->active = 0;
            return 1;
        }
        rc->segment++;

    case 1:
        if (min < 0) {
            if (max > 0) {
                RAC_NZ_GET(rc, ctx, NZ_INT_SIGN, &rc->sign);
            } else {
                rc->sign = 0;
            }
        } else {
            rc->sign = 1;
        }
        rc->amax = (rc->sign ? max : -min);
        rc->emax = ff_log2(rc->amax);
        rc->e    = ff_log2(rc->amin);
        rc->segment++;

    case 2:
        for (; rc->e < rc->emax; rc->e++) {
            RAC_NZ_GET(rc, ctx, NZ_INT_EXP(((rc->e << 1) + rc->sign)), &temp);
            if (temp)
                break;
            temp = 0;
        }
        rc->have = (1 << rc->e);
        rc->left = rc->have - 1;
        rc->pos  = rc->e;
        rc->segment++;

        while (rc->pos > 0) {
            rc->pos--;
            rc->left >>= 1;
            rc->minabs1 = rc->have | (1 << rc->pos);
            rc->maxabs0 = rc->have | rc->left;

            if (rc->minabs1 > rc->amax) {
                continue;
            } else if (rc->maxabs0 >= rc->amin) {
    case 3:
                RAC_NZ_GET(rc, ctx, NZ_INT_MANT(rc->pos), &temp);
                if (temp)
                    rc->have = rc->minabs1;
                temp = 0;
            } else {
                rc->have = rc->minabs1;
            }
        }
    }
    *target = (rc->sign ? rc->have : -rc->have);
    rc->active = 0;
    return 1;

    need_more_data:
    return 0;
}

int ff_flif16_rac_read_gnz_int(FLIF16RangeCoder *rc,
                               FLIF16ChanceContext *ctx,
                               int min, int max, int *target)
{
    int ret;
    if (min > 0) {
        ret = ff_flif16_rac_read_nz_int(rc, ctx, 0, max - min, target);
        if (ret)
            *target += min;
    } else if (max < 0) {
        ret =  ff_flif16_rac_read_nz_int(rc, ctx, min - max, 0, target);
        if (ret)
            *target += max;
    } else
        ret = ff_flif16_rac_read_nz_int(rc, ctx, min, max, target);
    return ret;

}

#ifdef MULTISCALE_CHANCES_ENABLED
// Multiscale chance definitions

static inline void ff_flif16_multiscale_chance_set(FLIF16MultiscaleChance *c,
                                                   uint16_t chance)
{
    for (int i = 0; i < MULTISCALE_CHANCETABLE_DEFAULT_SIZE; i++) {
        c->chances[i] = chance;
        c->quality[i] = 0;
    }
    c->best = 0;
}

static void ff_flif16_multiscale_chancetable_put(FLIF16RangeCoder *rc,
                                                 FLIF16MultiscaleChanceContext *ctx,
                                                 uint16_t type, uint8_t bit)
{
    FLIF16MultiscaleChance *c = &ctx->data[type];
    uint64_t sbits, oqual;
    for (int i = 0; i < MULTISCALE_CHANCETABLE_DEFAULT_SIZE; i++) {
        sbits = 0;
        ff_flif16_chance_estim(rc, c->chances[i], bit, &sbits);
        oqual = c->quality[i];
        c->quality[i] = (oqual * 255 + sbits * 4097 + 128) >> 8;
        c->chances[i] = (bit) ? rc->mct->sub_table[i].one_state[c->chances[i]]
                              : rc->mct->sub_table[i].zero_state[c->chances[i]];
    }
    for (int i = 0; i < MULTISCALE_CHANCETABLE_DEFAULT_SIZE; i++)
        if (c->quality[i] < c->quality[c->best])
            c->best = i;
}

static void ff_flif16_rac_read_multiscale_symbol(FLIF16RangeCoder *rc,
                                                 FLIF16MultiscaleChanceContext *ctx,
                                                 uint16_t type, uint8_t *target)
{
    ff_flif16_rac_read_chance(rc, ctx->data[type].chances[ctx->data[type].best], target);
    ff_flif16_multiscale_chancetable_put(rc, ctx, type, *target);
}

static inline int ff_flif16_rac_nz_read_multiscale_internal(FLIF16RangeCoder *rc,
                                                            FLIF16MultiscaleChanceContext *ctx,
                                                            uint16_t type, uint8_t *target)
{
    if(!ff_flif16_rac_renorm(rc))
        return 0; // EAGAIN condition
    ff_flif16_rac_read_multiscale_symbol(rc, ctx, type, target);
    return 1;
}

#define RAC_NZ_MULTISCALE_GET(rc, ctx, chance, target)                         \
    if (!ff_flif16_rac_nz_read_multiscale_internal((rc), (ctx), (chance),      \
                                                   (uint8_t *) (target))) {    \
        goto need_more_data;                                                   \
    }

int ff_flif16_rac_read_nz_multiscale_int(FLIF16RangeCoder *rc,
                                         FLIF16MultiscaleChanceContext *ctx,
                                         int min, int max, int *target)
{
    uint8_t temp = 0;
    
    if (min == max) {
        *target = min;
        rc->active = 0;
        return 1;
    }

    if (!rc->active) {
        rc->segment = 0;
        rc->amin    = 1;
        rc->active  = 1;
        rc->sign    = 0;
        rc->have    = 0;
    }

    switch (rc->segment) {
    case 0:
        RAC_NZ_MULTISCALE_GET(rc, ctx, NZ_INT_ZERO, &temp);
        if (temp) {
            *target = 0;
            rc->active = 0;
            return 1;
        }
        rc->segment++;

    case 1:
        if (min < 0) {
            if (max > 0) {
                RAC_NZ_MULTISCALE_GET(rc, ctx, NZ_INT_SIGN, &rc->sign);
            } else {
                rc->sign = 0;
            }
        } else {
            rc->sign = 1;
        }
        rc->amax = (rc->sign ? max : -min);
        rc->emax = ff_log2(rc->amax);
        rc->e    = ff_log2(rc->amin);
        rc->segment++;

    case 2:
        for (; rc->e < rc->emax; rc->e++) {
            RAC_NZ_MULTISCALE_GET(rc, ctx, NZ_INT_EXP(((rc->e << 1) + rc->sign)),
                       &temp);
            if (temp)
                break;
            temp = 0;
        }
        rc->have = (1 << rc->e);
        rc->left = rc->have - 1;
        rc->pos  = rc->e;
        rc->segment++;

        while (rc->pos > 0) {
            rc->pos--;
            rc->left >>= 1;
            rc->minabs1 = rc->have | (1 << rc->pos);
            rc->maxabs0 = rc->have | rc->left;

            if (rc->minabs1 > rc->amax) {
                continue;
            } else if (rc->maxabs0 >= rc->amin) {
    case 3:
                RAC_NZ_MULTISCALE_GET(rc, ctx, NZ_INT_MANT(rc->pos), &temp);
                if (temp)
                    rc->have = rc->minabs1;
                temp = 0;
            } else
                rc->have = rc->minabs1;
        }
    }

    *target = (rc->sign ? rc->have : -rc->have);
    rc->active = 0;
    return 1;

    need_more_data:
    return 0;
}

int ff_flif16_rac_read_gnz_multiscale_int(FLIF16RangeCoder *rc,
                                          FLIF16MultiscaleChanceContext *ctx,
                                          int min, int max, int *target)
{
    int ret;
    if (min > 0) {
        ret = ff_flif16_rac_read_nz_multiscale_int(rc, ctx, 0, max - min, target);
        if (ret)
            *target += min;
    } else if (max < 0) {
        ret =  ff_flif16_rac_read_nz_multiscale_int(rc, ctx, min - max, 0, target);
        if (ret)
            *target += max;
    } else
        ret = ff_flif16_rac_read_nz_multiscale_int(rc, ctx, min, max, target);
    return ret;

}
#endif

/*
 * Ported from rangecoder.c.
 * FLIF's reference decoder uses a slightly modified version of this function.
 * The copyright of rangecoder.c is in 2004, and therefore this function counts
 * as prior art to the function in the reference decoder (earliest copyright
 * 2010.)
 */
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
        p8 = (size * p + one / 2) >> 32; // FIXME try without the one
        if (p8 <= i)
            p8 = i + 1;
        if (p8 > max_p)
            p8 = max_p;
        one_state[i] = p8;
    }

    for (i = 1; i < size; i++)
        zero_state[i] = size - one_state[size - i];
}

static inline uint32_t log4k_compute(int32_t x, uint32_t base)
{
    int bits     = 8 * sizeof(int32_t) - ff_clz(x);
    uint64_t y   = ((uint64_t) x) << (32 - bits);
    uint32_t res = base * (13 - bits);
    uint32_t add = base;
    for (; (add > 1) && ((y & 0x7FFFFFFF) != 0);
           y = (((uint64_t) y) * y + 0x40000000) >> 31,
           add >>= 1)
        if ((y >> 32)) {
            res -= add;
            y >>= 1;
        }
    return res;
}

void ff_flif16_build_log4k_table(FLIF16Log4kTable *log4k)
{
    log4k->table[0] = 0;
    for (int i = 1; i < 4096; i++)
        log4k->table[i] = (log4k_compute(i, (65535UL << 16) / 12) +
                          (1 << 15)) >> 16;
    log4k->scale = 65535 / 12;
}

void ff_flif16_chancetable_init(FLIF16ChanceTable *ct, int alpha, int cut)
{
    build_table(ct->zero_state, ct->one_state, 4096, alpha, 4096 - cut);
}

void ff_flif16_chancecontext_init(FLIF16ChanceContext *ctx)
{
    memcpy(&ctx->data, &flif16_nz_int_chances, sizeof(flif16_nz_int_chances));
}

#ifdef MULTISCALE_CHANCES_ENABLED
FLIF16MultiscaleChanceTable *ff_flif16_multiscale_chancetable_init(void)
{
    unsigned int len = MULTISCALE_CHANCETABLE_DEFAULT_SIZE;
    FLIF16MultiscaleChanceTable *ct = av_malloc(sizeof(*ct));
    if (!ct)
        return NULL;
    for (int i = 0; i < len; i++) {
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
    for (int i = 0; i < FF_ARRAY_ELEMS(flif16_nz_int_chances); i++)
        ff_flif16_multiscale_chance_set(&ctx->data[i], flif16_nz_int_chances[i]);
}

#endif

int ff_flif16_read_maniac_tree(FLIF16RangeCoder *rc, FLIF16MANIACContext *m,
                               FLIF16MinMax *prop_ranges,
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
            m->forest[channel]->data  = av_mallocz_array(MANIAC_TREE_BASE_SIZE,
                                                         sizeof(*(m->forest[channel]->data)));
            if (!m->forest[channel]->data)
                return AVERROR(ENOMEM);
            m->stack = av_malloc_array(MANIAC_TREE_BASE_SIZE, sizeof(*(m->stack)));
            if (!(m->stack))
                return AVERROR(ENOMEM);

            for (int i = 0; i < 3; i++) {
#ifdef MULTISCALE_CHANCES_ENABLED
                ff_flif16_multiscale_chancecontext_init(&m->ctx[i]);
#else
                ff_flif16_chancecontext_init(&m->ctx[i]);
#endif
            }
            m->stack_top = m->tree_top = 0;

            m->forest[channel]->size       = MANIAC_TREE_BASE_SIZE;
            m->stack_size                  = MANIAC_TREE_BASE_SIZE;

            m->stack[m->stack_top].id      = m->tree_top;
            m->stack[m->stack_top].mode    = 0;
            m->stack[m->stack_top].visited = 0;
            m->stack[m->stack_top].p       = 0;

            m->stack_top++;
            m->tree_top++;
        }
        rc->segment2++;

    case 1:
        while (m->stack_top) {
            oldp = m->stack[m->stack_top - 1].p;
            if (!m->stack[m->stack_top - 1].visited) {
                switch (m->stack[m->stack_top - 1].mode) {
                case 1:
                    prop_ranges[oldp].min = m->stack[m->stack_top - 1].min;
                    prop_ranges[oldp].max = m->stack[m->stack_top - 1].max;
                    break;

                case 2:
                    prop_ranges[oldp].min = m->stack[m->stack_top - 1].min;
                    break;
                }
            } else {
                prop_ranges[oldp].max = m->stack[m->stack_top - 1].max2;
                m->stack_top--;
                rc->segment2 = 1;
                continue;
            }
            m->stack[m->stack_top - 1].visited = 1;
            rc->segment2++;

    case 2:
            RAC_GET(rc, &m->ctx[0], 0, prop_ranges_size,
                    &m->forest[channel]->data[m->stack[m->stack_top - 1].id].property,
                    FLIF16_RAC_MANIAC_GNZ_INT);
            p = --(m->forest[channel]->data[m->stack[m->stack_top - 1].id].property);
            if (p == -1) {
                m->stack_top--;
                rc->segment2 = 1;
                continue;
            }

            m->forest[channel]->data[m->stack[m->stack_top - 1].id].child_id = m->tree_top;
            rc->oldmin = prop_ranges[p].min;
            rc->oldmax = prop_ranges[p].max;
            if (rc->oldmin >= rc->oldmax)
                return AVERROR_INVALIDDATA;
            rc->segment2++;

    case 3:
            RAC_GET(rc, &m->ctx[1], MANIAC_TREE_MIN_COUNT, MANIAC_TREE_MAX_COUNT,
                    &m->forest[channel]->data[m->stack[m->stack_top - 1].id].count,
                    FLIF16_RAC_MANIAC_GNZ_INT);
            rc->segment2++;

    case 4:
            RAC_GET(rc, &m->ctx[2], rc->oldmin, rc->oldmax - 1,
                    &m->forest[channel]->data[m->stack[m->stack_top - 1].id].split_val,
                    FLIF16_RAC_MANIAC_GNZ_INT);
            split_val = m->forest[channel]->data[m->stack[m->stack_top - 1].id].split_val;
            rc->segment2++;

    case 5:
            if ((m->tree_top + 2) >= m->forest[channel]->size) {
                m->forest[channel]->data = av_realloc_f(m->forest[channel]->data,
                                                        m->forest[channel]->size * 2,
                                                        sizeof(*(m->forest[channel]->data)));
                if(!(m->forest[channel]->data))
                    return AVERROR(ENOMEM);
                m->forest[channel]->size *= 2;
            }

            if ((m->stack_top + 2) >= m->stack_size) {
                m->stack = av_realloc_f(m->stack, m->stack_size * 2, sizeof(*(m->stack)));
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
            m->stack_top++;

            // Left Child
            m->stack[m->stack_top].id      = m->tree_top;
            m->stack[m->stack_top].p       = temp;
            m->stack[m->stack_top].min     = split_val + 1;
            m->stack[m->stack_top].mode    = 2;
            m->stack[m->stack_top].visited = 0;
            m->stack_top++;

            m->tree_top += 2;
            rc->segment2 = 1;
        }
    }

    m->forest[channel]->data = av_realloc_f(m->forest[channel]->data,
                                            m->tree_top,
                                            sizeof(*m->forest[channel]->data));
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

void ff_flif16_maniac_close(FLIF16MANIACContext *m, uint8_t num_planes,
                            uint8_t lookback)
{
    for (int i = 0; i < (lookback ? MAX_PLANES : num_planes); i++) {
        if (!m->forest[i])
            continue;
        av_freep(&m->forest[i]->data);
        av_freep(&m->forest[i]->leaves);
        av_freep(&m->forest[i]);
    }

    av_freep(&m->forest);

    // Should be already freed in MANIAC reading, but checking anyway.
    av_freep(&m->stack);
}


static FLIF16MANIACChanceContext *ff_flif16_maniac_findleaf(FLIF16MANIACContext *m,
                                                            uint8_t channel,
                                                            int32_t *properties)
{
    unsigned int pos = 0;
    uint32_t old_leaf;
    uint32_t new_leaf;
    FLIF16MANIACTree *tree = m->forest[channel];
    FLIF16MANIACNode *nodes = tree->data;

    if (!m->forest[channel]->leaves) {
        m->forest[channel]->leaves = av_mallocz_array(MANIAC_TREE_BASE_SIZE,
                                                      sizeof(*m->forest[channel]->leaves));
        m->forest[channel]->leaves_size = MANIAC_TREE_BASE_SIZE;
        if(!m->forest[channel]->leaves)
            return NULL;
#ifdef MULTISCALE_CHANCES_ENABLED
        ff_flif16_multiscale_chancecontext_init(&m->forest[channel]->leaves[0]);
#else
        ff_flif16_chancecontext_init(&m->forest[channel]->leaves[0]);
#endif
        tree->leaves_top = 1;
    }

    while (nodes[pos].property != -1) {
        if (nodes[pos].count < 0) {
            if (properties[nodes[pos].property] > nodes[pos].split_val)
                pos = nodes[pos].child_id;
            else
                pos = nodes[pos].child_id + 1;
        } else if (nodes[pos].count > 0) {
            nodes[pos].count--;
            break;
        } else {
            nodes[pos].count--;
            if ((tree->leaves_top) >= tree->leaves_size) {
                m->forest[channel]->leaves = av_realloc_f(m->forest[channel]->leaves,
                                                          m->forest[channel]->leaves_size * 2,
                                                          sizeof(*m->forest[channel]->leaves));
                if (!m->forest[channel]->leaves)
                    return NULL;
                m->forest[channel]->leaves_size *= 2;
            }
            old_leaf = nodes[pos].leaf_id;
            new_leaf = tree->leaves_top;
            memcpy(&m->forest[channel]->leaves[tree->leaves_top],
                   &m->forest[channel]->leaves[nodes[pos].leaf_id],
                   sizeof(*m->forest[channel]->leaves));
            tree->leaves_top++;
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

int ff_flif16_maniac_read_int(FLIF16RangeCoder *rc, FLIF16MANIACContext *m,
                              int32_t *properties, uint8_t channel,
                              int min, int max, int *target)
{
    if (!rc->curr_leaf)
        rc->segment2 = 0;

    switch(rc->segment2) {
    case 0:
        if (min == max) {
            *target = min;
            goto end;
        }
        rc->curr_leaf = ff_flif16_maniac_findleaf(m, channel, properties);
        if(!rc->curr_leaf) {
            return AVERROR(ENOMEM);
        }
        rc->segment2++;

    case 1:
        RAC_GET(rc, rc->curr_leaf, min, max, target, FLIF16_RAC_MANIAC_NZ_INT);
    }

    end:
    rc->curr_leaf = NULL;
    rc->segment2  = 0;
    return 1;

    need_more_data:
    return 0;
}
