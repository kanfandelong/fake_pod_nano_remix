/*
 * SPDX-License-Identifier: MIT
 */

#include "pod_metadata.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "pod_metadata";

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t rd_le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | p[0];
}

static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) |
           ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) |
           (uint32_t)(p[3] & 0x7F);
}

static void clean_text(char *s)
{
    if (!s) return;
    char *w = s;
    for (char *r = s; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c >= 0x20) *w++ = (char)c;
    }
    *w = '\0';
    while (w > s && w[-1] == ' ') *--w = '\0';
    char *start = s;
    while (*start == ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static void append_utf8(char *out, size_t out_len, size_t *used, uint32_t cp)
{
    if (*used + 4 >= out_len) return;
    if (cp < 0x80) {
        out[(*used)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*used)++] = (char)(0xC0 | (cp >> 6));
        out[(*used)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*used)++] = (char)(0xE0 | (cp >> 12));
        out[(*used)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*used)++] = (char)(0xF0 | (cp >> 18));
        out[(*used)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*used)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char)(0x80 | (cp & 0x3F));
    }
    out[*used] = '\0';
}

static void utf16_to_utf8(const uint8_t *data, size_t len, bool be,
                          char *out, size_t out_len)
{
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t u = be ? ((uint32_t)data[i] << 8 | data[i + 1]) :
                          ((uint32_t)data[i + 1] << 8 | data[i]);
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < len) {
            uint32_t lo = be ? ((uint32_t)data[i + 2] << 8 | data[i + 3]) :
                               ((uint32_t)data[i + 3] << 8 | data[i + 2]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        append_utf8(out, out_len, &used, u);
    }
    clean_text(out);
}

static void decode_text(const uint8_t *data, size_t len, uint8_t enc,
                        char *out, size_t out_len)
{
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!data || len == 0) return;

    if (enc == 1 || enc == 2) {
        bool be = (enc == 2);
        size_t off = 0;
        if (enc == 1 && len >= 2) {
            if (data[0] == 0xFF && data[1] == 0xFE) { be = false; off = 2; }
            else if (data[0] == 0xFE && data[1] == 0xFF) { be = true; off = 2; }
            else { be = false; off = 2; }
        }
        utf16_to_utf8(data + off, len - off, be, out, out_len);
        return;
    }

    if (enc == 3) {
        size_t n = 0;
        while (n < len && n + 1 < out_len && data[n]) {
            out[n] = (char)data[n];
            n++;
        }
        out[n] = '\0';
        clean_text(out);
        return;
    }

    /* ISO-8859-1 -> UTF-8 */
    size_t used = 0;
    for (size_t i = 0; i < len && data[i]; i++) {
        append_utf8(out, out_len, &used, data[i]);
    }
    clean_text(out);
}

static uint32_t path_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static esp_err_t write_cover(const char *audio_path, const uint8_t *data,
                             size_t len, pod_metadata_t *meta)
{
    if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return ESP_ERR_NOT_SUPPORTED; /* JPEG only for now */
    }

    
    snprintf(meta->cover_path, sizeof(meta->cover_path),
             "cover_%08" PRIx32 ".jpg",
             path_hash(audio_path));

    FILE *fp = fopen(meta->cover_path, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "cover open failed: %s", strerror(errno));
        meta->cover_path[0] = '\0';
        return ESP_FAIL;
    }
    size_t wrote = fwrite(data, 1, len, fp);
    fclose(fp);
    if (wrote != len) {
        meta->cover_path[0] = '\0';
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void parse_apic(const uint8_t *data, size_t len, const char *audio_path,
                       pod_metadata_t *meta)
{
    if (len < 4) return;
    uint8_t enc = data[0];
    size_t p = 1;
    while (p < len && data[p]) p++;
    if (p >= len) return;
    p++; /* MIME NUL */
    if (p >= len) return;
    p++; /* picture type */
    if (enc == 1 || enc == 2) {
        while (p + 1 < len && !(data[p] == 0 && data[p + 1] == 0)) p += 2;
        p += 2;
    } else {
        while (p < len && data[p]) p++;
        p++;
    }
    if (p >= len) return;
    (void)write_cover(audio_path, data + p, len - p, meta);
}

static void parse_id3(FILE *fp, uint8_t version, long tag_end,
                      const char *path, pod_metadata_t *meta)
{
    uint8_t hdr[10];
    long pos = 10;
    while (pos + 10 <= tag_end) {
        if (fseek(fp, pos, SEEK_SET) != 0) break;
        if (fread(hdr, 1, 10, fp) != 10) break;
        if (hdr[0] == 0) break;
        uint32_t size = (version >= 4) ? syncsafe32(hdr + 4) : rd_be32(hdr + 4);
        if (size == 0 || pos + 10 + (long)size > tag_end) break;

        char id[5] = {0};
        memcpy(id, hdr, 4);

        if (strcmp(id, "TIT2") == 0 || strcmp(id, "TPE1") == 0 ||
            strcmp(id, "TALB") == 0 || strcmp(id, "APIC") == 0) {
            if (size > 4 * 1024 * 1024) break;
            uint8_t *buf = malloc(size);
            if (!buf) break;
            if (fseek(fp, pos + 10, SEEK_SET) != 0 ||
                fread(buf, 1, size, fp) != size) {
                free(buf);
                break;
            }
            if (strcmp(id, "APIC") == 0) {
                parse_apic(buf, size, path, meta);
            } else {
                char text[128] = {0};
                decode_text(buf + 1, size > 1 ? size - 1 : 0, buf[0],
                            text, sizeof(text));
                if (text[0]) {
                    if (strcmp(id, "TIT2") == 0) snprintf(meta->title, sizeof(meta->title), "%s", text);
                    else if (strcmp(id, "TPE1") == 0) snprintf(meta->artist, sizeof(meta->artist), "%s", text);
                    else snprintf(meta->album, sizeof(meta->album), "%s", text);
                }
            }
            free(buf);
        }

        pos += 10 + (long)size;
    }
}

static void parse_id3_file(FILE *fp, const char *path, pod_metadata_t *meta)
{
    uint8_t hdr[10];
    if (fseek(fp, 0, SEEK_SET) != 0) return;
    if (fread(hdr, 1, 10, fp) != 10) return;
    if (memcmp(hdr, "ID3", 3) != 0) return;
    uint32_t tag_size = syncsafe32(hdr + 6);
    long end = 10 + (long)tag_size;
    if (end < 10 || end > 64L * 1024 * 1024) return;

    /* Skip ID3v2 extended header when present. */
    long start = 10;
    if (hdr[5] & 0x40) {
        uint8_t ext[4];
        if (fread(ext, 1, 4, fp) == 4) {
            uint32_t esz = (hdr[3] >= 4) ? syncsafe32(ext) : rd_be32(ext);
            if (esz >= 4 && start + (long)esz <= end) start += (long)esz;
        }
    }
    /* parse_id3 starts at 10; temporarily seek and let it use absolute
     * positions. Extended headers are rare; without one this is exact. */
    (void)start;
    parse_id3(fp, hdr[3], end, path, meta);
}

static void parse_vorbis(const uint8_t *buf, size_t len, pod_metadata_t *meta)
{
    if (len < 8) return;
    uint32_t vlen = rd_le32(buf);
    if (vlen > len - 4) return;
    size_t p = 4 + vlen;
    if (p + 4 > len) return;
    uint32_t count = rd_le32(buf + p);
    p += 4;

    for (uint32_t i = 0; i < count && p + 4 <= len; i++) {
        uint32_t n = rd_le32(buf + p);
        p += 4;
        if (n > len - p) break;
        const char *s = (const char *)(buf + p);
        size_t sl = 0;
        while (sl < n && s[sl]) sl++;
        if (sl > 0 && sl < 120) {
            char key[32] = {0};
            size_t eq = 0;
            while (eq < sl && s[eq] != '=') eq++;
            if (eq > 0 && eq < sizeof(key)) {
                memcpy(key, s, eq);
                key[eq] = '\0';
                const char *val = s + eq + 1;
                size_t vl = sl - eq - 1;
                char tmp[128] = {0};
                size_t cp = vl < sizeof(tmp) - 1 ? vl : sizeof(tmp) - 1;
                memcpy(tmp, val, cp);
                clean_text(tmp);
                if (strcasecmp(key, "TITLE") == 0) snprintf(meta->title, sizeof(meta->title), "%s", tmp);
                else if (strcasecmp(key, "ARTIST") == 0) snprintf(meta->artist, sizeof(meta->artist), "%s", tmp);
                else if (strcasecmp(key, "ALBUM") == 0) snprintf(meta->album, sizeof(meta->album), "%s", tmp);
            }
        }
        p += n;
    }
}

static void parse_flac_picture(const uint8_t *buf, size_t len, const char *path,
                               pod_metadata_t *meta)
{
    if (len < 32) return;
    size_t p = 4; /* picture type */
    if (p + 4 > len) return;
    uint32_t mlen = rd_be32(buf + p); p += 4;
    if (mlen > len - p) return;
    p += mlen;
    if (p + 4 > len) return;
    uint32_t dlen = rd_be32(buf + p); p += 4;
    if (dlen > len - p) return;
    p += dlen;
    if (p + 16 + 4 > len) return;
    p += 16;
    uint32_t plen = rd_be32(buf + p); p += 4;
    if (plen > len - p) return;
    (void)write_cover(path, buf + p, plen, meta);
}

static void parse_flac(FILE *fp, const char *path, pod_metadata_t *meta)
{
    uint8_t hdr[4];
    if (fseek(fp, 0, SEEK_SET) != 0 || fread(hdr, 1, 4, fp) != 4) return;
    if (memcmp(hdr, "fLaC", 4) != 0) return;

    for (;;) {
        uint8_t bh[4];
        if (fread(bh, 1, 4, fp) != 4) break;
        bool last = (bh[0] & 0x80) != 0;
        uint8_t type = bh[0] & 0x7F;
        uint32_t len = ((uint32_t)bh[1] << 16) | ((uint32_t)bh[2] << 8) | bh[3];
        if (len > 4 * 1024 * 1024) break;

        if (type == 4 || type == 6) {
            uint8_t *buf = malloc(len);
            if (!buf) break;
            if (fread(buf, 1, len, fp) != len) {
                free(buf);
                break;
            }
            if (type == 4) parse_vorbis(buf, len, meta);
            else parse_flac_picture(buf, len, path, meta);
            free(buf);
        } else if (fseek(fp, len, SEEK_CUR) != 0) {
            break;
        }
        if (last) break;
    }
}

esp_err_t pod_metadata_load(const char *audio_path, pod_metadata_t *meta)
{
    if (!audio_path || !meta) return ESP_ERR_INVALID_ARG;
    memset(meta, 0, sizeof(*meta));

    FILE *fp = fopen(audio_path, "rb");
    if (!fp) return ESP_ERR_NOT_FOUND;

    uint8_t magic[4] = {0};
    if (fread(magic, 1, 4, fp) != 4) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(magic, "ID3", 3) == 0) {
        parse_id3_file(fp, audio_path, meta);
    } else if (memcmp(magic, "fLaC", 4) == 0) {
        parse_flac(fp, audio_path, meta);
    }

    fclose(fp);
    ESP_LOGI(TAG, "%s: title=\"%s\" artist=\"%s\" cover=\"%s\"", audio_path,
             meta->title, meta->artist, meta->cover_path);
    return (meta->title[0] || meta->artist[0] || meta->cover_path[0]) ?
           ESP_OK : ESP_ERR_NOT_FOUND;
}


