/*
 * SPDX-License-Identifier: MIT
 *
 * Flash-mapped external font loader.
 *
 * The 6 MB "font" data partition stores one or more POD face blobs:
 *
 *   pod_font_header_t
 *   pod_font_entry_t[count]
 *   pod_font_face_t
 *   pod_font_range_t[range_count]
 *   pod_font_glyph_t[glyph_count]
 *   glyph bitmap blob
 *
 * Glyph bitmaps are never copied into RAM.  LVGL expands individual glyphs
 * into its normal A8 draw buffer while rendering, exactly like a built-in
 * font, so the full CJK sets stay in flash and the internal heap only holds a
 * tiny lv_font_t per face.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POD_FONT_PARTITION_LABEL "font"
#define POD_FONT_MAGIC           0x544E4F46u /* "FONT" */
#define POD_FONT_VERSION         2

#define POD_FONT_FACE_MAGIC      0x45434146u /* "FACE" */
#define POD_FONT_FACE_VERSION    1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t header_size;
    uint32_t blob_size;
    uint32_t flags;
} pod_font_header_t;

typedef struct {
    uint16_t pixel_size;
    uint16_t bpp;
    uint32_t offset; /* absolute partition offset */
    uint32_t length;
} pod_font_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bpp;
    uint16_t pixel_size;
    uint16_t flags;
    uint16_t range_count;
    uint16_t reserved;
    int16_t line_height;
    int16_t base_line;
    uint32_t glyph_count;
    uint32_t ranges_offset;
    uint32_t glyphs_offset;
    uint32_t bitmap_offset;
    uint32_t bitmap_size;
} pod_font_face_t;

typedef struct {
    uint32_t start;       /* first code point, inclusive */
    uint32_t end;         /* last code point, inclusive */
    uint32_t glyph_start; /* glyph index of `start` */
} pod_font_range_t;

typedef struct {
    uint32_t bitmap_offset; /* relative to the face bitmap blob */
    uint16_t adv_w;         /* advance width in 12.4 fixed point */
    uint8_t box_w;
    uint8_t box_h;
    int8_t ofs_x;
    int8_t ofs_y;
    uint16_t reserved;
} pod_font_glyph_t;

_Static_assert(sizeof(pod_font_header_t) == 20, "font header layout");
_Static_assert(sizeof(pod_font_entry_t) == 12, "font entry layout");
_Static_assert(sizeof(pod_font_face_t) == 40, "font face layout");
_Static_assert(sizeof(pod_font_range_t) == 12, "font range layout");
_Static_assert(sizeof(pod_font_glyph_t) == 12, "font glyph layout");

esp_err_t pod_font_init(void);
bool pod_font_partition_ready(void);
size_t pod_font_partition_size(void);

/* Return a cached lv_font_t for a face, or NULL when the partition or face is
 * missing.  The returned font has no fallback set; the UI owns that policy. */
lv_font_t *pod_font_load_font(uint16_t pixel_size);

/* Read raw bytes from the font partition. */
esp_err_t pod_font_read(size_t offset, void *dst, size_t len);

#ifdef __cplusplus
}
#endif
