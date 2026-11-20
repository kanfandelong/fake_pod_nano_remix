# POD NANO · 460 × 460 音乐播放器触屏 UI

面向 **2.0 英寸、460 × 460、约 326 ppi** OLED 屏的交互原型，以及后续 LVGL / 数据层的设计约束。

> **当前阶段：UI 设计原型。**  
> `ui/index.html` 中的目录、文件、播放计数均为模拟数据；**没有接入真实 SD 扫描、LittleFS、NVS 或音频播放**。  
> 涉及工程实现的部分统一写在本文第 8 节，先形成约定，不在 UI 阶段写固件代码。

- 交互原型：`ui/index.html`
- 设计总览：`ui/gallery.html` / `ui/screenshots/00-gallery.png`
- 技术目标：LVGL 9 + ESP32-S3-R8 + CO5300 + CST820 单点触摸
- UI 文案：英文；后续中文化需单独做字库子集

---

## 1. 两个核心约束

### 1.1 2 英寸屏的物理尺寸

460 × 460、2.0 英寸屏的物理宽度约 **35.9 mm**，约 **325 ppi**。

| 屏幕像素 | 物理尺寸 | 用途 |
|---:|---:|---|
| 12 px | ≈0.94 mm | 正文 / 时间 / 次要信息的下限 |
| 14 px | ≈1.09 mm | 歌手、说明文字 |
| 23 px | ≈1.80 mm | 统计数值 |
| 28 px | ≈2.19 mm | 歌曲标题 |
| 30 px | ≈2.34 mm | 页面标题 |
| 38 px | ≈2.97 mm | Tab 高度 |
| 44 px | ≈3.44 mm | 顶栏按钮可见尺寸；热区扩到 52 px |
| 46 px | ≈3.59 mm | 随机 / 循环按钮；热区扩到 52 px |
| 52 px | ≈4.06 mm | 上一首 / 下一首 |
| 58 px | ≈4.53 mm | 歌曲 / 榜单行 |
| 60 px | ≈4.68 mm | 文件夹行 |
| 62 px | ≈4.84 mm | 队列行 |
| 68 px | ≈5.31 mm | 主播放按钮 |
| 70 px | ≈5.47 mm | 音乐库 source 行 |

尺寸原则：

1. 正文不小于 12 px；功能性数字不小于 10 px；7–9 px 只用于角标/脚注。
2. 主播放键 68 px，上一首/下一首 52 px，列表行 58–62 px。
3. 可视尺寸之外扩大热区：顶栏、收藏、Listening/Files 入口视觉 44 px，命中区 52 px；进度条视觉 4 px、命中高度 44 px。
4. 原型内部使用原生 460 × 460 坐标，LVGL 不要先按 230 × 230 排版再放大。

> 本稿按**方形 460 × 460 面板**排版。如果实物为圆形 AMOLED，需要重排为圆内安全区版本，四角内容至少退到距边 42–48 px。

### 1.2 设备没有 RTC

因此以下内容**不进入 UI**：

- 顶栏时钟（改为 `POD` 字标）
- Always-on clock（改为基于空闲计时器的 `Auto-dim`）
- 本周 / 今日收听
- 连续收听天数
- 按天/周柱状图

Listening 页只使用**本机可累计数据**：

- 每首歌的播放次数
- 累计播放时长（播放秒数，或用 `Σ 播放次数 × 曲目时长` 推导）
- 收藏标记
- 按播放次数排序的 Top played

---

## 2. 文件夹即播放列表

这是一个**文件夹优先**的音乐库，而不是歌曲/歌手/专辑分类库。

### 2.1 核心规则

1. 每个文件夹中，**直属的、解码器支持的音频文件**组成一个播放列表。
2. **不递归合并**子文件夹中的歌曲。
3. 每个子文件夹是独立的播放列表；可以继续嵌套。
4. 一个文件夹可以同时拥有：
   - 自己的直属歌曲（当前文件夹 playlist）
   - 一个或多个子文件夹（各自独立的 playlist）
5. 没有直属音频的文件夹只是容器；不显示“播放全部”。
6. 默认库根目录：`/sdcard/music`。
7. 用户可通过 Files 页面把其他文件夹加入音乐库；这些文件夹成为新的 library source。
8. 不存在“按歌手/专辑聚合”的 UI。

### 2.2 对播放队列的影响

从某个文件夹里点播歌曲时，**Up Next 队列 = 当前文件夹的直属歌曲**。

- `/sdcard/music` 直属 10 首歌 → 队列 10 首
- `/sdcard/music/Live Takes` 直属 2 首歌 → 队列 2 首
- `/sdcard/field-recordings/Coastline` 直属 2 首歌 → 队列 2 首
- 子文件夹的歌曲不会混入父文件夹队列

如果用户在 Files 中直接点一个库外的音频文件，队列使用该文件所在文件夹的直属歌曲，并照常计入 Listening 统计。

---

## 3. 屏幕地图

```
                 ┌──────────────────┐
                 │    Now Playing   │
                 └────────┬─────────┘
                          │ Menu
                 ┌────────▼─────────┐
                 │  POD / OS sheet  │
                 └──┬─────┬─────┬───┘
                    │     │     │
              ┌─────▼─┐ ┌─▼───┐ ┌▼────────┐
              │Library│ │Queue│ │ Settings│
              └──┬────┘ └─────┘ └─────────┘
                 │
       ┌─────────▼──────────┐
       │ Library sources    │
       │ /sdcard/music ...  │
       └─────────┬──────────┘
                 │
       ┌─────────▼──────────┐        ┌──────────┐
       │ Folder / playlist  │◀──────▶│  Files   │
       │ nested folders     │        │ add root │
       └────────────────────┘        └──────────┘

       Listening（统计）可从 Library 右上角或 Menu 进入
```

设备 UI 由 6 个页面 + 1 个底部菜单组成：

1. Now Playing
2. Library（Sources / Folder 两层状态）
3. Files
4. Up Next
5. Settings
6. Listening（Top played / Favorites）
7. Menu sheet（覆盖层）

原型外部还有 01–06 的评审跳转，不属于设备 UI。

---

## 4. 界面说明

### 4.1 Now Playing

- 220 × 220 专辑封面，24 px 圆角。
- 标题 28 px；歌手 / 专辑 14 px；时间 12 px。
- 进度条视觉 4 px，旋钮 12 px，热区 44 px。
- 控制区：随机 46 / 上一首 52 / 播放 68 / 下一首 52 / 循环 46。
- 左上角打开菜单；右上角显示耳机和电量。
- 顶栏使用 `POD` + 页面状态，不显示时间。
- 封面横向滑动切歌，拖动进度条 seek。

### 4.2 Library

#### Sources 状态

- 标题 `Library`，副标题如 `1 source · 2 playlists · 12 tracks`。
- 右上角 Listening 入口（图表）。
- 列表：
  - Source 行 70 px：文件夹图标、名称、路径、playlist / track 计数、DEFAULT 角标。
  - `Add music folder` 虚线入口，打开 Files 页面。
  - 一行说明：`Each folder is one playlist. Nested folders stay separate.`
- 默认 source 为 `/sdcard/music`。

#### Folder 状态

- 标题为当前文件夹名；副标题为绝对路径 + 本文件夹直属歌曲数。
- 右上角：
  - 有直属歌曲时显示 `Play folder` 圆形播放键（强调色）。
  - Listening 图表按钮常驻。
- 列表：
  - `FOLDERS` 区块：子文件夹行 60 px，显示直属 track 数与子文件夹数。
  - `FOLDER PLAYLIST · N TRACKS` 区块：**只列直属音频文件**。
  - 文件行 58 px：42 px 封面/缩略图、标题、格式、时长；不显示歌手/专辑分组。
- 顶部返回键：当前在子文件夹时回上一级；在 Sources 根时回 Now Playing。
- 底部 Mini Player 常驻。

### 4.3 Files

用于浏览 SD 卡、添加新的 library source、播放库外文件。

- 标题 `Files` 或当前文件夹名；副标题为当前路径 + 直属文件数。
- 右上角显示 SD 剩余空间（如 `18.2 GB free`）。
- 列表：
  - 文件夹行：名称、直属 track 数 / 子文件夹数；已纳入库的文件夹显示 `IN LIBRARY` 角标。
  - 音频文件行：可点击直接播放；队列仍按所在文件夹的直属歌曲生成。
- 底部固定操作按钮：
  - 当前目录可添加：`Add this folder`
  - 已在库中：`In library`（禁用）
  - 在 `/sdcard` 根：`Open a folder to add`（禁用）
  - 没有可解析音频：`No audio in this folder`（禁用）
- 返回键：在子目录中回上一级；在 `/sdcard` 根回 Library。
- 添加的 folder 会出现在 Library Sources 中；嵌套子文件夹不合并，仍是独立 playlist。
- 第一版不提供移除 library source 的操作，后续再补。

### 4.4 Up Next

- 标题 `Up Next`，副标题为 `{文件夹名} · {N} tracks`。
- 队列行 62 px，显示序号、封面、标题、歌手、时长。
- 当前曲目有暖色描边、左侧均衡器。
- 队列只来自当前文件夹的直属歌曲，不包含子文件夹。
- 右上角随机按钮与播放页共享状态。

### 4.5 Listening

- 无 RTC。
- `Total plays` 和 `Play time` 汇总卡。
- `Top played`：按播放次数降序，行 58 px；显示排名、封面、标题、相对播放量细条、次数。
- `Favorites`：收藏列表；每行右侧独立心形按钮，可直接取消收藏；无收藏时显示空状态。
- `Play time` 使用累计播放秒数，或“播放次数 × 时长”推导；不是本周/今日。
- 只显示播放次数 > 0 的曲目。

### 4.6 Settings

- 行高 54 px。
- Playback：Volume、Output、Equalizer、Gapless。
- Display：Brightness、Auto-dim（30 s after pause）。
- System：Storage、Firmware。
- 不使用时钟/日期类设置。

### 4.7 Menu sheet

- 当前曲目卡 + 音量。
- `Listening` 快捷行。
- Library / Up Next / Settings 三个大入口。
- 底部提示与版本信息。
- LVGL 上不做背景实时模糊，使用深色遮罩 + 高不透明面板。

---

## 5. 视觉系统

### 5.1 颜色

| Token | 值 | 用途 |
|---|---|---|
| `--screen-bg` | `#050506` | OLED 主背景 |
| `--text` | `#f4f1ea` | 主文字 |
| `--muted` | `rgba(244,241,234,.52)` | 次级文字 |
| `--faint` | `rgba(244,241,234,.31)` | 辅助信息 |
| `--line` | `rgba(255,255,255,.085)` | 分隔线 / 描边 |
| `--accent` / `--accent-2` | 随当前曲目变化 | 进度、播放键、激活态 |

专辑主题色：

| 专辑 | Accent | Accent 2 |
|---|---|---|
| Daylight Studies | `#ff7a67` | `#ffc36b` |
| After Hours | `#91a4ff` | `#c9b8ff` |
| Soft Focus | `#c9e06f` | `#8fd3a5` |
| Low Signals | `#ff6e9a` | `#ffb1c5` |
| Blue Hour | `#64d8cf` | `#91e8d8` |

背景光晕只在切歌时变化，不逐帧动画。

### 5.2 字体

原型使用系统字体，保证离线可看：

- Display：`Segoe UI Variable Display`, `Segoe UI`
- 正文：`Segoe UI Variable Text`, `Segoe UI`
- 数字：tabular-nums

LVGL 建议：

- 英文：Inter / Manrope（标题 Semibold）
- 中文：Noto Sans SC 子集，至少 12 / 14 / 28 px
- 数字等宽，避免时间文本跳动

### 5.3 间距 / 圆角

- 屏幕安全边距：左右 22 px
- 顶栏：56 px；内容区 404 px
- 行高：source 70、folder 60、song/file 58、queue 62、setting 54
- 卡片圆角 11–16 px；封面 24 px；屏幕 36 px
- 保持 OLED 深背景，不使用大面积浅色面板

---

## 6. 交互规格

### 6.1 Library / Files

- Library source 行：进入该 source 的根文件夹。
- Folder 行：进入子文件夹；不会把子文件夹曲目并入父级。
- 文件行：从当前文件夹直属歌曲构建队列，开始播放并进入 Now Playing。
- `Play folder`：把当前文件夹直属歌曲全部作为队列，从第一首开始。
- Sources 根返回：Now Playing。
- 子文件夹返回：上一级文件夹。
- Files 的返回逻辑相同：子目录 → 上一级；`/sdcard` → Library。
- Add folder：写入 library roots（原型内为内存状态），更新 Sources 列表和文件管理角标。
- Files 中直接点音频：队列取该文件所在目录的直属歌曲；不需要先加入音乐库。

### 6.2 播放

- 播放/暂停、上一首/下一首、随机、循环、收藏。
- 进度条拖动 seek；封面左右滑动切歌。
- Next/Previous 在**当前文件夹队列**内移动。
- 队列播完：repeat off + 自动播放时停止；repeat all 时回队首；repeat one 时重播。
- 收藏与 Listening / Favorites 双向同步。

### 6.3 Listening

- 点榜单行：从该曲所属文件夹建立队列并播放，回到 Now Playing。
- 点收藏行文字/封面：同上。
- 点心形按钮：只取消收藏，不触发播放。
- 播放计数在当前原型中“开始播放即 +1”。

### 6.4 动效

| 动效 | 时长 |
|---|---:|
| 页面切换 | 220 / 340 ms |
| 封面切歌 | 420 ms |
| Menu sheet | 430 ms |
| 均衡器 | 900 ms 循环 |
| Toast | 1.6 s |
| 按压反馈 | 150 ms |

LVGL 建议页面切换缩短到 180–240 ms。

### 6.5 键盘与硬件

原型：`Space` 播放暂停、`←/→` 切歌、`M` 菜单、`L/Q/S/I/F` 页面、`Esc` 返回。  
硬件建议：`BTN_PIN`（GPIO48）→ Home/返回，`BOOT_PIN`（GPIO0）→ 播放/暂停或电源。

---

## 7. LVGL 9 映射

### 7.1 对象树

```
screen (460×460, bg #050506)
├─ ambient
├─ topbar (56)
│  ├─ btn_menu (44)
│  ├─ col_state (POD / page label)
│  └─ row_battery
├─ pages (top 56)
│  ├─ page_now
│  ├─ page_library
│  │  ├─ source/folder list
│  │  ├─ mini_player
│  │  └─ contextual play button
│  ├─ page_filemanager
│  │  ├─ folder/file list
│  │  └─ add-folder action bar
│  ├─ page_queue
│  ├─ page_listening
│  └─ page_settings
├─ overlay_menu
└─ toast
```

单个 screen + 多个页面容器的做法比多个 screen 更容易保持顶栏、Toast、Menu 一致。

### 7.2 控件取舍

| UI | LVGL |
|---|---|
| Source / Folder 行 | `lv_button` + flex；60/70 高 |
| 文件行 | `lv_button` + 42 px 缩略图 + label |
| 播放全部 | 44 px `lv_button`，`LV_RADIUS_CIRCLE` |
| 文件夹角标 | 小 `lv_label` + 背景样式 |
| Files 底部按钮 | 48 px `lv_button`，disabled 状态 |
| 进度 | `lv_slider` + `lv_obj_set_ext_click_area` |
| 队列 / 榜单 | 固定行高 + `LV_OBJ_FLAG_SCROLLABLE` |
| 开关 | `lv_switch` + 扩大点击区 |
| 图片 | 预渲染缩略图，`lv_image` |
| Menu | 遮罩 + 底部面板 y 动画 |

### 7.3 性能

- 不要一次把整棵文件夹树和所有封面载入内存。
- 只保留当前文件夹的直接子项和当前播放队列。
- 预渲染 42 px 缩略图；Now Playing 220 px 单独缓存。
- 不在 LVGL 中做背景模糊；Menu 使用 92% 不透明面板。
- 阴影只保留播放键等少量元素；其余用边框/明度差。
- 长列表使用固定行高和延迟创建/复用对象。
- 字体使用 28 / 23 / 14 / 12 / 10 等目标字号，不做对象缩放。

---

## 8. 工程实现约定（后续阶段，不在原型中实现）

这一节记录项目实际落地时的目录和数据设计。当前 UI 原型只模拟，不接线。

### 8.1 存储介质分工

| 位置 | 内容 | 是否可重建 |
|---|---|---|
| `/sdcard/music` | 默认音乐库根目录 | 用户文件，不可重建 |
| `/sdcard/...` | 用户通过 Files 加入的其他 library roots | 用户文件，不可重建 |
| `/sdcard/.cache/` | 预处理索引、元数据、封面缩略图、波形等 | **可重建** |
| `/littlefs/data/` | 收藏、播放计数、库根列表等用户数据 | 部分可重建；需要保留 |
| NVS | 音量、亮度、EQ、Gapless、Auto-dim 等设置 | 少量键值 |

分工原则：

- **`.cache` 只放可重建的派生数据**；删除缓存不应丢失用户数据。
- **`/littlefs/data` 放不可从 SD 重新推导的用户状态**。
- **NVS 放小型键值设置**；不要把大数组、路径映射或逐曲状态塞进 NVS。
- 用户文件永远不放进 LittleFS。

### 8.2 对“两个 LittleFS 文件”的评估

原方案：

1. `path_sha256 + play_count + duration + favorite` 状态文件
2. `path ↔ sha256` 映射文件

建议调整为：

1. **保留**：`/littlefs/data/playstate.bin`，以路径摘要为 key，保存播放计数、累计播放秒数、收藏标记等。
2. **移出**：`path ↔ sha256` 映射是**派生数据**，建议放进 `/sdcard/.cache/` 的索引，而不是 LittleFS。
3. **不要重复存 duration**：时长属于可重建元数据，放 cache；如果 Play time 要准确，状态里保存实际播放秒数，比“计数 × 时长”更可靠。

理由：

- 给定路径可以重新计算 SHA-256，查找状态不需要路径映射表。
- 映射表体积会随曲库增长；放进 LittleFS 会浪费 Flash，并增加两文件一致性问题。
- 缓存丢失时，重新扫描路径并重算 hash 仍然能找回统计。
- 反向映射只在显示“孤立统计”（文件已删除/改名）时需要；这类记录可以暂时不展示。

如果后续确实需要在 LittleFS 中维护映射，应将它定义为可重建的镜像，并带版本号/CRC，不能作为唯一事实来源。

### 8.3 路径身份与规范化

- 使用规范化后的绝对路径计算 SHA-256：
  - UTF-8
  - 统一 `/`
  - 去除重复分隔符和尾随 `/`
  - FAT 大小写不敏感：决定是否对路径 casefold 后再 hash
  - 固定 `/sdcard` 挂载前缀，或存 root id + 相对路径
- 为节省空间，状态文件可以只存 **SHA-256 的前 16 字节**，但计算仍使用完整 SHA-256。
- 路径改名/移动会使 hash 变化，旧统计会成为孤立记录。第一版可接受；后续可增加：
  - 文件内容指纹（文件大小 + 首尾块 hash）作为迁移辅助
  - 或显式“重建统计”工具
- 同一路径重复加入多个重叠 root 时，应按规范化路径去重，避免重复计数。

### 8.4 文件夹索引

`.cache` 中的索引按文件夹组织，而不是把全部歌铺平：

```
FolderRecord {
  folder_id
  parent_id
  path_offset
  direct_track_count
  direct_track_ids[]
  child_folder_ids[]
  total_track_count       // 用于 source 行
  playlist_count          // 用于 source 行
}
```

- `direct_track_ids[]` 只包含直属文件，正是 UI 的当前文件夹 playlist。
- `child_folder_ids[]` 单独存在；不会递归并入父级队列。
- Source 行的 `N playlists / N tracks` 使用预聚合字段，不必现场递归。
- 打开文件夹时只读当前 folder record 和其直接子项，不构建全树。
- 排序：文件夹在前，按名称；文件按文件名；大小写规则与路径规范化一致。

### 8.5 用户状态文件建议

`/littlefs/data/playstate.bin`

```
Header {
  magic, version, record_size, count, crc32
  total_plays u64
  total_played_seconds u64
}
Record {
  path_id[16]
  play_count u32
  played_seconds u32
  flags u8        // bit0 favorite, bit1 completed...
  reserved u8[3]
}
```

- 每首曲目 24–28 字节；1 万首约 240–280 KB。
- Header 存总量，Listening 的 `Total plays` / `Play time` 不需要全量扫描。
- Top played：扫描状态记录并用**固定大小最小堆**取前 N（N = 10 或 20），O(N log N)，不需要全排序。
- Favorites：按 flags 筛选，再解析路径/标题。
- 若以后显示“最近播放”，不要用 RTC；增加 `last_play_seq u32` 全局单调序号即可。
- 写入：
  - 不要每次切歌都重写整个文件。
  - 可使用 append-only journal + 定期 compaction，或 debounce 到暂停/60 秒/退出时原子替换。
  - 推荐结构：`playstate.bin` 快照 + `playstate.log` 增量；启动时回放，日志过大时合并。

`/littlefs/data/library.cfg`

- 保存 library roots 列表、默认根标记、schema 版本。
- 默认包含 `/sdcard/music`。
- 路径长度上限固定，例如 255 字节；不要使用无界 JSON。

### 8.6 `.cache` 索引与预处理

推荐布局：

```
/sdcard/.cache/
  manifest.bin          # format version / root generation / CRC
  folders.bin           # folder records + direct/child offsets
  tracks.bin            # duration, codec, bitrate, art id, path/title offsets
  strings.bin           # path/title 字符串池
  hashes.bin            # sorted path_id -> track_id，用于统计解析
  art/<path_id>_42.rgb565
  art/<path_id>_220.rgb565
```

- `.cache` 可由 PC 工具预生成，也可由设备首次扫描时生成；两者必须共用 schema version。
- 每次加载校验 magic/version/CRC；不匹配就重建。
- 没有 RTC，缓存失效不能依赖“日期”：
  - 用库根 generation / 扫描代数
  - 用户手动 trigger rescan
  - Files 添加/移除 folder 时局部重建
  - 文件系统时间戳只能作为辅助，不可依赖
- 封面缩略图预生成，避免在 LVGL 中缩放 512 原图。
- 也可以缓存波形/频谱等后续功能需要的预处理数据，但第一版不必。

### 8.7 内存策略（ESP32-S3-R8）

- 只加载当前文件夹的直接子项；离开文件夹后释放 UI 列表。
- 当前播放队列只保存 track id / path_id 数组。
- 缩略图：
  - 42 × 42 RGB565 ≈ 3.5 KB/张
  - 16 张 LRU ≈ 56 KB
  - 220 × 220 RGB565 ≈ 96.8 KB
- 状态记录：
  - 16B path_id + 计数/秒数/标记 ≈ 24–28B/首
  - 1 万首约 240–280 KB，可放 PSRAM；不建议放内部 SRAM
- 不要使用完整 JSON 树作为运行时索引；使用固定长度 binary record + 字符串池。
- 不要在滚动时逐行分配 `std::string`；使用偏移和复用小缓冲。
- 图片解码缓冲放到 PSRAM，并与 LVGL 绘制缓冲分离规划。

### 8.8 扫描 / 文件管理约定

- 默认 root：`/sdcard/music`
- 支持扩展名由解码器能力决定（例如 flac/mp3/wav/m4a/aac/ogg），扫到不支持文件时忽略。
- 每个文件夹的直接音频文件构成 playlist；扫描逻辑不递归合并。
- Files 的“Add this folder”会把当前路径加入 library roots；其子文件夹作为独立 playlist 被索引。
- 如果目标路径已被现有 root 覆盖，禁止重复添加（UI 已显示 `In library`）。
- Files 直接点文件可播放库外文件；统计仍按路径 hash 记录，不需要先入音乐库。
- 第一版不做删除 source；后续加入 remove 时，需要同步清理/保留统计并更新 source 列表。
- 添加/移除 source 后，cache 重建可以是局部的；不需要全盘扫描。

### 8.9 后续需要确认的问题

- 路径 hash 与内容 hash 的取舍：路径 hash 便宜但改名丢统计；内容 hash 稳定但扫描成本高。
- LittleFS 分区可用容量与日志 compaction 阈值。
- 扫描时是否读取完整 tag（首帧读取、耗时）还是文件名优先。
- 是否允许同一文件夹被多个 root 覆盖时的去重规则。
- 中文 UI 与中文 ID3 需要字体子集和文件名规范。
- 若未来加 SNTP，可把时钟做成可选显示；离线仍不显示假时间。

---

## 9. 资源清单

| 文件 | 用途 |
|---|---|
| `assets/cover-solar.svg` | Daylight Studies / Live Takes |
| `assets/cover-night.svg` | After Hours |
| `assets/cover-paper.svg` | Soft Focus |
| `assets/cover-velvet.svg` | Low Signals |
| `assets/cover-tide.svg` | Blue Hour / Coastline |
| `screenshots/01-now.png` | Now Playing |
| `screenshots/02-library.png` | Library / Sources |
| `screenshots/03-library-folder.png` | Library / Folder playlist |
| `screenshots/04-files.png` | Files / SD root |
| `screenshots/05-files-add.png` | Files / add folder state |
| `screenshots/06-queue.png` | Up Next |
| `screenshots/07-settings.png` | Settings |
| `screenshots/08-listening.png` | Listening / Top played |
| `screenshots/09-favorites.png` | Listening / Favorites |
| `screenshots/10-menu.png` | Menu sheet |
| `screenshots/00-preview.png` | 整体展示页 |
| `screenshots/00-gallery.png` | 八屏设计总览 |

---

## 10. 原型使用

```text
ui/index.html       # 可交互原型
ui/gallery.html     # 八屏设计总览
```

指定状态：

```text
ui/index.html?screen=library
ui/index.html?screen=library&folder=/sdcard/music
ui/index.html?screen=filemanager
ui/index.html?screen=filemanager&path=/sdcard/field-recordings
ui/index.html?screen=queue
ui/index.html?screen=settings
ui/index.html?screen=listening
ui/index.html?screen=listening&view=favorites
ui/index.html?screen=now&track=1
ui/index.html?screen=now&ui=menu
```

干净截图模式：

```text
ui/index.html?capture=1&screen=library
ui/index.html?capture=1&screen=library&folder=/sdcard/music
ui/index.html?capture=1&screen=filemanager
ui/index.html?capture=1&screen=listening&view=favorites
```

没有外部字体和 CDN 依赖。原型内的 Files 添加操作只修改当前会话状态，刷新即恢复 demo 初始状态。

---

## 11. 已完成验证

- 文件夹浏览：Sources → `/sdcard/music` → `Live Takes` 的层级与返回逻辑。
- 直接文件播放：从文件夹文件行开始播放时，队列只包含当前文件夹直属歌曲。
- Files：浏览目录、添加 `/sdcard/field-recordings`、已加入状态的禁用反馈、库外文件播放。
- 队列：文件夹内切歌、返回上一级、队列摘要跟随文件夹。
- Listening：播放次数排行、收藏、空状态、菜单入口、无 RTC 数据。
- 所有页面在 460 × 460 原生分辨率下截图检查，无横向溢出。
- 临时验证脚本运行结果：文件夹 / Files / 队列 / Listening 流程 45/45 项通过，无运行时错误。

---

## 12. 下一阶段

1. 先做 **Now Playing + Library(folder) + Files** 的 LVGL 纵向切片。
2. 确定 `/sdcard/.cache` 的二进制索引 schema 和 PC 端生成工具。
3. 在 LittleFS 中实现 library roots + playstate 的最小闭环，验证 1 万首规模的 RAM/耗时。
4. 在实物 2 英寸屏上核对 12 px 正文与 58–62 px 行高。
5. 后续再加 source 移除、重命名迁移、封面/标签解析和中文 UI。
