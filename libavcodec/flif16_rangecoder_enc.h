/*
 * This file shouldn't be posted as a patch. It is to be merged with
 * flif16_rangecoder.c.
 */

#include "flif16_rangecoder.h"

void ff_flif16_rac_enc_init(FLIF16RangeCoder *rc, PutByteContext *gb);

int ff_flif16_rac_enc_renorm(FLIF16RangeCoder *rc);

int ff_flif16_rac_enc_flush(FLIF16RangeCoder *rc);

int ff_flif16_rac_enc_put(FLIF16RangeCoder *rc, uint32_t chance,
                          uint8_t bit);

int ff_flif16_rac_enc_write_bit(FLIF16RangeCoder *rc, uint8_t bit);

int ff_flif16_rac_enc_write_uni_int(FLIF16RangeCoder *rc, int min,
                                    int max, int val, int type);

int ff_flif16_rac_enc_write_chance(FLIF16RangeCoder *rc,
                                   uint64_t b12, uint8_t bit);

int ff_flif16_rac_enc_write_nz_int(FLIF16RangeCoder *rc, FLIF16ChanceContext *ctx,
                                   int min, int max, int value);

int ff_flif16_rac_enc_write_gnz_int(FLIF16RangeCoder *rc,
                                    FLIF16ChanceContext *ctx,
                                    int min, int max, int value);

static inline int ff_flif16_rac_enc_process(FLIF16RangeCoder *rc,
                                            void *ctx, int val1, int val2,
                                            int value, int type);

static inline int ff_flif16_rac_enc_process(FLIF16RangeCoder *rc,
                                            void *ctx, int val1, int val2,
                                            int value, int type)
{
    int flag = 0;
    while (!flag) {
        if (!ff_flif16_rac_enc_renorm(rc)) {
            return 0; // EAGAIN condition
        }

        switch (type) {
        case FLIF16_RAC_BIT:
            flag = ff_flif16_rac_enc_write_bit(rc, (uint8_t) value);
            break;

        case FLIF16_RAC_UNI_INT8:
        case FLIF16_RAC_UNI_INT16:
        case FLIF16_RAC_UNI_INT32:
            flag = ff_flif16_rac_enc_write_uni_int(rc, val1, val2, value, type);
            break;

        case FLIF16_RAC_CHANCE:
            flag = ff_flif16_rac_enc_write_chance(rc, val1, (uint8_t) value);
            break;

        case FLIF16_RAC_NZ_INT:
            flag = ff_flif16_rac_enc_write_nz_int(rc, (FLIF16ChanceContext *) ctx,
                                                  val1, val2, value);
            break;

        case FLIF16_RAC_GNZ_INT:
            flag = ff_flif16_rac_enc_write_gnz_int(rc, (FLIF16ChanceContext *) ctx,
                                                   val1, val2, value);
            break;
/*
#ifdef MULTISCALE_CHANCES_ENABLED
        case FLIF16_RAC_NZ_MULTISCALE_INT:
            flag = ff_flif16_rac_read_nz_multiscale_int(rc, (FLIF16MultiscaleChanceContext *) ctx,
                                                        val1, val2, (int *) target);
            break;

        case FLIF16_RAC_GNZ_MULTISCALE_INT:
            flag = ff_flif16_rac_read_gnz_multiscale_int(rc, (FLIF16MultiscaleChanceContext *) ctx,
                                                         val1, val2, (int *) target);
            break;
#endif
*/
        }
    }
    return 1;
}

#define RAC_PUT(rc, ctx, val1, val2, value, type) \
    if (!ff_flif16_rac_enc_process((rc), (ctx), (val1), (val2), (value), (type))) {\
        goto need_more_data;\
    }

#define MANIAC_PUT(rc, m, prop, channel, min, max, value) \
    if (!ff_flif16_maniac_write_int((rc), (m), (prop), (channel), (min), (max), (value))) {\
        goto need_more_data;\
    }
