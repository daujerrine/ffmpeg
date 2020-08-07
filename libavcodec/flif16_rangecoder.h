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

#include <stdint.h>


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

#define MANIAC_TREE_BASE_SIZE 1600
#define MANIAC_TREE_MIN_COUNT 1
#define MANIAC_TREE_MAX_COUNT 512

typedef enum FLIF16RACReader {
    FLIF16_RAC_BIT = 0,
    FLIF16_RAC_UNI_INT8,
    FLIF16_RAC_UNI_INT16,
    FLIF16_RAC_UNI_INT32,
    FLIF16_RAC_CHANCE,
    FLIF16_RAC_NZ_INT,
    FLIF16_RAC_GNZ_INT,
#ifdef MULTISCALE_CHANCES_ENABLED
    FLIF16_RAC_NZ_MULTISCALE_INT,
    FLIF16_RAC_GNZ_MULTISCALE_INT
#endif
} FLIF16RACReader;

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
    1900,        // MANT: 0
    1850,        // MANT: 1
    1800,        // MANT: 2
    1750,        // MANT: 3
    1650,        // MANT: 4
    1600,        // MANT: 5
    1600,        // MANT: 6
    2048,        // MANT: 7
    2048,        // MANT: 8
    2048,        // MANT: 9
    2048,        // MANT: 10
    2048,        // MANT: 11
    2048,        // MANT: 12
    2048,        // MANT: 13
    2048,        // MANT: 14
    2048,        // MANT: 15
    2048,        // MANT: 16
    2048         // MANT: 17
};

#define NZ_INT_ZERO (0)
#define NZ_INT_SIGN (1)
#define NZ_INT_EXP(k) ((2 + (k)))
#define NZ_INT_MANT(k) ((36 + (k)))


typedef struct FLIF16MultiscaleChanceContext {
    FLIF16MultiscaleChance data[FF_ARRAY_ELEMS(flif16_nz_int_chances) ];
} FLIF16MultiscaleChanceContext;

// Maybe rename to symbol context
typedef struct FLIF16ChanceContext {
    uint16_t data[FF_ARRAY_ELEMS(flif16_nz_int_chances)];
} FLIF16ChanceContext;

typedef struct FLIF16MinMax {
    int32_t min;
    int32_t max;
} FLIF16MinMax;

typedef struct FLIF16RangeCoder {
    uint_fast32_t range;
    uint_fast32_t low;
    uint16_t chance;
    uint8_t active;   ///< Is an integer reader currently active (to save/
                      ///  transfer state)

    // uni_int state management
    int32_t min;
    int32_t len;

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
    int32_t property;
    int32_t count;
    int32_t split_val;
    int32_t child_id;
    int32_t leaf_id;
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
                               FLIF16MinMax *prop_ranges,
                               unsigned int prop_ranges_size,
                               unsigned int channel);

void ff_flif16_maniac_close(FLIF16MANIACContext *m, uint8_t num_planes);

#ifdef MULTISCALE_CHANCES_ENABLED

void ff_flif16_multiscale_chancecontext_init(FLIF16MultiscaleChanceContext *ctx);

FLIF16MultiscaleChanceTable *ff_flif16_multiscale_chancetable_init(void);

#endif

int ff_flif16_maniac_read_int(FLIF16RangeCoder *rc,
                              FLIF16MANIACContext *m,
                              int32_t *properties, uint8_t channel,
                              int min, int max, int *target);

#define MANIAC_GET(rc, m, prop, channel, min, max, target) \
    if (!ff_flif16_maniac_read_int((rc), (m), (prop), (channel), (min), (max), (target))) {\
        goto need_more_data; \
    }

static inline int ff_flif16_rac_renorm(FLIF16RangeCoder *rc)
{
    uint32_t left;
    while (rc->range <= FLIF16_RAC_MIN_RANGE) {
        left = bytestream2_get_bytes_left(rc->gb);
        if (!left) {
            return 0;
        }
        rc->low <<= 8;
        rc->range <<= 8;
        rc->low |= bytestream2_get_byte(rc->gb);
        if(!left) {
            return 0;
        } else {
            left--;
        }
    }
    return 1;
}

uint8_t ff_flif16_rac_read_bit(FLIF16RangeCoder *rc,
                               uint8_t *target);

uint32_t ff_flif16_rac_read_chance(FLIF16RangeCoder *rc,
                                   uint16_t b12, uint8_t *target);

int ff_flif16_rac_read_uni_int(FLIF16RangeCoder *rc,
                               int32_t min, int32_t len,
                               int type, void *target);

int ff_flif16_rac_read_nz_int(FLIF16RangeCoder *rc,
                              FLIF16ChanceContext *ctx,
                              int min, int max, int *target);

int ff_flif16_rac_read_gnz_int(FLIF16RangeCoder *rc,
                               FLIF16ChanceContext *ctx,
                               int min, int max, int *target);

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
        if (!ff_flif16_rac_renorm(rc)) {
            return 0; // EAGAIN condition
        }

        switch (type) {
        case FLIF16_RAC_BIT:
            flag = ff_flif16_rac_read_bit(rc, (uint8_t *) target);
            break;

        case FLIF16_RAC_UNI_INT8:
        case FLIF16_RAC_UNI_INT16:
        case FLIF16_RAC_UNI_INT32:
            flag = ff_flif16_rac_read_uni_int(rc, val1, val2, type, target);
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
            // handle multiscale nz_ints
            flag = ff_flif16_rac_read_gnz_multiscale_int(rc, (FLIF16MultiscaleChanceContext *) ctx,
                                                         val1, val2, (int *) target);
            break;
#endif
        default:
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
