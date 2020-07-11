/*
 * Transforms for FLIF16.
 * Copyright (c) 2020 Kartik K. Khullar <kartikkhullar840@gmail.com>
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
 * Transforms for FLIF16.
 */

#include "flif16_transform.h"
#include "flif16_rangecoder.h"
#include "libavutil/common.h"


// Transform private structs

typedef struct ranges_priv_channelcompact {
    int nb_colors[4];
} ranges_priv_channelcompact;

typedef struct transform_priv_ycocg {
    int origmax4;
    FLIF16RangesContext *r_ctx;
} transform_priv_ycocg;

typedef struct transform_priv_permuteplanes {
    uint8_t subtract;
    uint8_t permutation[5];
    FLIF16RangesContext *r_ctx;

    uint8_t from[4], to[4];
    FLIF16ChanceContext ctx_a;
} transform_priv_permuteplanes;

typedef struct transform_priv_channelcompact {
    FLIF16ColorVal *CPalette[4];
    unsigned int CPalette_size[4];
    FLIF16ColorVal *CPalette_inv[4];
    unsigned int CPalette_inv_size[4];

    FLIF16ColorVal min;
    int remaining;
    unsigned int i;                   //Iterator for nested loop.
    FLIF16ChanceContext ctx_a;
} transform_priv_channelcompact;

typedef struct transform_priv_bounds {
    FLIF16ColorVal (*bounds)[2];
    int min;
    FLIF16ChanceContext ctx_a;
} transform_priv_bounds;

typedef struct transform_priv_palette{
    uint8_t has_alpha;
    uint8_t ordered_palette;
    uint32_t max_palette_size;
    FLIF16ColorVal (*Palette)[3];
    FLIF16ColorVal min[3], max[3];
    FLIF16ColorVal *prev;
    FLIF16ColorVal pp[2];
    FLIF16ColorVal Y, I, Q;
    FLIF16ChanceContext ctx;
    FLIF16ChanceContext ctxY;
    FLIF16ChanceContext ctxI;
    FLIF16ChanceContext ctxQ;
    long unsigned size;
    uint8_t sorted;
    unsigned int p;       //Iterator
}transform_priv_palette;

typedef struct transform_priv_palettealpha{
    FLIF16ColorVal (*Palette)[4];
    unsigned int max_palette_size;
    uint8_t alpha_zero_special;
    uint8_t ordered_palette;
    uint8_t already_has_palette;
    FLIF16ColorVal min[4], max[4];
    FLIF16ColorVal *prev;
    FLIF16ColorVal pp[2];
    FLIF16ColorVal Y, I, Q, A;
    FLIF16ChanceContext ctx;
    FLIF16ChanceContext ctxY;
    FLIF16ChanceContext ctxI;
    FLIF16ChanceContext ctxQ;
    FLIF16ChanceContext ctxA;
    unsigned int p;
    uint8_t sorted;
    long unsigned int size;
}transform_priv_palettealpha;

typedef int16_t ColorValCB;
typedef struct ColorValCB_list ColorValCB_list ;

typedef struct ColorValCB_list{
    ColorValCB data;
    ColorValCB_list *next;
}ColorValCB_list;

typedef struct ColorBucket{
    ColorValCB_list *snapvalues;
    unsigned int snapvalues_size;
    ColorValCB_list *values;
    unsigned int values_size;
    ColorValCB min, max;
    uint8_t discrete;
}ColorBucket;


typedef struct ColorBuckets{
    ColorBucket bucket0;
    int min0, min1;
    ColorBucket *bucket1;
    unsigned int bucket1_size;
    ColorBucket **bucket2; // list of a list
    unsigned int bucket2_size, *bucket2_list_size;
    ColorBucket bucket3;
    ColorBucket empty_bucket;
    const FLIF16RangesContext *ranges;
}ColorBuckets;

typedef struct transform_priv_colorbuckets{
    ColorBuckets *cb;
    uint8_t really_used;
}transform_priv_colorbuckets;

typedef struct ranges_priv_ycocg {
    int origmax4;
    FLIF16RangesContext *r_ctx;
} ranges_priv_ycocg;

typedef struct ranges_priv_permuteplanes {
    uint8_t permutation[5];
    FLIF16RangesContext *r_ctx;
} ranges_priv_permuteplanes;

typedef struct ranges_priv_bounds {
    FLIF16ColorVal (*bounds)[2];
    FLIF16RangesContext *r_ctx;
} ranges_priv_bounds;

typedef struct ranges_priv_palette{
    int nb_colors;
    FLIF16RangesContext *r_ctx;
}ranges_priv_palette;

static void insert_colorbucket(ColorValCB_list *list, unsigned int pos, ColorValCB val)
{
    ColorValCB_list *temp = list;
    for(int i = 1; i < pos; i++){
        temp = temp->next;
    }
    ColorValCB_list elem;
    elem.data = val;
    elem.next = temp->next;
    temp->next = &elem;
}

static ColorValCB colorbucket_at(ColorValCB_list *list, unsigned int pos)
{
    ColorValCB_list *temp = list;
    for(int i = 0; i < pos; i++){
        temp = temp->next;
    }
    return temp->data;
}

/*
void colorbucket_addcolor(ColorBucket *cb, const FLIF16ColorVal c, const unsigned int max_per_bucket)
{
    if(c < cb->min)
        cb->min = c;
    if(c > cb->max)
        cb->max = c;
    if(cb->discrete){
        unsigned int pos = 0;
        for(; pos < cb->values_size; pos++){
            if(c == colorbucket_at(cb->values, pos))
                return;
            if(colorbucket_at(cb->values, pos) > c)
                break;
        }
        if(cb->values_size < max_per_bucket){
            insert_colorbucket(cb->values, pos, c);
            cb->values_size++;
        }
    }
}
*/

typedef struct ranges_priv_colorbuckets{
    FLIF16RangesContext *r_ctx;
    ColorBuckets *buckets;
}ranges_priv_colorbuckets;

typedef struct ranges_priv_static {
    FLIF16ColorVal (*bounds)[2];
}ranges_priv_static;


/*
 * =============================================================================
 * Ranges
 * =============================================================================
 */

/*
 * Static
 */

static FLIF16ColorVal ff_static_min(FLIF16RangesContext* r_ctx,
                                    int p)
{
    ranges_priv_static* data = r_ctx->priv_data;
    if(p >= r_ctx->num_planes)
        return 0;
    av_assert0(p < r_ctx->num_planes);
    return data->bounds[p][0];
}

static FLIF16ColorVal ff_static_max(FLIF16RangesContext* r_ctx,
                                    int p)
{
    ranges_priv_static* data = r_ctx->priv_data;
    if(p >= r_ctx->num_planes)
        return 0;
    av_assert0(p < r_ctx->num_planes);
    return data->bounds[p][1];
}

static void ff_static_minmax(FLIF16RangesContext *src_ctx ,const int p,
                             FLIF16ColorVal* prev_planes,
                             FLIF16ColorVal* minv, FLIF16ColorVal* maxv)
{
    FLIF16Ranges* ranges = flif16_ranges[src_ctx->r_no];
    *minv = ranges->min(src_ctx, p);
    *maxv = ranges->max(src_ctx, p);
    //printf("s minmax %d %d %d\n", ranges->min(src_ctx, p), ranges->max(src_ctx, p), p);
}

static void ff_static_snap(FLIF16RangesContext *src_ctx , const int p,
                           FLIF16ColorVal *prev_planes,
                           FLIF16ColorVal *minv, FLIF16ColorVal *maxv, 
                           FLIF16ColorVal *v)
{
    //printf("static_snap\n");
    //printf("s1 %d %d %d\n", *minv, *maxv, *v);
    ff_flif16_ranges_minmax(src_ctx, p, prev_planes, minv, maxv);
    //printf("s2 %d %d %d\n",*minv, *maxv, *v);
    if(*minv > *maxv)
        *maxv = *minv;
    //printf("s3 %d %d %d\n", *minv, *maxv, *v);
    *v = av_clip(*v, *minv, *maxv);
    //printf("s4 %d %d %d\n===\n", *minv, *maxv, *v);
}

static void ff_static_close(FLIF16RangesContext *r_ctx){
    ranges_priv_static *data = r_ctx->priv_data;
    av_free(data->bounds);
}

/*
 * ChannelCompact
 */

static FLIF16ColorVal ff_channelcompact_min(FLIF16RangesContext* ranges, 
                                     int p){
    return 0;
}

static FLIF16ColorVal ff_channelcompact_max(FLIF16RangesContext* src_ctx,
                                     int p){
    ranges_priv_channelcompact* data = src_ctx->priv_data;
    return data->nb_colors[p];
}

static void ff_channelcompact_minmax(FLIF16RangesContext *r_ctx, int p,
                              FLIF16ColorVal* prev_planes,
                              FLIF16ColorVal* minv,
                              FLIF16ColorVal* maxv)
{
    ranges_priv_channelcompact* data = r_ctx->priv_data;
    *minv = 0;
    *maxv = data->nb_colors[p];
}

/*
 * YCoCg
 */

static inline FLIF16ColorVal ff_get_max_y(int origmax4)
{
    return 4 * origmax4-1;
}

static inline int ff_get_min_co(int origmax4, int yval)
{
    if (yval < origmax4 - 1)
        return -3 - 4*yval; 
    else if (yval >= 3 * origmax4)
        return 4*(1 + yval - 4*origmax4);
    else
        return -4*origmax4 + 1;
}

static inline int ff_get_max_co(int origmax4, int yval)
{
    if (yval < origmax4-1)
        return 3 + 4 * yval; 
    else if (yval >= 3 * origmax4)
        return 4*origmax4 - 4*(1 + yval - 3*origmax4);
    else
        return 4 * origmax4 - 1;
}

static inline int ff_get_min_cg(int origmax4, int yval, int coval)
{
    if (yval < origmax4 - 1)
        return -(2*yval+1); 
    else if (yval >= 3 * origmax4)
        return -(2*(4*origmax4 - 1 - yval) - ((1 + abs(coval))/2)*2);
    else{
        return -FFMIN(2*origmax4 - 1 + (yval -origmax4 + 1)*2, 
                     2*origmax4 + (3*origmax4 - 1 - yval)*2 - ((1 + abs(coval))/2)*2);
    }
}

static inline int ff_get_max_cg(int origmax4, int yval, int coval){
    if (yval < origmax4 - 1)
        return 1 + 2 * yval - 2 * (abs(coval) / 2); 
    else if (yval >= 3 * origmax4)
        return 2 * (4*origmax4 - 1 - yval);
    else
        return -FFMAX(-4*origmax4 + (1 + yval - 2*origmax4)*2, 
                      -2*origmax4 - (yval - origmax4)*2 - 1 + (abs(coval)/2)*2);
}

static FLIF16ColorVal ff_ycocg_min(FLIF16RangesContext* r_ctx, int p)
{   
    ranges_priv_ycocg* data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    switch(p) {
        case FLIF16_PLANE_Y:
            return 0;
        case FLIF16_PLANE_CO:
        case FLIF16_PLANE_CG:
            return -4 * data->origmax4 + 1;
        default:
            return ranges->min(data->r_ctx, p);
    }
}

static FLIF16ColorVal ff_ycocg_max(FLIF16RangesContext* r_ctx, int p)
{
    ranges_priv_ycocg* data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    switch(p) {
        case FLIF16_PLANE_Y:
        case FLIF16_PLANE_CO:
        case FLIF16_PLANE_CG:
            return 4 * data->origmax4 - 1;
        default:
            return ranges->max(data->r_ctx, p);
    }
}

static void ff_ycocg_minmax(FLIF16RangesContext *r_ctx ,const int p,
                             FLIF16ColorVal* prev_planes,
                             FLIF16ColorVal* minv,
                             FLIF16ColorVal* maxv)
{
    ranges_priv_ycocg* data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    //printf("ycocg_minmax\n");
    switch(p){
        case FLIF16_PLANE_Y:
            *minv = 0;
            *maxv = ff_get_max_y(data->origmax4);
            break;
        case FLIF16_PLANE_CO:
            *minv = ff_get_min_co(data->origmax4, prev_planes[0]);
            *maxv = ff_get_max_co(data->origmax4, prev_planes[0]);
            break;    
        case FLIF16_PLANE_CG:
            *minv = ff_get_min_cg( data->origmax4, prev_planes[0], prev_planes[1]);
            *maxv = ff_get_max_cg( data->origmax4, prev_planes[0], prev_planes[1]);
            break;
        default:
            ranges->minmax(data->r_ctx, p, prev_planes, minv, maxv);
    }
    //printf("y minmax %d %d %d\n", *minv, *maxv, p);
}

static void ff_ycocg_close(FLIF16RangesContext *r_ctx){
    ranges_priv_ycocg *data = r_ctx->priv_data;
    flif16_ranges[data->r_ctx->r_no]->close(data->r_ctx);
    av_free(data->r_ctx);
}

/*
 * PermutePlanesSubtract
 */

static FLIF16ColorVal ff_permuteplanessubtract_min(FLIF16RangesContext* r_ctx,
                                            int p)
{
    ranges_priv_permuteplanes* data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    if(p==0 || p>2)
        return ranges->min(data->r_ctx, data->permutation[p]);
    return ranges->min(data->r_ctx, data->permutation[p]) - 
           ranges->max(data->r_ctx, data->permutation[0]);
}

static FLIF16ColorVal ff_permuteplanessubtract_max(FLIF16RangesContext* r_ctx,
                                            int p)
{
    ranges_priv_permuteplanes* data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    if(p==0 || p>2)
        return ranges->max(data->r_ctx, data->permutation[p]);
    return ranges->max(data->r_ctx, data->permutation[p]) - 
           ranges->min(data->r_ctx, data->permutation[0]);
}

static void ff_permuteplanessubtract_minmax(FLIF16RangesContext* r_ctx, int p,
                                     FLIF16ColorVal* prev_planes, 
                                     FLIF16ColorVal* minv, FLIF16ColorVal* maxv)
{
    ranges_priv_permuteplanes* data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    if(p==0 || p>2){
        *minv = ranges->min(data->r_ctx, p);
        *maxv = ranges->max(data->r_ctx, p);
    }
    else{
        *minv = ranges->min(data->r_ctx, data->permutation[p]) - prev_planes[0];
        *maxv = ranges->max(data->r_ctx, data->permutation[p]) - prev_planes[0];
    }
}


/*
 * PermutePlanes
 */

static FLIF16ColorVal ff_permuteplanes_min(FLIF16RangesContext* r_ctx,
                                    int p)
{
    transform_priv_permuteplanes* data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    return ranges->min(data->r_ctx, data->permutation[p]);
}

static FLIF16ColorVal ff_permuteplanes_max(FLIF16RangesContext* r_ctx,
                                    int p)
{
    transform_priv_permuteplanes* data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    return ranges->max(data->r_ctx, data->permutation[p]);
}

static void ff_permuteplanes_close(FLIF16RangesContext *r_ctx){
    ranges_priv_permuteplanes *data = r_ctx->priv_data;
    flif16_ranges[data->r_ctx->r_no]->close(data->r_ctx);
    av_free(data->r_ctx);
}

/*
 * Bounds
 */

static FLIF16ColorVal ff_bounds_min(FLIF16RangesContext* r_ctx, int p)
{
    ranges_priv_bounds* data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    av_assert0(p < r_ctx->num_planes);
    return FFMAX(ranges->min(data->r_ctx, p), data->bounds[p][0]);
}

static FLIF16ColorVal ff_bounds_max(FLIF16RangesContext* r_ctx, int p)
{
    ranges_priv_bounds* data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    av_assert0(p < r_ctx->num_planes);
    return FFMIN(ranges->max(data->r_ctx, p), data->bounds[p][1]);
}

static void ff_bounds_minmax(FLIF16RangesContext* r_ctx, 
                      int p, FLIF16ColorVal* prev_planes,
                      FLIF16ColorVal* minv, FLIF16ColorVal* maxv)
{
    ranges_priv_bounds *data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    //printf("bounds_minmax\n");
    av_assert0(p < r_ctx->num_planes);
    if(p==0 || p==3){
        *minv = data->bounds[p][0];
        *maxv = data->bounds[p][1];
        //printf("b1 min = %d max = %d\n", *minv, *maxv);
        return;
    }
    ranges->minmax(data->r_ctx, p, prev_planes, minv, maxv);
    //printf("b2 min = %d max = %d\n", *minv, *maxv);
    if(*minv < data->bounds[p][0])
        *minv = data->bounds[p][0];
    //printf("b3 min = %d max = %d\n", *minv, *maxv);
    if(*maxv > data->bounds[p][1])
        *maxv = data->bounds[p][1];
    //printf("b4 min = %d max = %d\n", *minv, *maxv);
    if(*minv > *maxv){
        *minv = data->bounds[p][0];
        *maxv = data->bounds[p][1];
        //printf("b5 min = %d max = %d\n", *minv, *maxv);
    }
    //printf("b6 min = %d max = %d\n", *minv, *maxv);
    av_assert0(*minv <= *maxv);
}

static void ff_bounds_snap(FLIF16RangesContext* r_ctx, 
                    int p, FLIF16ColorVal* prev_planes,
                    FLIF16ColorVal* minv,
                    FLIF16ColorVal* maxv,
                    FLIF16ColorVal* v)
{
    //printf("bounds_snap\n");
    ranges_priv_bounds *data = r_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    if (p==0 || p==3) {
        *minv = data->bounds[p][0];
        *maxv = data->bounds[p][1];
        //printf("b1 %d %d %d\n", *minv, *maxv, *v);
    } else {
        ranges->snap(data->r_ctx, p, prev_planes, minv, maxv, v);
        if(*minv < data->bounds[p][0])
            *minv = data->bounds[p][0];
        if (*maxv > data->bounds[p][1])
            *maxv = data->bounds[p][1];
        //printf("b2 %d %d %d\n", *minv, *maxv, *v);
        if (*minv > *maxv) {
            *minv = data->bounds[p][0];
            *maxv = data->bounds[p][1];
        }
        //printf("b3 %d %d %d\n", *minv, *maxv, *v);
    }
    if(*v > *maxv)
        *v = *maxv;
    if(*v < *minv)
        *v = *minv;
    //printf("b4 %d %d %d\n", *minv, *maxv, *v);
}

static void ff_bounds_close(FLIF16RangesContext *r_ctx){
    ranges_priv_bounds *data = r_ctx->priv_data;
    flif16_ranges[data->r_ctx->r_no]->close(data->r_ctx);
    av_free(data->bounds);
    av_free(data->r_ctx);
}

/*
 * Palette
 */

static FLIF16ColorVal ff_palette_min(FLIF16RangesContext *r_ctx, int p){
    ranges_priv_palette *data = r_ctx->priv_data;
    if(p < 3)
        return 0;
    else
        return ff_flif16_ranges_min(data->r_ctx, p); 
}

static FLIF16ColorVal ff_palette_max(FLIF16RangesContext *r_ctx, int p){
    ranges_priv_palette *data = r_ctx->priv_data;
    if(p==1)
        return data->nb_colors-1;
    else if(p < 3)
        return 0;
    else
        return ff_flif16_ranges_max(data->r_ctx, p);
}

static void ff_palette_minmax(FLIF16RangesContext* r_ctx, 
                             int p, FLIF16ColorVal* prev_planes,
                             FLIF16ColorVal* minv, FLIF16ColorVal* maxv)
{
    ranges_priv_palette *data = r_ctx->priv_data;
    if(p == 1){
        *minv = 0;
        *maxv = data->nb_colors-1;
    }
    else if(p < 3){
        *minv = 0;
        *maxv = 0;
    }
    else
        ff_flif16_ranges_minmax(data->r_ctx, p, prev_planes, minv, maxv);
}

static void ff_palette_close(FLIF16RangesContext *r_ctx){
    ranges_priv_palette *data = r_ctx->priv_data;
    flif16_ranges[data->r_ctx->r_no]->close(data->r_ctx);
    av_free(data->r_ctx);
}

static FLIF16ColorVal ff_palettealpha_min(FLIF16RangesContext *r_ctx, int p){
    ranges_priv_palette *data = r_ctx->priv_data;
    if(p < 3)
        return 0;
    else if(p == 3)
        return 1;
    else
        return ff_flif16_ranges_min(data->r_ctx, p); 
}

static FLIF16ColorVal ff_palettealpha_max(FLIF16RangesContext *r_ctx, int p){
    ranges_priv_palette *data = r_ctx->priv_data;
    switch(p){
        case FLIF16_PLANE_Y:
            return 0;
        case FLIF16_PLANE_CO:
            return data->nb_colors-1;
        case FLIF16_PLANE_CG:
            return 0;
        case FLIF16_PLANE_ALPHA:
            return 1;
        default:
            return ff_flif16_ranges_max(data->r_ctx, p);
    }
}

static void ff_palettealpha_minmax(FLIF16RangesContext* r_ctx, 
                                   int p, FLIF16ColorVal* prev_planes,
                                   FLIF16ColorVal* minv, FLIF16ColorVal* maxv)
{
    ranges_priv_palette *data = r_ctx->priv_data;
    if(p == 1){
        *minv = 0;
        *maxv = data->nb_colors-1;
    }
    else if(p < 3){
        *minv = 0;
        *maxv = 0;
    }
    else if(p == 3){
        *minv = 1;
        *maxv = 1;
    }
    else
        ff_flif16_ranges_minmax(data->r_ctx, p, prev_planes, minv, maxv);
}

// quantization constants
#define CB0a 1
#define CB0b 1
#define CB1 4

// function below incomplete
/*
ColorBucket *ff_bucket_buckets(ColorBuckets *buckets, const int p, const FLIF16ColorVal *prev_planes){
    av_assert0(p >= 0);
    av_assert0(p < 4);
    if(p == 0)
        return &buckets->bucket0;
    if(p == 1){
        av_assert0(((prev_planes[0] - buckets->min0) >= 0) 
        && ((prev_planes[0] - buckets->min0) < buckets->bucket1_size));
        return &buckets->bucket1[(prev_planes[0] - buckets->min0)];
    }
}
*/
static void ff_init_bucket_default(ColorBucket *b){
    b->min = 10000;
    b->max = -10000;
    b->discrete = 1;
}

static ColorBucket *ff_bucket_buckets(ColorBuckets *buckets, const int p, const FLIF16ColorVal *prev_planes){
    av_assert0(p >= 0);
    av_assert0(p < 4);
    if(p == 0)
        return &buckets->bucket0;
    if(p == 1){
        int i = (prev_planes[0] - buckets->min0)/CB0a;
        if(i >= 0 && i < (int)buckets->bucket1_size)
            return &buckets->bucket1[i];
        else
            return &buckets->empty_bucket;
    }
    if(p == 2){
        int i = (prev_planes[0] - buckets->min0)/CB0b;
        int j = (prev_planes[1] - buckets->min1)/CB1;
        if(i >= 0 && i < (int)buckets->bucket1_size && j >= 0 && j < (int) buckets->bucket2_list_size[i])
            return &buckets->bucket2[i][j];
        else
            return &buckets->empty_bucket;
    }
    else
        return &buckets->bucket3;
}

static FLIF16ColorVal *ff_snap_color_bucket(ColorBucket *bucket, const FLIF16ColorVal c)
{
    if(c <= bucket->min)
        return bucket->min;
    if(c >= bucket->max)
        return bucket->max;
    if(bucket->discrete){
        av_assert0((FLIF16ColorVal)bucket->snapvalues_size > (c - bucket->min));
        return colorbucket_at(bucket->snapvalues, c - bucket->min);
    }
    return c;
}

static FLIF16ColorVal ff_colorbuckets_min(FLIF16RangesContext *r_ctx, int p)
{
    ranges_priv_colorbuckets *data = r_ctx->priv_data;
    return ff_flif16_ranges_min(data->r_ctx, p);
}

static FLIF16ColorVal ff_colorbuckets_max(FLIF16RangesContext *r_ctx, int p)
{
    ranges_priv_colorbuckets *data = r_ctx->priv_data;
    return ff_flif16_ranges_max(data->r_ctx, p);
}

static void ff_colorbuckets_snap(FLIF16RangesContext *src_ctx,
                                          const int p,
                                          FLIF16ColorVal *prev_planes,
                                          FLIF16ColorVal *minv,
                                          FLIF16ColorVal *maxv, 
                                          FLIF16ColorVal *v)
{
    ranges_priv_colorbuckets *data = src_ctx->priv_data;
    const ColorBucket *b = ff_bucket_buckets(data->buckets, p, prev_planes);
    *minv = b->min;
    *maxv = b->max;
    if(b->min > b->max){
        *minv = ff_colorbuckets_min(src_ctx, p);
        *v = *minv;
        *maxv = ff_colorbuckets_max(src_ctx, p);
        return;
    }
    *v = ff_snap_color_bucket(b, v);
}

static void ff_colorbuckets_minmax(FLIF16RangesContext* r_ctx, 
                                   int p, FLIF16ColorVal* prev_planes,
                                   FLIF16ColorVal* minv, FLIF16ColorVal* maxv)
{
    ranges_priv_colorbuckets *data = r_ctx->priv_data;
    const ColorBucket *b = ff_bucket_buckets(data->buckets, p, prev_planes);
    *minv = b->min;
    *maxv = b->max;
    if(b->min > b->max){
        *minv = ff_colorbuckets_min(r_ctx, p);
        *maxv = ff_colorbuckets_max(r_ctx, p);
    }
}

FLIF16Ranges flif16_ranges_static = {
    .priv_data_size = sizeof(ranges_priv_static),
    .min            = &ff_static_min,
    .max            = &ff_static_max,
    .minmax         = &ff_static_minmax,
    .snap           = &ff_static_snap,
    .is_static      = 1,
    .close          = &ff_static_close
};

FLIF16Ranges flif16_ranges_channelcompact = {
    .priv_data_size = sizeof(ranges_priv_channelcompact),
    .min            = &ff_channelcompact_min,
    .max            = &ff_channelcompact_max,
    .minmax         = &ff_channelcompact_minmax,
    .snap           = &ff_static_snap,
    .is_static      = 1,
    .close          = NULL
};

FLIF16Ranges flif16_ranges_ycocg = {
    .priv_data_size = sizeof(ranges_priv_ycocg),
    .min            = &ff_ycocg_min,
    .max            = &ff_ycocg_max,
    .minmax         = &ff_ycocg_minmax,
    .snap           = &ff_static_snap,
    .is_static      = 0,
    .close          = &ff_ycocg_close
};

FLIF16Ranges flif16_ranges_permuteplanessubtract = {
    .priv_data_size = sizeof(ranges_priv_permuteplanes),
    .min            = &ff_permuteplanessubtract_min,
    .max            = &ff_permuteplanessubtract_max,
    .minmax         = &ff_permuteplanessubtract_minmax,
    .snap           = &ff_static_snap,
    .is_static      = 0,
    .close          = &ff_permuteplanes_close
};

FLIF16Ranges flif16_ranges_permuteplanes = {
    .priv_data_size = sizeof(ranges_priv_permuteplanes),
    .min            = &ff_permuteplanes_min,
    .max            = &ff_permuteplanes_max,
    .minmax         = &ff_static_minmax,
    .snap           = &ff_static_snap,
    .is_static      = 0,
    .close          = &ff_permuteplanes_close
};

FLIF16Ranges flif16_ranges_bounds = {
    .priv_data_size = sizeof(ranges_priv_bounds),
    .min            = &ff_bounds_min,
    .max            = &ff_bounds_max,
    .minmax         = &ff_bounds_minmax,
    .snap           = &ff_bounds_snap,
    .is_static      = 0,
    .close          = &ff_bounds_close
};

FLIF16Ranges flif16_ranges_palette = {
    .priv_data_size = sizeof(ranges_priv_palette),
    .min            = &ff_palette_min,
    .max            = &ff_palette_max,
    .minmax         = &ff_palette_minmax,
    .snap           = &ff_static_snap,
    .is_static      = 0,
    .close          = &ff_palette_close 
};

FLIF16Ranges flif16_ranges_palettealpha = {
    .priv_data_size = sizeof(ranges_priv_palette),
    .min            = &ff_palettealpha_min,
    .max            = &ff_palettealpha_max,
    .minmax         = &ff_palettealpha_minmax,
    .snap           = &ff_static_snap,
    .is_static      = 0,
    .close          = &ff_palette_close 
};

FLIF16Ranges flif16_ranges_colorbuckets = {
    .priv_data_size = sizeof(ranges_priv_colorbuckets),
    .min            = &ff_colorbuckets_min,
    .max            = &ff_colorbuckets_max,
    .minmax         = &ff_colorbuckets_minmax,
    .snap           = &ff_colorbuckets_snap,
    .is_static      = 0,
    //.close          = &ff_palette_close 
};

FLIF16Ranges* flif16_ranges[] = {
    &flif16_ranges_channelcompact,        // FLIF16_RANGES_CHANNELCOMPACT,
    &flif16_ranges_ycocg,                 // FLIF16_RANGES_YCOCG,
    &flif16_ranges_permuteplanes,         // FLIF16_RANGES_PERMUTEPLANES,
    &flif16_ranges_permuteplanessubtract, // FLIF16_RANGES_PERMUTEPLANESSUBTRACT,
    &flif16_ranges_bounds,                // FLIF16_RANGES_BOUNDS,
    &flif16_ranges_static,                // FLIF16_RANGES_STATIC,
    &flif16_ranges_palettealpha,          // FLIF16_RANGES_PALETTEALPHA,
    &flif16_ranges_palette,               // FLIF16_RANGES_PALETTE,
    NULL,                                 // FLIF16_RANGES_COLORBUCKETS,
    NULL,                                 // FLIF16_RANGES_DUPLICATEFRAME,
    NULL,                                 // FLIF16_RANGES_FRAMESHAPE,
    NULL,                                 // FLIF16_RANGES_FRAMELOOKBACK,
    NULL                                  // FLIF16_RANGES_DUP
};

FLIF16RangesContext *ff_flif16_ranges_static_init(unsigned int channels,
                                                  unsigned int bpc)
{
    FLIF16Ranges *r = flif16_ranges[FLIF16_RANGES_STATIC];
    FLIF16RangesContext *ctx = av_mallocz(sizeof(*ctx));
    ranges_priv_static *data;
    ctx->r_no       = FLIF16_RANGES_STATIC;
    ctx->num_planes = channels;
    ctx->priv_data  = av_mallocz(r->priv_data_size);
    data = ctx->priv_data;
    data->bounds = av_mallocz(sizeof(*data->bounds) * channels);
    for (int i = 0; i < channels; ++i) {
        data->bounds[i][0] = 0;
        data->bounds[i][1] = bpc;
    }
    return ctx;
}

static void ff_flif16_planes_get(FLIF16Context *ctx, FLIF16PixelData *frame,
                                 FLIF16ColorVal *values, uint32_t row, uint32_t col)
{
    for(int i = 0; i < 3; i++){
        values[i] = ff_flif16_pixel_get(ctx, frame, i, row, col);
    }
}

static void ff_flif16_planes_set(FLIF16Context *ctx, FLIF16PixelData *frame,
                                 FLIF16ColorVal *values, uint32_t row, uint32_t col)
{
    for(int i = 0; i < 3; i++){
        ff_flif16_pixel_set(ctx, frame, i, row, col, values[i]);
    }
}

/*
 * =============================================================================
 * Transforms
 * =============================================================================
 */

/*
 * YCoCg
 */
static int8_t transform_ycocg_init(FLIF16TransformContext *ctx, 
                                       FLIF16RangesContext* r_ctx)
{   
    transform_priv_ycocg *data = ctx->priv_data;
    FLIF16Ranges* src_ranges = flif16_ranges[r_ctx->r_no];
    
    if(   r_ctx->num_planes < 3   
       || src_ranges->min(r_ctx, 0) == src_ranges->max(r_ctx, 0) 
       || src_ranges->min(r_ctx, 1) == src_ranges->max(r_ctx, 1) 
       || src_ranges->min(r_ctx, 2) == src_ranges->max(r_ctx, 2)
       || src_ranges->min(r_ctx, 0) < 0 
       || src_ranges->min(r_ctx, 1) < 0 
       || src_ranges->min(r_ctx, 2) < 0)
        return 0;

    data->origmax4 = FFMAX3(src_ranges->max(r_ctx, 0), 
                            src_ranges->max(r_ctx, 1), 
                            src_ranges->max(r_ctx, 2))/4 + 1;
    //printf("origmax4 : %d\n", data->origmax4);
    data->r_ctx = r_ctx;
    return 1;
}

static FLIF16RangesContext* transform_ycocg_meta(FLIF16Context *ctx,
                                                 FLIF16PixelData *frame,
                                                 uint32_t frame_count,
                                                 FLIF16TransformContext* t_ctx,
                                                 FLIF16RangesContext* src_ctx)
{   
    FLIF16RangesContext *r_ctx;
    ranges_priv_ycocg* data;
    transform_priv_ycocg* trans_data = t_ctx->priv_data;
    r_ctx = av_mallocz(sizeof(FLIF16RangesContext));
    r_ctx->r_no = FLIF16_RANGES_YCOCG;
    r_ctx->priv_data = av_mallocz(sizeof(ranges_priv_ycocg));
    data = r_ctx->priv_data;
    
    data->origmax4 = trans_data->origmax4;
    data->r_ctx    = trans_data->r_ctx;
    
    r_ctx->num_planes = src_ctx->num_planes;
    return r_ctx;
}

static int8_t transform_ycocg_forward(FLIF16Context *ctx,
                                      FLIF16TransformContext* t_ctx,
                                      FLIF16PixelData* pixel_data)
{
    int r, c;
    FLIF16ColorVal RGB[3], YCOCG[3];

    int height = ctx->height;
    int width  = ctx->width;

    for (r=0; r<height; r++) {
        for (c=0; c<width; c++) {
            ff_flif16_planes_get(ctx, pixel_data, RGB, r, c);

            YCOCG[0] = (((RGB[0] + RGB[2])>>1) + RGB[1])>>1;
            YCOCG[1] = RGB[0] - RGB[2];
            YCOCG[2] = RGB[1] - ((RGB[0] + RGB[2])>>1);

            ff_flif16_planes_set(ctx, pixel_data, YCOCG, r, c);
        }
    }
    return 1;
}

static int8_t transform_ycocg_reverse(FLIF16Context *ctx,
                                      FLIF16TransformContext *t_ctx,
                                      FLIF16PixelData * pixel_data,
                                      uint32_t stride_row,
                                      uint32_t stride_col)
{
    int r, c;
    FLIF16ColorVal RGB[3], YCOCG[3];
    int height = ctx->height;
    int width  = ctx->width;
    transform_priv_ycocg *data = t_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];

    for (r=0; r<height; r+=stride_row) {
        for (c=0; c<width; c+=stride_col) {
            ff_flif16_planes_get(ctx, pixel_data, YCOCG, r, c);
  
            RGB[1] = YCOCG[0] - ((-YCOCG[2])>>1);
            RGB[2] = YCOCG[0] + ((1-YCOCG[2])>>1) - (YCOCG[1]>>1);
            RGB[0] = YCOCG[1] + RGB[2];

            RGB[0] = av_clip(RGB[0], 0, ranges->max(data->r_ctx, 0));
            RGB[1] = av_clip(RGB[1], 0, ranges->max(data->r_ctx, 1));
            RGB[2] = av_clip(RGB[2], 0, ranges->max(data->r_ctx, 2));

            ff_flif16_planes_set(ctx, pixel_data, RGB, r, c);
        }
    }
    return 1;
}

static void transform_ycocg_close(FLIF16TransformContext *ctx){
    transform_priv_ycocg *data = ctx->priv_data;
    av_free(data->r_ctx);
}

/*
 * PermutePlanes
 */

static int8_t transform_permuteplanes_init(FLIF16TransformContext* ctx, 
                                            FLIF16RangesContext* r_ctx)
{
    transform_priv_permuteplanes *data = ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[r_ctx->r_no];
    ff_flif16_chancecontext_init(&data->ctx_a);
    
    if( r_ctx->num_planes     < 3
      ||ranges->min(r_ctx, 0) < 0
      ||ranges->min(r_ctx, 1) < 0
      ||ranges->min(r_ctx, 2) < 0) 
        return 0;
    
    data->r_ctx = r_ctx;
    return 1;
}

static int8_t transform_permuteplanes_read(FLIF16TransformContext* ctx,
                                            FLIF16Context* dec_ctx,
                                            FLIF16RangesContext* r_ctx)
{
    int p;
    transform_priv_permuteplanes* data = ctx->priv_data;

    switch (ctx->segment) {
        case 0:
            RAC_GET(&dec_ctx->rc, &data->ctx_a, 0, 1, &data->subtract,
                    FLIF16_RAC_NZ_INT);
            //data->subtract = read_nz_int(rac, 0, 1);
            
            for(p=0; p<4; p++){
                data->from[p] = 0;
                data->to[p] = 0;
            }
        case 1:
            for (; ctx->i < dec_ctx->num_planes; ++ctx->i) {
                RAC_GET(&dec_ctx->rc, &data->ctx_a, 0, dec_ctx->num_planes-1,
                        &data->permutation[ctx->i], 
                        FLIF16_RAC_NZ_INT);
                //data->permutation[p] = read_nz_int(s->rc, 0, s->num_planes-1);
                data->from[ctx->i] = 1;
                data->to[ctx->i] = 1;
            }
            ctx->i = 0;

            for (p = 0; p < dec_ctx->num_planes; p++) {
                if(!data->from[p] || !data->to[p])
                return 0;
            }
            ++ctx->segment;
            goto end;
    }

    end:
        ctx->segment = 0;
        return 1;

    need_more_data:
        return AVERROR(EAGAIN);
}

static FLIF16RangesContext* transform_permuteplanes_meta(FLIF16Context *ctx,
                                                         FLIF16PixelData *frame,
                                                         uint32_t frame_count,
                                                         FLIF16TransformContext* t_ctx,
                                                         FLIF16RangesContext* src_ctx)
{
    FLIF16RangesContext* r_ctx = av_mallocz(sizeof(FLIF16RangesContext));
    transform_priv_permuteplanes* data = t_ctx->priv_data;
    ranges_priv_permuteplanes* priv_data = av_mallocz(sizeof(ranges_priv_permuteplanes));
    int i;
    if(data->subtract)
        r_ctx->r_no = FLIF16_RANGES_PERMUTEPLANESSUBTRACT;
    else
        r_ctx->r_no = FLIF16_RANGES_PERMUTEPLANES;
    r_ctx->num_planes = src_ctx->num_planes;
    for(i=0; i<5; i++){
        priv_data->permutation[i] = data->permutation[i];
    }
    priv_data->r_ctx       = data->r_ctx;
    r_ctx->priv_data = priv_data;
    return r_ctx;
}

static int8_t transform_permuteplanes_forward(FLIF16Context *ctx,
                                              FLIF16TransformContext *t_ctx,
                                              FLIF16PixelData* pixel_data)
{
    FLIF16ColorVal pixel[5];
    int r, c, p;
    int width  = ctx->width;
    int height = ctx->height;
    transform_priv_permuteplanes *data = t_ctx->priv_data;
    
    // Transforming pixel data.
    for (r=0; r<height; r++) {
        for (c=0; c<width; c++) {
            for (p=0; p<data->r_ctx->num_planes; p++)
                pixel[p] = ff_flif16_pixel_get(ctx, pixel_data, 0, r, c);
            ff_flif16_pixel_set(ctx, pixel_data, 0, r, c, pixel[data->permutation[0]]);
            if (!data->subtract){
                for (p=1; p<data->r_ctx->num_planes; p++)
                    ff_flif16_pixel_set(ctx, pixel_data, p, r, c, pixel[data->permutation[p]]);
            }
            else{ 
                for(p=1; p<3 && p<data->r_ctx->num_planes; p++)
                    ff_flif16_pixel_set(ctx, pixel_data, p, r, c, 
                    pixel[data->permutation[p]] - pixel[data->permutation[0]]);
                for(p=3; p<data->r_ctx->num_planes; p++)
                    ff_flif16_pixel_set(ctx, pixel_data, p, r, c, pixel[data->permutation[p]]);
            }
        }
    }
    return 1;
}

static int8_t transform_permuteplanes_reverse(FLIF16Context *ctx,
                                              FLIF16TransformContext *t_ctx,
                                              FLIF16PixelData * frame,
                                              uint32_t stride_row,
                                              uint32_t stride_col)
{   
    int p, r, c;
    FLIF16ColorVal pixel[5];
    transform_priv_permuteplanes *data = t_ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[data->r_ctx->r_no];
    int height = ctx->height;
    int width  = ctx->width;
    printf("Permute Planes Reverse\n");    
    for (r=0; r<height; r+=stride_row) {
        for (c=0; c<width; c+=stride_col) {
            for (p=0; p<data->r_ctx->num_planes; p++)
                pixel[p] =  ff_flif16_pixel_get(ctx, frame, p, r, c);
            for (p=0; p<data->r_ctx->num_planes; p++)
                ff_flif16_pixel_set(ctx, frame, data->permutation[p], r, c, pixel[p]);
            
            ff_flif16_pixel_set(ctx, frame, data->permutation[0], r, c, pixel[0]);
            if (!data->subtract) {
                for (p=1; p<data->r_ctx->num_planes; p++)
                    ff_flif16_pixel_set(ctx, frame, data->permutation[p], r, c, pixel[p]);
            } else {
                for (p=1; p<3 && p<data->r_ctx->num_planes; p++)
                    ff_flif16_pixel_set(ctx, frame, data->permutation[p], r, c,
                    av_clip(pixel[p] + pixel[0],
                         ranges->min(data->r_ctx, data->permutation[p]),
                         ranges->max(data->r_ctx, data->permutation[p])));
                for (p=3; p<data->r_ctx->num_planes; p++)
                    ff_flif16_pixel_set(ctx, frame, data->permutation[p], r, c, pixel[p]);
            }
        }
    }
    for(int p=0; p<data->r_ctx->num_planes; p++){
    for(r=0; r<height; r++){
        for(c=0; c<width; c++){
            printf("%d ", ff_flif16_pixel_get(ctx, frame, p, r, c));
        }
        printf("\n");
    }
    }
    return 1;
}

static void transform_permuteplanes_close(FLIF16TransformContext *ctx){
    transform_priv_permuteplanes *data = ctx->priv_data;
    av_free(data->r_ctx);
}

/*
 * ChannelCompact
 */

static int8_t transform_channelcompact_init(FLIF16TransformContext *ctx, 
                                                FLIF16RangesContext* src_ctx)
{
    int p;
    transform_priv_channelcompact *data = ctx->priv_data;
    if(src_ctx->num_planes > 4)
        return 0;
    
    for(p=0; p<4; p++){
        data->CPalette[p]       = 0;
        data->CPalette_size[p]  = 0;
    }    
    ff_flif16_chancecontext_init(&data->ctx_a);
    return 1;
}

static int8_t transform_channelcompact_read(FLIF16TransformContext * ctx,
                                             FLIF16Context *dec_ctx,
                                             FLIF16RangesContext* src_ctx)
{
    unsigned int nb;
    transform_priv_channelcompact *data = ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[src_ctx->r_no];
    start:
    switch (ctx->segment) {
        case 0:
            if(ctx->i < dec_ctx->num_planes) {
                RAC_GET(&dec_ctx->rc, &data->ctx_a,
                        0, ranges->max(src_ctx, ctx->i) -
                        ranges->min(src_ctx, ctx->i),
                        &nb, FLIF16_RAC_NZ_INT);
                nb += 1;
                data->min = ranges->min(src_ctx, ctx->i);
                data->CPalette[ctx->i] = av_mallocz(nb*sizeof(FLIF16ColorVal));
                data->CPalette_size[ctx->i] = nb;
                data->remaining = nb-1;
                ++ctx->segment;
                goto next_case;
            }
            ctx->i = 0;
            goto end;
        
        next_case:
        case 1:
            for (; data->i < data->CPalette_size[ctx->i]; ++data->i) {
                RAC_GET(&dec_ctx->rc, &data->ctx_a,
                        0, ranges->max(src_ctx, ctx->i)-data->min-data->remaining,
                        &data->CPalette[ctx->i][data->i], 
                        FLIF16_RAC_NZ_INT);
                data->CPalette[ctx->i][data->i] += data->min;
                data->min = data->CPalette[ctx->i][data->i]+1;
                data->remaining--;
            }
            data->i = 0;
            ctx->segment--;
            ctx->i++;
            goto start;
    }
    
    end:
        printf("Channel Compact Read : \n");
        for(int j=0; j<dec_ctx->num_planes; j++){
            for(int i=0; i<data->CPalette_size[j]; i++)
                printf("%d ", data->CPalette[j][i]);
            printf("\n");
        }
        ctx->segment = 0;
        return 1;

    need_more_data:
        return AVERROR(EAGAIN);
}

static FLIF16RangesContext* transform_channelcompact_meta(FLIF16Context *ctx,
                                                          FLIF16PixelData *frame,
                                                          uint32_t frame_count,
                                                          FLIF16TransformContext* t_ctx,
                                                          FLIF16RangesContext* src_ctx)
{
    int i;
    FLIF16RangesContext* r_ctx = av_mallocz(sizeof(FLIF16RangesContext));
    ranges_priv_channelcompact* data = av_mallocz(sizeof(ranges_priv_channelcompact));
    transform_priv_channelcompact* trans_data = t_ctx->priv_data;
    r_ctx->num_planes = src_ctx->num_planes;
    for(i=0; i<src_ctx->num_planes; i++){
        data->nb_colors[i] = trans_data->CPalette_size[i] - 1;
    }
    r_ctx->priv_data = data;
    r_ctx->r_no = FLIF16_RANGES_CHANNELCOMPACT;
    return r_ctx;
}

static int8_t transform_channelcompact_reverse(FLIF16Context *ctx,
                                               FLIF16TransformContext* t_ctx,
                                               FLIF16PixelData* frame,
                                               uint32_t stride_row,
                                               uint32_t stride_col)
{
    int p, P;
    uint32_t r, c;
    FLIF16ColorVal* palette;
    unsigned int palette_size;
    transform_priv_channelcompact *data = t_ctx->priv_data;
    
    for(p=0; p < ctx->num_planes; p++){
        palette      = data->CPalette[p];
        palette_size = data->CPalette_size[p];

        for(r=0; r < ctx->height; r += stride_row){
            for(c=0; c < ctx->width; c += stride_col){
                P = ff_flif16_pixel_get(ctx, frame, p, r, c);
                if (P < 0 || P >= (int) palette_size)
                    P = 0;
                av_assert0(P < (int) palette_size);
                ff_flif16_pixel_set(ctx, frame, p, r, c, palette[P]);
            }
        }
    }
    return 1;
}

static void transform_channelcompact_close(FLIF16TransformContext *ctx){
    transform_priv_channelcompact *data = ctx->priv_data;
    av_free(data->CPalette);
    av_free(data->CPalette_inv);
}

/*
 * Bounds
 */

static int8_t transform_bounds_init(FLIF16TransformContext *ctx, 
                                        FLIF16RangesContext* src_ctx)
{
    transform_priv_bounds *data = ctx->priv_data;
    if(src_ctx->num_planes > 4)
        return 0;
    ff_flif16_chancecontext_init(&data->ctx_a);
    printf("context data:");
    for(int i = 0; i < sizeof(flif16_nz_int_chances) / sizeof(flif16_nz_int_chances[0]); ++i)
        printf("%d: %d ", i, data->ctx_a.data[i]);
    printf("\n");
    data->bounds = av_mallocz(src_ctx->num_planes*sizeof(*data->bounds));
    return 1;
}

static int8_t transform_bounds_read(FLIF16TransformContext* ctx,
                                    FLIF16Context* dec_ctx,
                                    FLIF16RangesContext* src_ctx)
{
    transform_priv_bounds *data = ctx->priv_data;
    FLIF16Ranges* ranges = flif16_ranges[src_ctx->r_no];
    int max;
    start:
    printf("ctx->i : %d\n", ctx->i);
    if(ctx->i < dec_ctx->num_planes){
        switch(ctx->segment){
            case 0:
                ranges->min(src_ctx, ctx->i);
                ranges->max(src_ctx, ctx->i);
                RAC_GET(&dec_ctx->rc, &data->ctx_a,
                        ranges->min(src_ctx, ctx->i), 
                        ranges->max(src_ctx, ctx->i),
                        &data->min, FLIF16_RAC_GNZ_INT);
                ctx->segment++;
        
            case 1:
                RAC_GET(&dec_ctx->rc, &data->ctx_a,
                        data->min, ranges->max(src_ctx, ctx->i),
                        &max, FLIF16_RAC_GNZ_INT);
                if(data->min > max)
                    return 0;
                if(data->min < ranges->min(src_ctx, ctx->i))
                    return 0;
                if(max > ranges->max(src_ctx, ctx->i))
                    return 0;
                data->bounds[ctx->i][0] = data->min;
                data->bounds[ctx->i][1] = max;
                ctx->i++;
                ctx->segment--;
                goto start;
        }
    }
    else{
        ctx->i = 0;
        ctx->segment = 0;
        goto end;
    }
    end:
        printf("[Bounds Result]\n");
        for(int i = 0; i < 3; ++i)
            printf("%d (%d, %d)\n", i, data->bounds[i][0], data->bounds[i][1]);
        return 1;

    need_more_data:
        printf("need more data<bounds>\n");
        return AVERROR(EAGAIN);
}

static FLIF16RangesContext* transform_bounds_meta(FLIF16Context *ctx,
                                                  FLIF16PixelData *frame,
                                                  uint32_t frame_count,
                                                  FLIF16TransformContext* t_ctx,
                                                  FLIF16RangesContext* src_ctx)
{
    FLIF16RangesContext* r_ctx;
    transform_priv_bounds* trans_data = t_ctx->priv_data;
    ranges_priv_static* data;
    ranges_priv_bounds* dataB;

    r_ctx = av_mallocz(sizeof(FLIF16RangesContext));
    if(!r_ctx)
        return NULL;
    r_ctx->num_planes = src_ctx->num_planes;
    
    if(flif16_ranges[src_ctx->r_no]->is_static){
        r_ctx->r_no = FLIF16_RANGES_STATIC;
        r_ctx->priv_data = av_mallocz(sizeof(ranges_priv_static));
        data = r_ctx->priv_data;
        data->bounds = trans_data->bounds;
    }
    else{
        r_ctx->r_no = FLIF16_RANGES_BOUNDS;
        r_ctx->priv_data = av_mallocz(sizeof(ranges_priv_bounds));
        dataB = r_ctx->priv_data;
        dataB->bounds = trans_data->bounds;
        dataB->r_ctx = src_ctx;
    }
    return r_ctx;
}

static void transform_bounds_close(FLIF16TransformContext *ctx){
    transform_priv_bounds *data = ctx->priv_data;
    av_free(data->bounds);
}

#define MAX_PALETTE_SIZE 30000

static int8_t transform_palette_init(FLIF16TransformContext *ctx,
                                      FLIF16RangesContext *src_ctx)
{
    transform_priv_palette *data = ctx->priv_data;

    if(  (src_ctx->num_planes < 3)  ||
         (ff_flif16_ranges_max(src_ctx, 0) == 0
       && ff_flif16_ranges_max(src_ctx, 2) == 0 
       && src_ctx->num_planes > 3
       && ff_flif16_ranges_min(src_ctx, 3) == 1
       && ff_flif16_ranges_max(src_ctx, 3) == 1)  || 
         (ff_flif16_ranges_min(src_ctx, 1) == ff_flif16_ranges_max(src_ctx, 1)
       && ff_flif16_ranges_min(src_ctx, 2) == ff_flif16_ranges_max(src_ctx, 2)))
        return 0;

    if(src_ctx->num_planes > 3)
        data->has_alpha = 1;
    else
        data->has_alpha = 0;

    ff_flif16_chancecontext_init(&data->ctx);
    ff_flif16_chancecontext_init(&data->ctxY);
    ff_flif16_chancecontext_init(&data->ctxI);
    ff_flif16_chancecontext_init(&data->ctxQ);
    data->p = 0;

    return 1;
}

static int8_t transform_palette_read(FLIF16TransformContext* ctx,
                                        FLIF16Context* dec_ctx,
                                        FLIF16RangesContext* src_ctx)
{
    transform_priv_palette *data = ctx->priv_data;
    switch (ctx->i)
    {
        case 0:
            printf("Palette Read :\n");
            RAC_GET(&dec_ctx->rc, &data->ctx, 1, MAX_PALETTE_SIZE,
                    &data->size, FLIF16_RAC_GNZ_INT);
            printf("size : %d\n", data->size);
            data->Palette = av_mallocz(data->size * sizeof(*data->Palette));
            ctx->i++;
        
        case 1:
            RAC_GET(&dec_ctx->rc, &data->ctx, 0, 1,
                    &data->sorted, FLIF16_RAC_GNZ_INT);
            printf("sorted : %d\n", data->sorted);
            if(data->sorted){
                ctx->i = 2;
                for(int i = 0; i < 3; i++){
                    data->min[i] = ff_flif16_ranges_min(src_ctx, i);
                    data->max[i] = ff_flif16_ranges_max(src_ctx, i);
                    data->Palette[0][i] = -1;
                }
                data->prev = data->Palette[0];
            }
            else{
                ctx->i = 5;
                goto unsorted;
            }
        
        loop:
        if(data->p < data->size){
        case 2:
            RAC_GET(&dec_ctx->rc, &data->ctxY, data->min[0], data->max[0],
                    &data->Y, FLIF16_RAC_GNZ_INT);
            printf("Y : %d\n", data->Y);
            data->pp[0] = data->Y;
            ff_flif16_ranges_minmax(src_ctx, 1, data->pp, &data->min[1], &data->max[1]);
            ctx->i++;

        case 3:
            RAC_GET(&dec_ctx->rc, &data->ctxI, 
                    data->prev[0] == data->Y ? data->prev[1] : data->min[1],
                    data->max[1],
                    &data->I, FLIF16_RAC_GNZ_INT);
            printf("I : %d\n", data->I);
            data->pp[1] = data->I;
            ff_flif16_ranges_minmax(src_ctx, 2, data->pp, &data->min[2], &data->max[2]);
            ctx->i++;

        case 4:
            RAC_GET(&dec_ctx->rc, &data->ctxQ, data->min[2], data->max[2],
                    &data->Q, FLIF16_RAC_GNZ_INT);
            printf("Q : %d\n", data->Q);
            data->Palette[data->p][0] = data->Y;
            data->Palette[data->p][1] = data->I;
            data->Palette[data->p][2] = data->Q;
            data->min[0] = data->Y;
            data->prev = data->Palette[data->p];
            data->p++;
            ctx->i = 2;
            goto loop;
        }
        else{
            ctx->i = 0;
            data->p = 0;
            goto end;
        }
        
        unsorted:
        if(data->p < data->size){
        case 5:
            ff_flif16_ranges_minmax(src_ctx, 0, data->pp, &data->min[0], &data->max[0]);
            RAC_GET(&dec_ctx->rc, &data->ctxY, data->min[0], data->max[0],
                    &data->Y, FLIF16_RAC_GNZ_INT);
            printf("Y : %d\n", data->Y);
            data->pp[0] = data->Y;
            ctx->i++;

        case 6:
            ff_flif16_ranges_minmax(src_ctx, 1, data->pp, &data->min[0], &data->max[0]);
            RAC_GET(&dec_ctx->rc, &data->ctxI, data->min[0], data->max[0],
                    &data->I, FLIF16_RAC_GNZ_INT);
            printf("I : %d\n", data->I);
            data->pp[1] = data->I;
            ctx->i++;

        case 7:
            ff_flif16_ranges_minmax(src_ctx, 2, data->pp, &data->min[0], &data->max[0]);
            RAC_GET(&dec_ctx->rc, &data->ctxQ, data->min[0], data->max[0],
                    &data->Q, FLIF16_RAC_GNZ_INT);
            printf("Q : %d\n", data->Q);
            data->Palette[data->p][0] = data->Y;
            data->Palette[data->p][1] = data->I;
            data->Palette[data->p][2] = data->Q;
            data->p++;
            ctx->i = 5;
            goto unsorted;
        }
        else{
            data->p = 0;
            ctx->i = 0;
            goto end;
        }
    
    }
    end:
        return 1;

    need_more_data:
        return AVERROR(EAGAIN);
}

static FLIF16RangesContext* transform_palette_meta(FLIF16Context *ctx,
                                                   FLIF16PixelData *frame,
                                                   uint32_t frame_count,
                                                   FLIF16TransformContext* t_ctx,
                                                   FLIF16RangesContext* src_ctx)
{
    FLIF16RangesContext *r_ctx = av_mallocz(sizeof(FLIF16RangesContext));
    transform_priv_palette *trans_data = t_ctx->priv_data;
    ranges_priv_palette *data = av_mallocz(sizeof(ranges_priv_palette));
    int i;
    // for(i = 0; i < frame_count; i++)
    //     frame[i].palette = 1;
    data->r_ctx = src_ctx;
    data->nb_colors = trans_data->size;
    r_ctx->r_no = FLIF16_RANGES_PALETTE;
    r_ctx->num_planes = src_ctx->num_planes;
    r_ctx->priv_data = data;
    return r_ctx;
}

static int8_t transform_palette_reverse(FLIF16Context *ctx,
                                        FLIF16TransformContext *t_ctx,
                                        FLIF16PixelData *frame,
                                        uint32_t stride_row,
                                        uint32_t stride_col)
{
    int r, c;
    int P;
    transform_priv_palette *data = t_ctx->priv_data;
    printf("Palette inverse: \n");
    for(r = 0; r < ctx->height; r += stride_row){
        for(c = 0; c < ctx->width; c += stride_col){
            P = ff_flif16_pixel_get(ctx, frame, 1, r, c);
            printf("%d ", P);
            if(P < 0 || P >= data->size)
                P = 0;
            av_assert0(P < data->size);
            av_assert0(P >= 0);
            ff_flif16_pixel_set(ctx, frame, 0, r, c, data->Palette[P][0]);
            ff_flif16_pixel_set(ctx, frame, 1, r, c, data->Palette[P][1]);
            ff_flif16_pixel_set(ctx, frame, 2, r, c, data->Palette[P][2]);
        }
        //frame->palette = 0;
    }
    printf("\n");
    return 1;
}

static void transform_palette_close(FLIF16TransformContext *ctx){
    transform_priv_palette *data = ctx->priv_data;
    av_free(data->Palette);
    av_free(data->prev);
}

static int8_t transform_palettealpha_init(FLIF16TransformContext *ctx, 
                                                FLIF16RangesContext* src_ctx)
{
    int p;
    transform_priv_palettealpha *data = ctx->priv_data;
    if(  src_ctx->num_planes < 4
      || ff_flif16_ranges_min(src_ctx, 3) == ff_flif16_ranges_max(src_ctx, 3))
        return 0;

    data->already_has_palette = 0;
    ff_flif16_chancecontext_init(&data->ctx);
    ff_flif16_chancecontext_init(&data->ctxY);
    ff_flif16_chancecontext_init(&data->ctxI);
    ff_flif16_chancecontext_init(&data->ctxQ);
    ff_flif16_chancecontext_init(&data->ctxA);
    data->p = 0;
    
    return 1;
}

static int8_t transform_palettealpha_read(FLIF16TransformContext * ctx,
                                             FLIF16Context *dec_ctx,
                                             FLIF16RangesContext* src_ctx)
{
    transform_priv_palettealpha *data = ctx->priv_data;
    switch (ctx->i)
    {
        case 0:
            printf("Palette Alpha Read :\n");
            RAC_GET(&dec_ctx->rc, &data->ctx, 1, MAX_PALETTE_SIZE,
                    &data->size, FLIF16_RAC_GNZ_INT);
            printf("size : %d\n", data->size);
            data->Palette = av_mallocz(data->size * sizeof(*data->Palette));
            ctx->i++;
        
        case 1:
            RAC_GET(&dec_ctx->rc, &data->ctx, 0, 1,
                    &data->sorted, FLIF16_RAC_GNZ_INT);
            printf("sorted : %d\n", data->sorted);
            if(data->sorted){
                ctx->i = 2;
                data->min[0] = ff_flif16_ranges_min(src_ctx, 3);
                data->max[0] = ff_flif16_ranges_max(src_ctx, 3);
                for(int i = 1; i < 4; i++){
                    data->min[i] = ff_flif16_ranges_min(src_ctx, i-1);
                    data->max[i] = ff_flif16_ranges_max(src_ctx, i-1);
                    data->Palette[0][i] = -1;
                }
                data->prev = data->Palette[0];
            }
            else{
                ctx->i = 6;
                goto unsorted;
            }
        
        loop:
        if(data->p < data->size){
        case 2:
            RAC_GET(&dec_ctx->rc, &data->ctxA, data->min[0], data->max[0],
                    &data->A, FLIF16_RAC_GNZ_INT);
            printf("A : %d\n", data->A);
            if(data->alpha_zero_special && data->A == 0){
                for(int i = 0; i < 4; i++)
                    data->Palette[data->p][i] = 0;
                data->p++;
                goto loop;
            }
            ctx->i++;

        case 3:
            RAC_GET(&dec_ctx->rc, &data->ctxY, 
                    data->prev[0] == data->A ? data->prev[1] : data->min[1],
                    data->max[1],
                    &data->Y, FLIF16_RAC_GNZ_INT);
            printf("Y : %d\n", data->Y);
            data->pp[0] = data->Y;
            ff_flif16_ranges_minmax(src_ctx, 1, data->pp, &data->min[2], &data->max[2]);
            ctx->i++;

        case 4:
            RAC_GET(&dec_ctx->rc, &data->ctxI, 
                    data->min[2], data->max[2],
                    &data->I, FLIF16_RAC_GNZ_INT);
            printf("I : %d\n", data->I);
            data->pp[1] = data->I;
            ff_flif16_ranges_minmax(src_ctx, 2, data->pp, &data->min[3], &data->max[3]);
            ctx->i++;

        case 5:
            RAC_GET(&dec_ctx->rc, &data->ctxQ, data->min[3], data->max[3],
                    &data->Q, FLIF16_RAC_GNZ_INT);
            printf("Q : %d\n", data->Q);
            data->Palette[data->p][0] = data->A;
            data->Palette[data->p][1] = data->Y;
            data->Palette[data->p][2] = data->I;
            data->Palette[data->p][3] = data->Q;
            data->min[0] = data->A;
            data->prev = data->Palette[data->p];
            data->p++;
            ctx->i = 2;
            goto loop;
        }
        else{
            ctx->i = 0;
            data->p = 0;
            goto end;
        }
        
        unsorted:
        if(data->p < data->size){
        case 6:
            RAC_GET(&dec_ctx->rc, &data->ctxA,
            ff_flif16_ranges_min(src_ctx, 3), ff_flif16_ranges_max(src_ctx, 3),
            &data->A, FLIF16_RAC_GNZ_INT);
            printf("A : %d\n", data->A);
            if(data->alpha_zero_special && data->A == 0){
                for(int i = 0; i < 4; i++)
                    data->Palette[data->p][i] = 0;
                data->p++;
                goto loop;
            }
            ctx->i++;
        
        case 7:
            ff_flif16_ranges_minmax(src_ctx, 0, data->pp, &data->min[0], &data->max[0]);
            RAC_GET(&dec_ctx->rc, &data->ctxY, data->min[0], data->max[0],
                    &data->Y, FLIF16_RAC_GNZ_INT);
            printf("Y : %d\n", data->Y);
            data->pp[0] = data->Y;
            ctx->i++;

        case 8:
            ff_flif16_ranges_minmax(src_ctx, 1, data->pp, &data->min[0], &data->max[0]);
            RAC_GET(&dec_ctx->rc, &data->ctxI, data->min[0], data->max[0],
                    &data->I, FLIF16_RAC_GNZ_INT);
            printf("I : %d\n", data->I);
            data->pp[1] = data->I;
            ctx->i++;

        case 9:
            ff_flif16_ranges_minmax(src_ctx, 2, data->pp, &data->min[0], &data->max[0]);
            RAC_GET(&dec_ctx->rc, &data->ctxQ, data->min[0], data->max[0],
                    &data->Q, FLIF16_RAC_GNZ_INT);
            printf("Q : %d\n", data->Q);
            data->Palette[data->p][0] = data->A;
            data->Palette[data->p][1] = data->Y;
            data->Palette[data->p][2] = data->I;
            data->Palette[data->p][3] = data->Q;
            data->p++;
            ctx->i = 6;
            goto unsorted;
        }
        else{
            data->p = 0;
            ctx->i = 0;
            goto end;
        }
    
    }
    end:
        return 1;

    need_more_data:
        return AVERROR(EAGAIN);
}

static void transform_palettealpha_configure(FLIF16TransformContext *ctx,
                                      const int setting)
{
    transform_priv_palettealpha *data = ctx->priv_data;
    data->alpha_zero_special = setting;
    if(setting > 0){
        data->ordered_palette = 1;
        data->max_palette_size = setting;
    }
    else{
        data->ordered_palette = 0;
        data->max_palette_size = -setting;
    }
}

static FLIF16RangesContext* transform_palettealpha_meta(FLIF16Context *ctx,
                                                        FLIF16PixelData *frame,
                                                         uint32_t frame_count,
                                                         FLIF16TransformContext* t_ctx,
                                                         FLIF16RangesContext* src_ctx)
{
    FLIF16RangesContext *r_ctx = av_mallocz(sizeof(FLIF16RangesContext));
    transform_priv_palettealpha *data = t_ctx->priv_data;
    ranges_priv_palette *priv_data = av_mallocz(sizeof(ranges_priv_permuteplanes));
    r_ctx->r_no = FLIF16_RANGES_PALETTEALPHA;
    r_ctx->num_planes = src_ctx->num_planes;
    priv_data->nb_colors = data->size;
    priv_data->r_ctx = src_ctx;
    r_ctx->priv_data = priv_data;

    // for(int i = 0; i < frame_count; i++)
    //     frame[i].palette = 1;

    return r_ctx;
}

static int8_t transform_palettealpha_reverse(FLIF16Context *ctx,
                                             FLIF16TransformContext *t_ctx,
                                             FLIF16PixelData* frame,
                                             uint32_t stride_row,
                                             uint32_t stride_col)
{
    int r, c;
    int P;
    transform_priv_palettealpha *data = t_ctx->priv_data;
    printf("Palette Alpha inverse: \n");
    for(r = 0; r < ctx->height; r += stride_row){
        for(c = 0; c < ctx->width; c += stride_col){
            P = ff_flif16_pixel_get(ctx, frame, 1, r, c);
            printf("%d ", P);
            av_assert0(P < data->size);
            ff_flif16_pixel_set(ctx, frame, 0, r, c, data->Palette[P][1]);
            ff_flif16_pixel_set(ctx, frame, 1, r, c, data->Palette[P][2]);
            ff_flif16_pixel_set(ctx, frame, 2, r, c, data->Palette[P][3]);
            ff_flif16_pixel_set(ctx, frame, 3, r, c, data->Palette[P][0]);
        }
        //frame->palette = 0;
    }
    printf("\n");
    return 1;
}

static void transform_palettealpha_close(FLIF16TransformContext *ctx){
    transform_priv_palettealpha *data = ctx->priv_data;
    av_free(data->Palette);
    av_free(data->prev);
}

/*
 * ColorBuckets
 */

static int8_t transform_colorbuckets_init(FLIF16TransformContext *ctx,
                                          FLIF16RangesContext *src_ctx)
{
    transform_priv_colorbuckets *data = ctx->priv_data;
    int length, temp;
    ColorBuckets *cb;
    data->cb = NULL;
    data->really_used = 0;
    if((src_ctx->num_planes < 3)
     ||
      (ff_flif16_ranges_min(src_ctx, 0) == 0
    && ff_flif16_ranges_max(src_ctx, 0) == 0 
    && ff_flif16_ranges_min(src_ctx, 2) == 0 
    && ff_flif16_ranges_max(src_ctx, 2) == 0)
     ||
      (ff_flif16_ranges_min(src_ctx, 0) == ff_flif16_ranges_max(src_ctx, 0)
    && ff_flif16_ranges_min(src_ctx, 1) == ff_flif16_ranges_max(src_ctx, 1)
    && ff_flif16_ranges_min(src_ctx, 2) == ff_flif16_ranges_max(src_ctx, 2))
     ||
      (ff_flif16_ranges_max(src_ctx, 0) - ff_flif16_ranges_min(src_ctx, 0) > 1023
     ||ff_flif16_ranges_max(src_ctx, 1) - ff_flif16_ranges_min(src_ctx, 1) > 1023
     ||ff_flif16_ranges_max(src_ctx, 2) - ff_flif16_ranges_min(src_ctx, 2) > 1023)
     ||
    (ff_flif16_ranges_min(src_ctx, 1) == ff_flif16_ranges_max(src_ctx, 1)))
        return 0;
    
    length = ((ff_flif16_ranges_max(src_ctx, 0) - cb->min0)/CB0b + 1);
    temp = ((ff_flif16_ranges_max(src_ctx, 1) - cb->min1)/CB1 + 1);
    
    cb = av_mallocz(sizeof(*cb));
    
    ff_init_bucket_default(&cb->bucket0);
    cb->min0 = ff_flif16_ranges_min(src_ctx, 0);
    cb->min1 = ff_flif16_ranges_min(src_ctx, 1);
    cb->bucket1 = av_mallocz(((ff_flif16_ranges_max(src_ctx, 0)
                                   - cb->min0)/CB0a + 1)
                                   * sizeof(*cb->bucket1));
    cb->bucket2 = av_mallocz(length * sizeof(*cb->bucket2));
    for(int i = 0; i < length; i++){
        cb->bucket2[i] = av_mallocz(temp * sizeof(*cb->bucket2[i]));
    }
    ff_init_bucket_default(&cb->bucket3);
    cb->ranges = src_ctx;
    data->cb = cb;
    
    return 1;
}

FLIF16Transform flif16_transform_channelcompact = {
    .priv_data_size = sizeof(transform_priv_channelcompact),
    .init           = &transform_channelcompact_init,
    .read           = &transform_channelcompact_read,
    .meta           = &transform_channelcompact_meta,
    .forward        = NULL,//&transform_channelcompact_forward,
    .reverse        = &transform_channelcompact_reverse,
    .close          = &transform_channelcompact_close
};

FLIF16Transform flif16_transform_ycocg = {
    .priv_data_size = sizeof(transform_priv_ycocg),
    .init           = &transform_ycocg_init,
    .read           = NULL,
    .meta           = &transform_ycocg_meta,
    .forward        = &transform_ycocg_forward,
    .reverse        = &transform_ycocg_reverse,
    .close          = &transform_ycocg_close
};

FLIF16Transform flif16_transform_permuteplanes = {
    .priv_data_size = sizeof(transform_priv_permuteplanes),
    .init           = &transform_permuteplanes_init,
    .read           = &transform_permuteplanes_read,
    .meta           = &transform_permuteplanes_meta,
    .forward        = &transform_permuteplanes_forward,
    .reverse        = &transform_permuteplanes_reverse,
    .close          = &transform_permuteplanes_close
};

FLIF16Transform flif16_transform_bounds = {
    .priv_data_size = sizeof(transform_priv_bounds),
    .init           = &transform_bounds_init,
    .read           = &transform_bounds_read,
    .meta           = &transform_bounds_meta,
    .forward        = NULL,
    .reverse        = NULL,
    .close          = &transform_bounds_close
};

FLIF16Transform flif16_transform_palette = {
    .priv_data_size = sizeof(transform_priv_palette),
    .init           = &transform_palette_init,
    .read           = &transform_palette_read,
    .meta           = &transform_palette_meta,
    //.forward
    .reverse        = &transform_palette_reverse,
    .close          = &transform_palette_close
};

FLIF16Transform flif16_transform_palettealpha = {
    .priv_data_size = sizeof(transform_priv_palettealpha),
    .init           = &transform_palettealpha_init,
    .read           = &transform_palettealpha_read,
    .meta           = &transform_palettealpha_meta,
    .configure      = &transform_palettealpha_configure,
    //.forward
    .reverse        = &transform_palettealpha_reverse,
    .close          = &transform_palettealpha_close
};

FLIF16Transform *flif16_transforms[13] = {
    &flif16_transform_channelcompact,
    &flif16_transform_ycocg,
    NULL, // FLIF16_TRANSFORM_RESERVED1,
    &flif16_transform_permuteplanes,
    &flif16_transform_bounds,
    &flif16_transform_palettealpha,
    &flif16_transform_palette,
    NULL, // FLIF16_TRANSFORM_COLORBUCKETS,
    NULL, // FLIF16_TRANSFORM_RESERVED2,
    NULL, // FLIF16_TRANSFORM_RESERVED3,
    NULL, // FLIF16_TRANSFORM_DUPLICATEFRAME,
    NULL, // FLIF16_TRANSFORM_FRAMESHAPE,
    NULL  // FLIF16_TRANSFORM_FRAMELOOKBACK
};

FLIF16TransformContext *ff_flif16_transform_init(int t_no, FLIF16RangesContext* r_ctx)
{
    FLIF16Transform *trans;
    FLIF16TransformContext *ctx;
    void *k = NULL;

    trans = flif16_transforms[t_no];
    if (!trans)
        return NULL;
    ctx = av_mallocz(sizeof(FLIF16TransformContext));
    if(!ctx)
        return NULL;
    if (trans->priv_data_size)
        k = av_mallocz(trans->priv_data_size);
    ctx->t_no      = t_no;
    ctx->priv_data = k;
    ctx->segment   = 0;
    ctx->i         = 0;

    if (trans->init)
        if(!trans->init(ctx, r_ctx))
            return NULL;
    
    return ctx;
}

int8_t ff_flif16_transform_read(FLIF16TransformContext *ctx,
                                 FLIF16Context *dec_ctx,
                                 FLIF16RangesContext* r_ctx)
{
    FLIF16Transform *trans = flif16_transforms[ctx->t_no];
    if(trans->read)
        return trans->read(ctx, dec_ctx, r_ctx);
    else
        return 1;
}

FLIF16RangesContext *ff_flif16_transform_meta(FLIF16Context *ctx,
                                              FLIF16PixelData *frames,
                                              uint32_t frames_count,
                                              FLIF16TransformContext *t_ctx,
                                              FLIF16RangesContext *r_ctx)
{
    FLIF16Transform *trans;
    trans = flif16_transforms[t_ctx->t_no];
    if(trans->meta)
        return trans->meta(ctx, frames, frames_count, t_ctx, r_ctx);
    else
        return r_ctx;
}

void ff_flif16_transform_configure(FLIF16TransformContext *ctx, const int setting){
    FLIF16Transform *trans = flif16_transforms[ctx->t_no];
    if(trans->configure)
        trans->configure(ctx, setting);
}

int8_t ff_flif16_transform_reverse( FLIF16Context *ctx,
                                    FLIF16TransformContext *t_ctx,
                                    FLIF16PixelData *frame,
                                    uint8_t stride_row,
                                    uint8_t stride_col)
{
    FLIF16Transform* trans = flif16_transforms[t_ctx->t_no];
    if(trans->reverse != NULL)
        return trans->reverse(ctx, t_ctx, frame, stride_row, stride_col);
    else
        return 1;
}                                    

void ff_flif16_transforms_close(FLIF16TransformContext* ctx){
    FLIF16Transform* trans = flif16_transforms[ctx->t_no];
    if(trans->priv_data_size){
        trans->close(ctx);
        av_free(ctx->priv_data);
    }
    av_freep(&ctx);
}
