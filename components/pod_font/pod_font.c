/*
 * SPDX-License-Identifier: MIT
 */

#include "pod_font.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "pod_font";

static const esp_partition_t *s_partition;
static esp_partition_mmap_handle_t s_map_handle;
static const void *s_map;
static pod_font_header_t s_header;
static bool s_header_valid;

#define POD_FONT_MAX_FACES 8

static lv_font_t s_fonts[POD_FONT_MAX_FACES];
static uint16_t s_font_sizes[POD_FONT_MAX_FACES];
static uint8_t s_font_count;

/* -------------------------------------------------------------------------- */
/* Layout helpers                                                             */
/* -------------------------------------------------------------------------- */

static const uint8_t *map_base(void)
{
    return (const uint8_t *)s_map;
}

static const pod_font_entry_t *entry_at(size_t index)
{
    return (const pod_font_entry_t *)(map_base() + sizeof(pod_font_header_t) +
                                      index * sizeof(pod_font_entry_t));
}

static const pod_font_face_t *face_at(const pod_font_entry_t *entry)
{
    return (const pod_font_face_t *)(map_base() + entry->offset);
}

static const pod_font_range_t *ranges_at(const pod_font_face_t *face)
{
    return (const pod_font_range_t *)((const uint8_t *)face + face->ranges_offset);
}

static const pod_font_glyph_t *glyphs_at(const pod_font_face_t *face)
{
    return (const pod_font_glyph_t *)((const uint8_t *)face + face->glyphs_offset);
}

static const uint8_t *bitmap_at(const pod_font_face_t *face)
{
    return (const uint8_t *)face + face->bitmap_offset;
}

static bool face_layout_is_valid(const pod_font_entry_t *entry,
                                 const pod_font_face_t *face)
{
    if (face->magic != POD_FONT_FACE_MAGIC ||
        face->version != POD_FONT_FACE_VERSION) {
        return false;
    }
    if (face->pixel_size != entry->pixel_size || face->bpp != entry->bpp) {
        return false;
    }
    if (face->bpp != 1 && face->bpp != 2 && face->bpp != 3 &&
        face->bpp != 4 && face->bpp != 8) {
        return false;
    }
    if (face->line_height <= 0 || face->base_line < 0 ||
        face->base_line > face->line_height) {
        return false;
    }
    if (face->range_count == 0 || face->range_count > 256 ||
        face->glyph_count == 0 || face->glyph_count > 65535) {
        return false;
    }

    const uint64_t ranges_end = (uint64_t)face->ranges_offset +
                                (uint64_t)face->range_count * sizeof(pod_font_range_t);
    const uint64_t glyphs_end = (uint64_t)face->glyphs_offset +
                                (uint64_t)face->glyph_count * sizeof(pod_font_glyph_t);
    const uint64_t bitmap_end = (uint64_t)face->bitmap_offset + face->bitmap_size;

    if (face->ranges_offset < sizeof(*face) ||
        face->glyphs_offset < ranges_end ||
        face->bitmap_offset < glyphs_end ||
        bitmap_end > entry->length) {
        return false;
    }
    return true;
}

static bool glyph_lookup(const pod_font_face_t *face, uint32_t code,
                         uint32_t *glyph_index)
{
    const pod_font_range_t *ranges = ranges_at(face);
    uint32_t lo = 0;
    uint32_t hi = face->range_count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const pod_font_range_t *range = &ranges[mid];
        if (code < range->start) {
            hi = mid;
        } else if (code > range->end) {
            lo = mid + 1;
        } else {
            uint32_t index = range->glyph_start + (code - range->start);
            if (index >= face->glyph_count) return false;
            if (glyph_index) *glyph_index = index;
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* LVGL callbacks                                                             */
/* -------------------------------------------------------------------------- */

static bool font_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                               uint32_t letter, uint32_t letter_next)
{
    (void)letter_next;
    const pod_font_face_t *face = font->dsc;
    uint32_t index;
    if (!face || !glyph_lookup(face, letter, &index)) {
        return false;
    }

    const pod_font_glyph_t *glyph = &glyphs_at(face)[index];
    /* LVGL's public glyph descriptor stores advance in whole pixels; the
     * packed face stores it in 12.4 fixed point (like lv_font_fmt_txt). */
    dsc->adv_w = (uint16_t)((glyph->adv_w + 8) >> 4);
    dsc->box_w = glyph->box_w;
    dsc->box_h = glyph->box_h;
    dsc->ofs_x = glyph->ofs_x;
    dsc->ofs_y = glyph->ofs_y;
    dsc->stride = 0;
    switch (face->bpp) {
    case 1: dsc->format = LV_FONT_GLYPH_FORMAT_A1; break;
    case 2: dsc->format = LV_FONT_GLYPH_FORMAT_A2; break;
    case 3: dsc->format = LV_FONT_GLYPH_FORMAT_A3; break;
    case 4: dsc->format = LV_FONT_GLYPH_FORMAT_A4; break;
    default: dsc->format = LV_FONT_GLYPH_FORMAT_A8; break;
    }
    dsc->gid.index = index;
    dsc->is_placeholder = 0;
    return true;
}

static void expand_a4(const uint8_t *src, uint8_t *dst, uint32_t stride,
                      uint32_t w, uint32_t h)
{
    const uint32_t row_bytes = (w + 1) / 2;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + y * row_bytes;
        uint8_t *out = dst + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t byte = row[x >> 1];
            uint8_t v = (x & 1) ? (byte & 0x0F) : (byte >> 4);
            out[x] = (uint8_t)(v * 17);
        }
    }
}

static void expand_a2(const uint8_t *src, uint8_t *dst, uint32_t stride,
                      uint32_t w, uint32_t h)
{
    const uint32_t row_bytes = (w + 3) / 4;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + y * row_bytes;
        uint8_t *out = dst + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t byte = row[x >> 2];
            uint8_t v = (byte >> (6 - 2 * (x & 3))) & 0x03;
            out[x] = (uint8_t)(v * 85);
        }
    }
}

static void expand_generic(const uint8_t *src, uint8_t *dst, uint32_t stride,
                           uint32_t w, uint32_t h, uint32_t bpp)
{
    const uint32_t mask = (1u << bpp) - 1u;
    const uint32_t row_bytes = (w * bpp + 7) / 8;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + y * row_bytes;
        const uint8_t *p = row;
        uint8_t *out = dst + y * stride;
        uint32_t acc = 0;
        uint32_t bits = 0;
        for (uint32_t x = 0; x < w; x++) {
            while (bits < bpp) {
                acc = (acc << 8) | *p++;
                bits += 8;
            }
            bits -= bpp;
            uint32_t v = (acc >> bits) & mask;
            out[x] = (uint8_t)((v * 255u + mask / 2u) / mask);
        }
    }
}

static const void *font_get_glyph_bitmap(lv_font_glyph_dsc_t *dsc,
                                         lv_draw_buf_t *draw_buf)
{
    const lv_font_t *font = dsc->resolved_font;
    const pod_font_face_t *face = font->dsc;
    if (!face || dsc->gid.index >= face->glyph_count) {
        return NULL;
    }

    const pod_font_glyph_t *glyph = &glyphs_at(face)[dsc->gid.index];
    const uint8_t *src = bitmap_at(face) + glyph->bitmap_offset;

    if (dsc->req_raw_bitmap) {
        return src;
    }
    if (!draw_buf || glyph->box_w == 0 || glyph->box_h == 0) {
        return NULL;
    }

    uint32_t stride = draw_buf->header.stride;
    if (stride == 0) stride = glyph->box_w;
    uint8_t *dst = draw_buf->data;

    if (face->bpp == 4) {
        expand_a4(src, dst, stride, glyph->box_w, glyph->box_h);
    } else if (face->bpp == 2) {
        expand_a2(src, dst, stride, glyph->box_w, glyph->box_h);
    } else {
        expand_generic(src, dst, stride, glyph->box_w, glyph->box_h, face->bpp);
    }

    lv_draw_buf_flush_cache(draw_buf, NULL);
    return draw_buf;
}

static void font_self_check(lv_font_t *font, uint16_t pixel_size)
{
    static const uint32_t probes[] = {'A', 0x4E2D}; /* A, 中 */
    static const char *names[] = {"ASCII", "CJK"};
    lv_draw_buf_t *buf = lv_draw_buf_create(64, 64, LV_COLOR_FORMAT_A8, 0);
    if (!buf) return;

    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, probes[i], 0) ||
            g.box_w == 0 || g.box_h == 0) {
            ESP_LOGE(TAG, "%uppx %s probe missing/invalid", pixel_size, names[i]);
            continue;
        }
        const void *bitmap = lv_font_get_glyph_bitmap(&g, buf);
        uint32_t ink = 0;
        if (bitmap && g.box_w <= 64 && g.box_h <= 64) {
            const uint8_t *p = bitmap;
            uint32_t stride = buf->header.stride;
            for (uint32_t y = 0; y < g.box_h; y++) {
                for (uint32_t x = 0; x < g.box_w; x++) {
                    ink += p[y * stride + x];
                }
            }
        }
        if (ink == 0) {
            ESP_LOGE(TAG, "%uppx %s probe has empty bitmap", pixel_size, names[i]);
        } else {
            ESP_LOGI(TAG, "%uppx %s probe: box=%ux%u adv=%u ink=%" PRIu32,
                     pixel_size, names[i], g.box_w, g.box_h,
                     (unsigned)g.adv_w, ink);
        }
    }
    lv_draw_buf_destroy(buf);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t pod_font_init(void)
{
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           (esp_partition_subtype_t)0x40,
                                           POD_FONT_PARTITION_LABEL);
    if (!s_partition) {
        ESP_LOGW(TAG, "font partition '%s' not found", POD_FONT_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_partition_mmap(s_partition, 0, s_partition->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &s_map, &s_map_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "font mmap failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_partition->size < sizeof(s_header)) {
        ESP_LOGW(TAG, "font partition too small");
        return ESP_OK;
    }

    memcpy(&s_header, s_map, sizeof(s_header));
    if (s_header.magic != POD_FONT_MAGIC || s_header.version != POD_FONT_VERSION) {
        ESP_LOGW(TAG, "font partition present (%" PRIu32 " bytes) but not provisioned",
                 s_partition->size);
        return ESP_OK;
    }

    const uint64_t entries_end = (uint64_t)sizeof(s_header) +
                                 (uint64_t)s_header.count * sizeof(pod_font_entry_t);
    if (s_header.header_size < entries_end ||
        s_header.header_size > s_partition->size ||
        s_header.blob_size > s_partition->size ||
        s_header.count == 0 || s_header.count > 16) {
        ESP_LOGW(TAG, "font partition header invalid");
        return ESP_OK;
    }

    for (uint16_t i = 0; i < s_header.count; i++) {
        const pod_font_entry_t *entry = entry_at(i);
        if (entry->length == 0 ||
            (uint64_t)entry->offset + entry->length > s_partition->size) {
            ESP_LOGW(TAG, "font entry %u out of range", i);
            return ESP_OK;
        }
        const pod_font_face_t *face = face_at(entry);
        if (!face_layout_is_valid(entry, face)) {
            ESP_LOGW(TAG, "font entry %u layout invalid", i);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "face %uppx %ubpp: %" PRIu32 " glyphs, %u ranges, %" PRIu32
                 " bitmap bytes", entry->pixel_size, entry->bpp,
                 face->glyph_count, face->range_count, face->bitmap_size);
    }

    s_header_valid = true;
    ESP_LOGI(TAG, "font partition ready: %" PRIu32 " bytes, %u faces, %" PRIu32
             " bytes used", s_partition->size, s_header.count, s_header.blob_size);
    return ESP_OK;
}

bool pod_font_partition_ready(void)
{
    return s_partition != NULL;
}

size_t pod_font_partition_size(void)
{
    return s_partition ? s_partition->size : 0;
}

lv_font_t *pod_font_load_font(uint16_t pixel_size)
{
    if (!s_header_valid || !s_map) {
        return NULL;
    }

    for (uint8_t i = 0; i < s_font_count; i++) {
        if (s_font_sizes[i] == pixel_size) {
            return &s_fonts[i];
        }
    }
    if (s_font_count >= POD_FONT_MAX_FACES) {
        return NULL;
    }

    for (uint16_t i = 0; i < s_header.count; i++) {
        const pod_font_entry_t *entry = entry_at(i);
        if (entry->pixel_size != pixel_size) continue;
        const pod_font_face_t *face = face_at(entry);
        if (!face_layout_is_valid(entry, face)) return NULL;

        lv_font_t *font = &s_fonts[s_font_count];
        memset(font, 0, sizeof(*font));
        font->get_glyph_dsc = font_get_glyph_dsc;
        font->get_glyph_bitmap = font_get_glyph_bitmap;
        font->release_glyph = NULL;
        font->line_height = face->line_height;
        font->base_line = face->base_line;
        font->subpx = LV_FONT_SUBPX_NONE;
        font->kerning = LV_FONT_KERNING_NONE;
        font->static_bitmap = 0;
        font->underline_position = -2;
        font->underline_thickness = 1;
        font->dsc = face;
        font->fallback = NULL;
        font->user_data = NULL;

        s_font_sizes[s_font_count] = pixel_size;
        s_font_count++;

        font_self_check(font, pixel_size);
        ESP_LOGI(TAG, "loaded %uppx external font: %" PRIu32 " glyphs, "
                 "line=%d base=%d, bitmap=%" PRIu32 " B (flash-mapped)",
                 pixel_size, face->glyph_count, face->line_height,
                 face->base_line, face->bitmap_size);
        return font;
    }

    ESP_LOGW(TAG, "no %uppx face in font partition", pixel_size);
    return NULL;
}

esp_err_t pod_font_read(size_t offset, void *dst, size_t len)
{
    if (!s_partition) return ESP_ERR_INVALID_STATE;
    if (!dst || offset + len > s_partition->size) return ESP_ERR_INVALID_ARG;
    return esp_partition_read(s_partition, offset, dst, len);
}
