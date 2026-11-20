# Font partition

The flash layout reserves an 8 MB raw data partition for fonts:

```text
font, data, 0x40, 0x410000, 0x800000
```

## Packed image (version 2)

`font_data/font_partition.bin` is an 8 MB raw image built from the two external
CJK faces:

- `font_data/cjk16.bin` — 16 px, 4 bpp, line height 20 / base line 5
- `font_data/cjk24.bin` — 24 px, 2 bpp, line height 30 / base line 7

The 16 px face covers ASCII, Latin-1, general punctuation, CJK punctuation, the
full `0x4E00-0x9FFF` CJK Unified Ideographs range, and fullwidth forms (21,490
code points), sourced from `SourceHanSansSC-Normal.otf` bundled with LVGL.

The 24 px face uses `assets/zh-cn.ttf` (`SDK_SC_Web Heavy`, 21,251 code
points) for headings and large values. Its license is proprietary/HANYI; this
is a local build choice and must be reviewed before redistributing the font
partition. The 16 px body face remains SIL OFL Source Han Sans.

The bitmap data is **never copied into RAM**. `pod_font_init()` memory-maps the
partition and `pod_font_load_font()` returns a tiny `lv_font_t` whose glyph
callbacks point directly at the flash-mapped glyph table. LVGL expands one
glyph at a time into its normal A8 draw buffer, exactly like a built-in font.
This keeps the 16 + 24 px pair at 6.14 MB of flash data inside the 8 MB
partition (2.24 MB remains free for future faces) and a few hundred bytes of
static RAM.

> The previous implementation used `lv_binfont_create()` with
> `LV_FONT_FMT_TXT_LARGE=n`. Its 20-bit `bitmap_index` wrapped after 1 MB, so
> later glyphs had valid metrics but empty bitmaps. It also loaded the entire
> font into RAM. Version 2 avoids both problems.

## Rebuild

```powershell
python tools/build_cjk_fonts.py `
  --font managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf `
  --font-24 assets/zh-cn.ttf
python tools/pack_font_partition.py
```

The first command rasterizes each size with Pillow/FreeType and writes
`cjk16.bin`, `cjk24.bin`, and `faces.json`. The second packs the outer header
and blobs into `font_partition.bin`, updates `font_manifest.json`, and verifies
that the image fits the 8 MB partition. The generator needs the system Python
packages `fonttools`, `pillow`, and `numpy` (they are not in the ESP-IDF venv).

Format (little endian):

```text
offset 0:
  pod_font_header_t {
    uint32_t magic;        // 0x544E4F46 "FONT"
    uint16_t version;      // 2
    uint16_t count;
    uint32_t header_size;  // 20 + count * 12
    uint32_t blob_size;    // total used bytes
    uint32_t flags;
  }
  pod_font_entry_t[count] {
    uint16_t pixel_size;
    uint16_t bpp;
    uint32_t offset;       // absolute partition offset
    uint32_t length;
  }
  face blobs padded to 16 bytes

face blob:
  pod_font_face_t {
    uint32_t magic;        // "FACE"
    uint16_t version;      // 1
    uint16_t bpp;
    uint16_t pixel_size;
    uint16_t flags;
    uint16_t range_count;
    uint16_t reserved;
    int16_t  line_height;
    int16_t  base_line;
    uint32_t glyph_count;
    uint32_t ranges_offset;
    uint32_t glyphs_offset;
    uint32_t bitmap_offset;
    uint32_t bitmap_size;
  }
  pod_font_range_t[range_count] {
    uint32_t start;        // first code point
    uint32_t end;          // last code point, inclusive
    uint32_t glyph_start;  // glyph index of `start`
  }
  pod_font_glyph_t[glyph_count] {
    uint32_t bitmap_offset; // relative to the face bitmap blob
    uint16_t adv_w;         // advance in 12.4 fixed point
    uint8_t  box_w;
    uint8_t  box_h;
    int8_t   ofs_x;
    int8_t   ofs_y;
    uint16_t reserved;
  }
  glyph bitmaps, row-major, rows byte-aligned
```

`pod_font` converts `adv_w` from 12.4 fixed point to whole pixels before
returning it through the LVGL glyph descriptor.

## Flashing

A full flash installs the partition table and creates the empty partition:

```powershell
idf.py -p COM9 flash
```

Then write the packed font image:

```powershell
python $env:IDF_PATH\components\partition_table\parttool.py `
  --port COM9 write_partition `
  --partition-name font `
  --input font_data\font_partition.bin
```

`idf.py app-flash` does **not** update the font partition.

For a single-image install that also contains the font partition and an empty,
formatted littlefs partition, see [FULL_FIRMWARE.md](FULL_FIRMWARE.md).
