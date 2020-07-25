/*
 * FLIF16 parser
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
  * FLIF16 parser
  */

#include "flif16.h"
#include "parser.h"
#include "libavutil/avassert.h"
#include "libavutil/bswap.h"

#include <stdio.h> //remove
#include <stdint.h>
#include <stdlib.h>

typedef enum FLIF16ParseStates {
    FLIF16_HEADER = 1,
    FLIF16_METADATA,
    FLIF16_BITSTREAM
} FLIF16ParseStates;

typedef struct FLIF16ParseContext {
    ParseContext pc;
    int state;          ///< The section of the file the parser is in currently.
    unsigned int index; ///< An index based on the current state. 
    uint8_t animated;   ///< Is image animated or not
    uint8_t varint;     ///< Number of varints to process in sequence
    uint32_t width;
    uint32_t height;
    uint32_t frames;
    uint32_t meta;      ///< Size of a meta chunk
    uint32_t count;
} FLIF16ParseContext;


// TODO revamp this function
static int flif16_find_frame(FLIF16ParseContext *f, const uint8_t *buf,
                             int buf_size)
{
    int next = END_NOT_FOUND;
    int index;

    for (index = 0; index < buf_size; index++) {
        if (!f->state) {
            if (!memcmp(flif16_header, buf + index, 4))
                f->state = FLIF16_HEADER;
            ++f->index;
        } else if (f->state == FLIF16_HEADER) {
            if (f->index == 3 + 1) {
                // See whether image is animated or not
                f->animated = (((buf[index] >> 4) > 4)?1:0);
            } else if (f->index == (3 + 1 + 1)) {
                // Start - 1 of the first varint
                f->varint = 1;
            } else if (f->varint) {
                // Count varint
                if (f->count == 5)
                        return AVERROR(ENOMEM);

                switch (f->varint) {
                    case 1:
                        VARINT_APPEND(f->width, buf[index]);
                        break;
                    
                    case 2:
                        VARINT_APPEND(f->height, buf[index]);
                        break;
                    
                    case 3:
                        VARINT_APPEND(f->frames, buf[index]);
                        break;
                }
                if (buf[index] < 128) {
                    if (f->varint < (2 + f->animated)) {
                        switch (f->varint) {
                            case 1: f->width++;  break;
                            case 2: f->height++; break;
                        }
                        f->varint++;
                        f->count = 0;
                    } else {
                        if (f->varint == 2)
                            f->height++;
                        if (f->animated)
                            f->frames += 2;
                        else
                            f->frames = 1;
                        f->state = FLIF16_METADATA;
                        f->varint = 0;
                        f->index = 0;
                        f->count = 0;
                        continue;
                    }
                } else {
                    f->count++;
                }
            }
            f->index++;
        } else if (f->state == FLIF16_METADATA) {
            if (f->index == 0) {
                // Identifier for the bitstream chunk is a null byte.
                if (buf[index] == 0) {
                    f->state = FLIF16_BITSTREAM;
                    return buf_size;
                }
            } else if (f->index < 3) {
                // nop
            } else if (f->index == 3) {
                // Handle the size varint
                f->varint = 1;
            } else if (f->varint) {
                if (f->count == 9)
                    return AVERROR(ENOMEM);
                if (buf[index] < 128) {
                    f->varint = 0;
                    f->count = 0;
                }
                VARINT_APPEND(f->meta, buf[index]);
                f->count++;
            } else if (f->meta > 1) {
                // increment varint until equal to size
                f->meta--;
            } else {
                f->meta = 0;
                f->index = 0;
                continue;
            }
            f->index++;
        } else if (f->state == FLIF16_BITSTREAM) {
            /* Since we cannot find the end of the bitstream without any
             * processing, we will simply return each read chunk as a packet
             * to the decoder.
             */
            printf("<Bitstream chunk size %dd>\n", buf_size);
            return buf_size;
        }
    }
    printf("End not found\n");
    return next;
}

static int flif16_parse(AVCodecParserContext *s, AVCodecContext *avctx,
                        const uint8_t **poutbuf, int *poutbuf_size,
                        const uint8_t *buf, int buf_size)
{
    FLIF16ParseContext *fpc = s->priv_data;
    int next;
    
    next = flif16_find_frame(fpc, buf, buf_size);
    
    if (ff_combine_frame(&fpc->pc, next, &buf, &buf_size) < 0) {
        *poutbuf      = NULL;
        *poutbuf_size = 0;
        return buf_size;
    }
    printf("Width:%u\nHeight:%u\nFrames:%u\nEnd:%d\n", 
           fpc->width, fpc->height, fpc->frames, buf_size);
    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return next;
}

AVCodecParser ff_flif16_parser = {
    .codec_ids      = { AV_CODEC_ID_FLIF16 },
    .priv_data_size = sizeof(FLIF16ParseContext),
    .parser_parse   = flif16_parse,
    .parser_close   = ff_parse_close,
};

