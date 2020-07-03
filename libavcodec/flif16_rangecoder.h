/*
 * Range coder for FLIF16
 * Copyright (c) 2020 Anamitra Ghorui <aghorui@teknik.io>
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
 * Range coder for FLIF16.
 */

#ifndef FLIF16_RANGECODER_H
#define FLIF16_RANGECODER_H

#include "libavutil/mem.h"
#include "libavutil/intmath.h"
#include "bytestream.h"
#include "rangecoder.h"

#include <stdio.h> // Remove
#include <stdint.h>
#include <assert.h> // Remove

//#define __PLN__ printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
//#define MSG(fmt, ...) printf("[%s] " fmt, __func__, ##__VA_ARGS__)

//#define __PLN__
//#define MSG(fmt,...) while(0)

#define FLIF16_RAC_MAX_RANGE_BITS 24
#define FLIF16_RAC_MAX_RANGE_BYTES (FLIF16_RAC_MAX_RANGE_BITS / 8)
#define FLIF16_RAC_MIN_RANGE_BITS 16
#define FLIF16_RAC_MAX_RANGE (uint32_t) 1 << FLIF16_RAC_MAX_RANGE_BITS
#define FLIF16_RAC_MIN_RANGE (uint32_t) 1 << FLIF16_RAC_MIN_RANGE_BITS

#define CHANCETABLE_DEFAULT_ALPHA (0xFFFFFFFF / 19)
#define CHANCETABLE_DEFAULT_CUT 2

// #define MULTISCALE_CHANCES_ENABLED

#define MULTISCALE_CHANCETABLE_DEFAULT_SIZE 6
#define MULTISCALE_CHANCETABLE_DEFAULT_CUT  8

#define MANIAC_TREE_BASE_SIZE 160
#define MANIAC_TREE_MIN_COUNT 1
#define MANIAC_TREE_MAX_COUNT 512

typedef enum FLIF16RACTypes {
    FLIF16_RAC_BIT = 0,
    FLIF16_RAC_UNI_INT,
    FLIF16_RAC_CHANCE,
    FLIF16_RAC_NZ_INT,
    FLIF16_RAC_GNZ_INT,
#ifdef MULTISCALE_CHANCES_ENABLED
    FLIF16_RAC_NZ_MULTISCALE_INT,
    FLIF16_RAC_GNZ_MULTISCALE_INT
#endif
} FLIF16RACReaders;

typedef struct FLIF16ChanceTable {
    uint16_t zero_state[4096];
    uint16_t one_state[4096];
} FLIF16ChanceTable;

typedef struct FLIF16MultiscaleChanceTable {
    FLIF16ChanceTable sub_table[MULTISCALE_CHANCETABLE_DEFAULT_SIZE];
} FLIF16MultiscaleChanceTable;


typedef struct FLIF16Log4kTable {
    uint16_t table[4097];
    int scale;
} FLIF16Log4kTable;

static const uint32_t flif16_multiscale_alphas[] = {
    21590903, 66728412, 214748365, 7413105, 106514140, 10478104
};

typedef struct FLIF16MultiscaleChance {
    uint16_t chances[MULTISCALE_CHANCETABLE_DEFAULT_SIZE];
    uint32_t quality[MULTISCALE_CHANCETABLE_DEFAULT_SIZE];
    uint8_t best;
} FLIF16MultiscaleChance;

// Apparently this will not overflow over to any other section.
/*
static uint16_t flif16_nz_int_chances[] = {
    1000, // Zero
    2048, // Sign

    // Exponents
    1000, 1200, 1500, 1750, 2000, 2300, 2800, 2400, 2300, 2048,
    2048, 2048, 2048, 2048, 2048, 2048, 2048,

    // Mantisaa
    1900, 1850, 1800, 1750, 1650, 1600, 1600, 2048,
    2048, 2048, 2048, 2048, 2048, 2048, 2048, 2048
};*/

static uint16_t flif16_nz_int_chances[] = {
    1000,        // ZERO 
    2048,        // SIGN (0)  (1)
    1000, 1000,  // EXP:  0,   1
    1200, 1200,  // EXP:  2,   3
    1500, 1500,  // EXP:  4,   5
    1750, 1750,  // EXP:  6,   7
    2000, 2000,  // EXP:  8,   9
    2300, 2300,  // EXP:  10,  11
    2800, 2800,  // EXP:  12,  13
    2400, 2400,  // EXP:  14,  15
    2300, 2300,  // EXP:  16,  17
    2048, 2048,  // EXP:  18,  19
    2048, 2048,  // EXP:  20,  21
    2048, 2048,  // EXP:  22,  23
    2048, 2048,  // EXP:  24,  25
    2048, 2048,  // EXP:  26,  27
    2048, 2048,  // EXP:  28,  29
    2048, 2048,  // EXP:  30,  31
    2048, 2048,  // EXP:  32,  33
    1900,        // MANT: 0:
    1850,        // MANT: 1:
    1800,        // MANT: 2:
    1750,        // MANT: 3:
    1650,        // MANT: 4:
    1600,        // MANT: 5:
    1600,        // MANT: 6:
    2048,        // MANT: 7:
    2048,        // MANT: 8:
    2048,        // MANT: 9:
    2048,        // MANT: 10:
    2048,        // MANT: 11:
    2048,        // MANT: 12:
    2048,        // MANT: 13:
    2048,        // MANT: 14:
    2048,        // MANT: 15:
    2048,        // MANT: 16:
    2048         // MANT: 17:
};

#define NZ_INT_ZERO (0)
#define NZ_INT_SIGN (1)
#define NZ_INT_EXP(k) ((2 + (k)))
#define NZ_INT_MANT(k) ((36 + (k)))


typedef struct FLIF16MultiscaleChanceContext {
    FLIF16MultiscaleChance data[sizeof(flif16_nz_int_chances)/sizeof(flif16_nz_int_chances[0])];
} FLIF16MultiscaleChanceContext;

// Maybe rename to symbol context
typedef struct FLIF16ChanceContext {
    uint16_t data[sizeof(flif16_nz_int_chances)/sizeof(flif16_nz_int_chances[0])];
} FLIF16ChanceContext;

// rc->renorm may be useless. Check.

typedef struct FLIF16RangeCoder {
    uint_fast32_t range;
    uint_fast32_t low;
    uint16_t chance;
    uint8_t empty;    ///< Is bytestream empty
    uint8_t renorm;   ///< Is a renormalisation required
    uint8_t active;   ///< Is an integer reader currently active (to save/
                      ///  transfer state)

    // uni_int state management
    uint32_t min;
    uint32_t len;

    // nz_int state management
    uint8_t segment; ///< The "segment" the function currently is in
    uint8_t sign;
    int amin, amax, emax, e, have, left, minabs1, maxabs0, pos;

    // maniac_int state management
    uint8_t segment2;
    int oldmin, oldmax;

    #ifdef MULTISCALE_CHANCES_ENABLED
    FLIF16MultiscaleChanceContext *maniac_ctx;
    #else
    FLIF16ChanceContext *maniac_ctx;
    #endif

    FLIF16ChanceTable ct;
#ifdef MULTISCALE_CHANCES_ENABLED
    FLIF16MultiscaleChanceTable *mct;
    FLIF16Log4kTable log4k;
#endif
    GetByteContext *gb;
} FLIF16RangeCoder;

/**
 * The Stack used to construct the MANIAC tree
 */
typedef struct FLIF16MANIACStack {
    unsigned int id;
    int p;
    int min;
    int max;
    int max2;
    uint8_t mode;
    uint8_t visited;
} FLIF16MANIACStack;

typedef struct FLIF16MANIACNode {
    int8_t property;
    int16_t count;
    // typedef int32_t ColorVal;
    int32_t split_val;
    uint32_t child_id;
    uint32_t leaf_id;
    // probably safe to use only uint16
    //uint16_t childID;
    //uint16_t leafID;
} FLIF16MANIACNode;

typedef struct FLIF16MANIACTree {
    FLIF16MANIACNode *data;
#ifdef MULTISCALE_CHANCES_ENABLED
    FLIF16MultiscaleChanceContext *leaves;
#else
    FLIF16ChanceContext *leaves;
#endif
    unsigned int size;
    unsigned int leaves_size;
    unsigned int leaves_top;
} FLIF16MANIACTree;

typedef struct FLIF16MANIACContext {
    FLIF16MANIACTree **forest;
    FLIF16MANIACStack *stack;
#ifdef MULTISCALE_CHANCES_ENABLED
    FLIF16MultiscaleChanceContext ctx[3];
#else
    FLIF16ChanceContext ctx[3];
#endif
    unsigned int tree_top;
    unsigned int stack_top;
    unsigned int stack_size;
} FLIF16MANIACContext;

void ff_flif16_rac_init(FLIF16RangeCoder *rc, GetByteContext *gb, uint8_t *buf,
                        uint8_t buf_size);

void ff_flif16_rac_free(FLIF16RangeCoder *rc);

void ff_flif16_chancecontext_init(FLIF16ChanceContext *ctx);

void ff_flif16_chancetable_init(FLIF16ChanceTable *ct, int alpha, int cut);

void ff_flif16_build_log4k_table(FLIF16Log4kTable *log4k);

int ff_flif16_read_maniac_tree(FLIF16RangeCoder *rc,
                               FLIF16MANIACContext *m,
                               int32_t (*prop_ranges)[2],
                               unsigned int prop_ranges_size,
                               unsigned int channel);

#ifdef MULTISCALE_CHANCES_ENABLED

void ff_flif16_multiscale_chancecontext_init(FLIF16MultiscaleChanceContext *ctx);

FLIF16MultiscaleChanceTable *ff_flif16_multiscale_chancetable_init(void);

FLIF16MultiscaleChanceContext *ff_flif16_maniac_findleaf(FLIF16MANIACContext *m,
                                                         uint8_t channel,
                                                         int32_t *properties);
#else
FLIF16ChanceContext *ff_flif16_maniac_findleaf(FLIF16MANIACContext *m,
                                               uint8_t channel,
                                               int32_t *properties);
#endif

int ff_flif16_maniac_read_int(FLIF16RangeCoder *rc,
                              FLIF16MANIACContext *m,
                              int32_t *properties,
                              uint8_t channel,
                              int min, int max, int *target);

#define MANIAC_GET(rc, m, prop, channel, min, max, target) \
    if (!ff_flif16_maniac_read_int((rc), (m), (prop), (channel), (min), (max), (target))) {\
        goto need_more_data; \
    }

// Functions

static inline int ff_flif16_rac_renorm(FLIF16RangeCoder *rc)
{
    uint32_t left;
    while (rc->range <= FLIF16_RAC_MIN_RANGE) {
        left = bytestream2_get_bytes_left(rc->gb);
        if (!left) {
            return 0;
        }
        //printf("Renorm left = %d %lu %u\n", left, rc->range, FLIF16_RAC_MIN_RANGE);
        rc->low <<= 8;
        rc->range <<= 8;
        rc->low |= bytestream2_get_byte(rc->gb);
        //printf("Renorm low = %lu range = %lu\n", rc->low, rc->range);
        if(!left)
            return 0;
        else
            --left;
    }
    rc->renorm = 0;
    return 1;
}

static inline uint8_t ff_flif16_rac_get(FLIF16RangeCoder *rc, uint32_t chance,
                                        uint8_t *target)
{
    if (rc->renorm) {
        printf("Triggered\n");
        return 0;
    }
    // printf("low = %lu range = %lu chance = %u renorm = %d\n", rc->low, rc->range, chance, rc->renorm);
    if (rc->low >= rc->range - chance) {
        rc->low -= rc->range - chance;
        rc->range = chance;
        *target = 1;
    } else {
        rc->range -= chance;
        *target = 0;
    }

    rc->renorm = 1;

    return 1;
}

static inline uint8_t ff_flif16_rac_read_bit(FLIF16RangeCoder *rc,
                                             uint8_t *target)
{
    return ff_flif16_rac_get(rc, rc->range >> 1, target);
}

static inline uint32_t ff_flif16_rac_read_chance(FLIF16RangeCoder *rc,
                                                 uint16_t b12, uint8_t *target)
{
    uint32_t ret;

    if (rc->renorm) {
        printf("Triggered\n");
        return 0;
    }

    if (sizeof(rc->range) > 4)
        ret = ((rc->range) * b12 + 0x800) >> 12;
    else
        ret = (((((rc->range) & 0xFFF) * b12 + 0x800) >> 12) +
              (((rc->range) >> 12) * b12));

    return ff_flif16_rac_get(rc, ret, target);
}

/**
 * Reads a Uniform Symbol Coded Integer.
 */
static inline int ff_flif16_rac_read_uni_int(FLIF16RangeCoder *rc,
                                             uint32_t min, uint32_t len,
                                             uint32_t *target)
{
    int med;
    uint8_t bit;

    if (rc->renorm) {
        printf("Triggered\n");
        return 0;
    }

    if (!rc->active) {
        rc->min = min;
        rc->len = len;
        rc->active = 1;
    }

    if ((rc->len) > 0) {
        ff_flif16_rac_read_bit(rc, &bit);
        med = (rc->len) / 2;
        if (bit) {
            rc->min += med + 1;
            rc->len -= med + 1;
        } else {
            rc->len = med;
        }
        // MSG("min = %d , len = %d\n", rc->min, rc->len);
        return 0;
    } else {
        *target = rc->min;
        // MSG("target = %d\n", rc->min);
        rc->active = 0;
        return 1;
    }
}

// Overload of above function in original code

static inline int ff_flif16_rac_read_uni_int_bits(FLIF16RangeCoder *rc,
                                                  int bits, uint32_t *target)
{
    return ff_flif16_rac_read_uni_int(rc, 0, (1 << bits) - 1, target);
}

// Nearzero integer definitions

static inline void ff_flif16_chancetable_put(FLIF16RangeCoder *rc,
                                             FLIF16ChanceContext *ctx,
                                             uint16_t type, uint8_t bit)
{
    //printf("put: type = %d chance = %d\n", type, ctx->data[type]);
    if(ctx->data[type] >= 4096)
        printf("type: %u data: %u\n", type, ctx->data[type]);
    ctx->data[type] = (!bit) ? rc->ct.zero_state[ctx->data[type]]
                             : rc->ct.one_state[ctx->data[type]];
}

#ifdef MULTISCALE_CHANCES_ENABLED
static inline void ff_flif16_chance_estim(FLIF16RangeCoder *rc,
                                          uint16_t chance, uint8_t bit,
                                          uint64_t *total)
{
    *total += rc->log4k.table[bit ? chance : 4096 - chance];
}
#endif

/**
 * Reads a near-zero encoded symbol into the RAC probability model/chance table
 * @param type The symbol chance specified by the NZ_INT_* macros
 */
// TODO remove return value
static inline uint8_t ff_flif16_rac_read_symbol(FLIF16RangeCoder *rc,
                                                FLIF16ChanceContext *ctx,
                                                uint16_t type,
                                                uint8_t *target)
{
    ff_flif16_rac_read_chance(rc, ctx->data[type], target);
    ff_flif16_chancetable_put(rc, ctx, type, *target);
    //printf("%s ", __func__);
    //for(int i = 0; i < sizeof(flif16_nz_int_chances)/sizeof(flif16_nz_int_chances[0]); ++i)
    //    printf("%u ", ctx->data[i]);
    //printf("\n");
    if(ctx->data[type] >= 4096)
        printf("[ !!! ] Out of bounds type: %u chance: %u\n", type, ctx->data[type]);
    return 1;
}

// NearZero Integer Coder

static inline int ff_flif16_rac_nz_read_internal(FLIF16RangeCoder *rc,
                                                 FLIF16ChanceContext *ctx,
                                                 uint16_t type, uint8_t *target)
{
    int flag = 0;
    // __PLN__
    // Maybe remove the while loop
    while (!flag) {
        if (rc->renorm) {
            if(!ff_flif16_rac_renorm(rc))
                return 0; // EAGAIN condition
        }
        flag = ff_flif16_rac_read_symbol(rc, ctx, type, target);
    }
    return 1;
}

#define RAC_NZ_GET(rc, ctx, chance, target)                                    \
    if (!ff_flif16_rac_nz_read_internal((rc), (ctx), (chance),                 \
                                        (uint8_t *) (target))) {               \
        goto need_more_data;                                                   \
    }

static inline int ff_flif16_rac_read_nz_int(FLIF16RangeCoder *rc,
                                            FLIF16ChanceContext *ctx,
                                            int min, int max, int *target)
{
    uint8_t temp = 0;
    //printf("segment = %d min: %d max: %d\n", rc->segment, min, max);
    if (min == max) {
        // printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
        //printf("min == max\n");
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
            //printf("segment = %d\n", rc->segment);
            RAC_NZ_GET(rc, ctx, NZ_INT_ZERO, &(temp));
            if (temp) {
                // printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                //printf("bit zero\n");
                *target = 0;
                rc->active = 0;
                return 1;
            }
            ++rc->segment;

        case 1:
            //printf("segment = %d\n", rc->segment);
            if (min < 0) {
                if (max > 0) {
                    // printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                    RAC_NZ_GET(rc, ctx, NZ_INT_SIGN, &(rc->sign));
                } else {
                    rc->sign = 0;
                }
            } else {
                rc->sign = 1;
            }
            rc->amax = (rc->sign ? max : -min);
            rc->emax = ff_log2(rc->amax);
            rc->e    = ff_log2(rc->amin);
            ++rc->segment;

        case 2:
            //printf("segment = %d\n", rc->segment);
            for (; (rc->e) < (rc->emax); (rc->e++)) {
                // printf("At: [%s] %s, %d\n", __func__, __FILE__, __LINE__);
                RAC_NZ_GET(rc, ctx, NZ_INT_EXP((((rc->e) << 1) + rc->sign)),
                           &(temp));
                if (temp)
                    break;
                temp = 0;
            }
            rc->have = (1 << (rc->e));
            rc->left = rc->have - 1;
            rc->pos  = rc->e;
            ++rc->segment;

         /*
          case 3 and case 4 mimic a for loop.
          This is done to separate the RAC read statement.
          for(pos = e; pos > 0; --pos) ...
          TODO replace entirely with an actual for loop.
         */
        case 3:
            //printf("segment = %d\n", rc->segment);
            loop: /* start for */
            if ((rc->pos) <= 0)
                goto end;
            --(rc->pos);
            rc->left >>= 1;
            rc->minabs1 = (rc->have) | (1 << (rc->pos));
            rc->maxabs0 = (rc->have) | (rc->left);
            ++rc->segment;

        case 4:
            if ((rc->minabs1) > (rc->amax)) {
                --rc->segment;
                goto loop; /* continue; */
            } else if ((rc->maxabs0) >= (rc->amin)) {
                RAC_NZ_GET(rc, ctx, NZ_INT_MANT(rc->pos), &temp);
                if (temp)
                    rc->have = rc->minabs1;
                temp = 0;
            } else
                rc->have = rc->minabs1;
            --rc->segment;
            goto loop; /* end for */
    }

    end:
    //printf("%d %d %u %d %d %d %d %d %d %d %d %d \n", min, max, rc->sign, rc->amin, rc->amax, rc->emax, rc->e, rc->minabs1, rc->maxabs0, rc->left, rc->have, rc->pos);
    *target = ((rc->sign) ? (rc->have) : -(rc->have));
    rc->active = 0;
    return 1;

    need_more_data:
    return 0;
}

static inline int ff_flif16_rac_read_gnz_int(FLIF16RangeCoder *rc,
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

static inline uint16_t ff_flif16_multiscale_chance_get(FLIF16MultiscaleChance c)
{
    return c.chances[c.best];
}

static inline void ff_flif16_multiscale_chancetable_put(FLIF16RangeCoder *rc,
                                                        FLIF16MultiscaleChanceContext *ctx,
                                                        uint16_t type, uint8_t bit)
{
    FLIF16MultiscaleChance *c = &ctx->data[type];
    uint64_t sbits, oqual;
    //printf("multiscale_put: type = %d bit = %d || ", type, bit);
    for (int i = 0; i < MULTISCALE_CHANCETABLE_DEFAULT_SIZE; ++i) {
        sbits = 0;
        ff_flif16_chance_estim(rc, c->chances[i], bit, &sbits);
        oqual = c->quality[i];
        c->quality[i] = (oqual * 255 + sbits * 4097 + 128) >> 8;
        c->chances[i] = (bit) ? rc->mct->sub_table[i].one_state[c->chances[i]]
                              : rc->mct->sub_table[i].zero_state[c->chances[i]];
    }
    for (int i = 0; i < MULTISCALE_CHANCETABLE_DEFAULT_SIZE; ++i)
        if (c->quality[i] < c->quality[c->best])
            c->best = i;
}

static inline int ff_flif16_rac_read_multiscale_symbol(FLIF16RangeCoder *rc,
                                                       FLIF16MultiscaleChanceContext *ctx,
                                                       uint16_t type, uint8_t *target)
{
    ff_flif16_rac_read_chance(rc, ff_flif16_multiscale_chance_get(ctx->data[type]), target);
    ff_flif16_multiscale_chancetable_put(rc, ctx, type, *target);
    return 1;
}

static inline int ff_flif16_rac_nz_read_multiscale_internal(FLIF16RangeCoder *rc,
                                                            FLIF16MultiscaleChanceContext *ctx,
                                                            uint16_t type, uint8_t *target)
{
    int flag = 0;
    // Maybe remove the while loop
    while (!flag) {
        if (rc->renorm) {
            if(!ff_flif16_rac_renorm(rc))
                return 0; // EAGAIN condition
        }
        flag = ff_flif16_rac_read_multiscale_symbol(rc, ctx, type, target);
    }
    return 1;
}

#define RAC_NZ_MULTISCALE_GET(rc, ctx, chance, target)                         \
    if (!ff_flif16_rac_nz_read_multiscale_internal((rc), (ctx), (chance),      \
                                                   (uint8_t *) (target))) {    \
        goto need_more_data;                                                   \
    }

static inline int ff_flif16_rac_read_nz_multiscale_int(FLIF16RangeCoder *rc,
                                                       FLIF16MultiscaleChanceContext *ctx,
                                                      int min, int max, int *target)
{
    int temp = 0;

    if (min == max) {
        *target = min;
        goto end;
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
            RAC_NZ_MULTISCALE_GET(rc, ctx, NZ_INT_ZERO, &(temp));
            if (temp) {
                *target = 0;
                goto end;
            }
            ++rc->segment;__PLN__

        case 1:
            if (min < 0) {
                if (max > 0) {
                    RAC_NZ_MULTISCALE_GET(rc, ctx, NZ_INT_SIGN, &(rc->sign));
                } else {
                    rc->sign = 0;
                }
            } else {
                rc->sign = 1;
            }
            rc->amax = (rc->sign ? max : -min);
            rc->emax = ff_log2(rc->amax);
            rc->e    = ff_log2(rc->amin);
            ++rc->segment;__PLN__

        case 2:
            for (; (rc->e) < (rc->emax); (rc->e++)) {
                RAC_NZ_MULTISCALE_GET(rc, ctx, NZ_INT_EXP((((rc->e) << 1) + rc->sign)),
                                      &(temp));
                if (temp)
                    break;
                temp = 0;
            }
            rc->have = (1 << (rc->e));
            rc->left = rc->have - 1;
            rc->pos  = rc->e;
            ++rc->segment;__PLN__

         /*
          case 3 and case 4 mimic a for loop.
          This is done to separate the RAC read statement.
          for(pos = e; pos > 0; --pos) ...
         */
        case 3:
            loop: /* start for */
            if ((rc->pos) <= 0)
                goto end;
            --(rc->pos);
            rc->left >>= 1;
            rc->minabs1 = (rc->have) | (1 << (rc->pos));
            rc->maxabs0 = (rc->have) | (rc->left);
            ++rc->segment;__PLN__

        case 4:
            if ((rc->minabs1) > (rc->amax)) {
                --rc->segment;
                goto loop; /* continue; */
            } else if ((rc->maxabs0) >= (rc->amin)) {
                RAC_NZ_MULTISCALE_GET(rc, ctx, NZ_INT_MANT(rc->pos), &temp);
                if (temp)
                    rc->have = rc->minabs1;
                temp = 0;
            }
            else
                rc->have = rc->minabs1;
            --rc->segment;
            goto loop; /* end for */
    }

    end:
    *target = ((rc->sign) ? (rc->have) : -(rc->have));
    rc->active = 0;
    return 1;

    need_more_data:
    return 0;
}

static inline int ff_flif16_rac_read_gnz_multiscale_int(FLIF16RangeCoder *rc,
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
RAC_NZ_DEFINE(, FLIF16ChanceContext)
RAC_GNZ_DEFINE(, FLIF16ChanceContext)

#ifdef MULTISCALE_CHANCES_ENABLED

#undef RAC_NZ_GET

#define RAC_NZ_GET(rc, ctx, chance, target)                                    \
    if (!ff_flif16_rac_nz_read_multiscale_internal((rc), (ctx), (chance),      \
                                            (uint8_t *) (target))) {           \
        goto need_more_data;                                                   \
    }

RAC_NZ_DEFINE(_multiscale, FLIF16MultiscaleChanceContext)
RAC_GNZ_DEFINE(_multiscale, FLIF16MultiscaleChanceContext)

#endif
*/
/**
 * Reads an integer encoded by FLIF's RAC.
 * @param[in]  val1 A generic value, chosen according to the required type
 * @param[in]  val2 Same as val1
 * @param[out] target The place where the resultant value should be written to
 * @param[in]  type The type of the integer to be decoded specified by
 *             FLIF16RACTypes
 * @return 0 on bytestream empty, 1 on successful decoding.
 */
static inline int ff_flif16_rac_process(FLIF16RangeCoder *rc,
                                        void *ctx,
                                        int val1, int val2, void *target,
                                        int type)
{
    int flag = 0;
    while (!flag) {
        if(rc->renorm) {
            if(!ff_flif16_rac_renorm(rc))
                return 0; // EAGAIN condition
        }

        switch (type) {
            case FLIF16_RAC_BIT:
                flag = ff_flif16_rac_read_bit(rc, (uint8_t *) target);
                break;

            case FLIF16_RAC_UNI_INT:
                flag = ff_flif16_rac_read_uni_int(rc, val1, val2,
                                                  (uint32_t *) target);
                break;

            case FLIF16_RAC_CHANCE:
                flag = ff_flif16_rac_read_chance(rc, val1, (uint8_t *) target);
                break;

            case FLIF16_RAC_NZ_INT:
                // handle nz_ints
                flag = ff_flif16_rac_read_nz_int(rc, (FLIF16ChanceContext *) ctx,
                                                 val1, val2, (int *) target);
                break;

            case FLIF16_RAC_GNZ_INT:
                // handle gnz_ints
                flag = ff_flif16_rac_read_gnz_int(rc, (FLIF16ChanceContext *) ctx,
                                                  val1, val2, (int *) target);
                break;
#ifdef MULTISCALE_CHANCES_ENABLED
            case FLIF16_RAC_NZ_MULTISCALE_INT:
                // handle nz_ints
                flag = ff_flif16_rac_read_nz_multiscale_int(rc, (FLIF16MultiscaleChanceContext *) ctx,
                                                            val1, val2, (int *) target);
                break;

            case FLIF16_RAC_GNZ_MULTISCALE_INT:
                // handle nz_ints
                flag = ff_flif16_rac_read_gnz_multiscale_int(rc, (FLIF16MultiscaleChanceContext *) ctx,
                                                             val1, val2, (int *) target);
                break;
#endif
            default:
                // MSG("unknown rac reader\n");
                break;
        }
    }

    return 1;
}

#define RAC_GET(rc, ctx, val1, val2, target, type) \
    if (!ff_flif16_rac_process((rc), (ctx), (val1), (val2), (target), (type))) {\
        goto need_more_data; \
    }

#endif /* FLIF16_RANGECODER_H */
