# POD NANO 固件（fake_pod_nano_remix）

基于 **ESP32-S3** 的便携音乐播放器固件。2.0 英寸 460 × 460 AMOLED + 单点触摸，
CS43131 DAC，microSD 曲库与片内 LittleFS 播放数据，界面使用 **LVGL 9.4**，
解码由 [espaudiocore](espaudiocore/README.md) 提供。

- 460 × 460 触摸 UI：Now Playing、Library、Files、Up Next、Settings、Listening
- microSD 曲库（按文件夹成列表）+ 片内 LittleFS 持久化播放次数、收藏、队列、最近播放
- WAV / MP3 / AAC / M4A / FLAC / OGG Vorbis / Opus / WavPack 解码
- ID3v2 / FLAC 元数据、封面与歌词
- 外置 CJK 字库（16 px / 24 px），字形直接从 flash 映射，不占 RAM
- USB-Serial-JTAG 调试 shell（与 `idf.py monitor` 共用一个口）

## 硬件

| 部件 | 型号 / 连接 |
| --- | --- |
| MCU | ESP32-S3，240 MHz，16 MB flash + 8 MB 八线 PSRAM（80 MHz） |
| 屏幕 | CO5300 AMOLED，460 × 460，QSPI（CS 8 / SCK 9 / D0–D3 10–13 / RST 7 / TE 6） |
| 触摸 | CST820，I2C（SDA 3 / SCL 2 / INT 4 / RST 5） |
| DAC | CS43131，I2S（BCK 40 / WS 38 / DO 39 / RST 41），I2C 控制 |
| 存储 | microSD，SDMMC 4-bit（CLK 16 / CMD 17 / D0 15 / D1 14 / D2 21 / D3 18） |
| 麦克风 | PDM（DATA 46 / SCK 45） |
| 按键 | BTN 48（短按切换纯封面页）、BOOT 0（短按菜单 / 长按息屏） |
| 电池 | ADC 1 |

完整引脚定义见 [pinmap.txt](pinmap.txt)。

## 软件栈与分区

ESP-IDF **v6.1.0**（`main/idf_component.yml` 要求 `>=6.1.0`）、LVGL 9.4.0、
`esp_lcd_co5300` 2.0.3、`joltwallet/littlefs` 1.23.1，版本由
[dependencies.lock](dependencies.lock) 锁定。

[partitions.csv](partitions.csv)（16 MB flash）：

| 名称 | 类型 | 偏移 | 大小 |
| --- | --- | --- | --- |
| nvs | data/nvs | 0x009000 | 24 KB |
| phy_init | data/phy | 0x00F000 | 4 KB |
| factory | app | 0x010000 | 4 MB |
| font | data/0x40 | 0x410000 | 8 MB |
| coredump | data/coredump | 0xC10000 | 128 KB |
| littlefs | data/littlefs | 0xC30000 | 3904 KB |

## 目录结构

```text
main/                     启动入口：屏幕/触摸、DAC、存储、字库、音频、UI 初始化
components/
  cs43131/                CS43131 DAC 驱动（I2C 控制 + I2S）
  audio_test/             板级 I2S/DAC 测试音
  esp_lcd_touch_cst820/   CST820 触摸驱动
  pod_audio/              播放控制状态层（espaudiocore 之上）
  pod_debug/              USB-Serial-JTAG 调试 shell
  pod_display/            LVGL 显示/触摸移植层
  pod_font/               字库分区 mmap + LVGL 字体适配
  pod_metadata/           ID3v2 / FLAC 元数据、封面、歌词
  pod_storage/            SD / LittleFS / NVS 与曲库枚举
  pod_ui/                 全部 LVGL 页面
espaudiocore/             多格式解码核心（git submodule）
bootloader_components/    自定义 bootloader（可用 RTC 寄存器覆盖启动分区）
ui/                       HTML/CSS 交互原型，不参与固件构建
tools/                    字库生成、字体分区打包、整片镜像打包等脚本
patches/                  espaudiocore 的本地补丁
doc/                      调试控制台、字库分区、整片固件、数据手册
```

## 快速开始

### 1. 环境

- ESP-IDF **v6.1**（`idf.py --version` 应显示 6.1.x），目标 `esp32s3`
- Python 3.10+（ESP-IDF 自带 venv 即可）
- 一块 16 MB flash 的 ESP32-S3 板子；串口通过 USB-Serial-JTAG（示例用 `COM9`）

Windows 下先激活 IDF 环境（路径按自己的安装位置替换）：

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
```

Linux / macOS：

```bash
. $HOME/esp/esp-idf/export.sh
```

### 2. 获取代码

```powershell
git clone --recurse-submodules https://github.com/kanfandelong/fake_pod_nano_remix.git
cd fake_pod_nano_remix
```

已经克隆过的话补一次子模块：

```powershell
git submodule update --init --recursive
```

`pod_audio` 调用了 `espaudiocore_probe()`，它只存在于本地补丁里，**必须应用**，
否则链接会报 `undefined reference to espaudiocore_probe`：

```powershell
git -C espaudiocore apply ..\patches\espaudiocore-local-changes.patch
```

Linux / macOS 把 `..\` 换成 `../`。补丁说明见 [patches/README.md](patches/README.md)。

### 3. 构建

```powershell
idf.py build
```

首次构建会按 `dependencies.lock` 自动下载 managed components（LVGL 等），需要联网。
产物在 `build/`：`fake_pod_nano_remix.bin`、`bootloader/bootloader.bin`、
`partition_table/partition-table.bin`。

### 4. 烧录

```powershell
idf.py -p COM9 flash monitor
```

`idf.py flash` 只写 bootloader、分区表和 app，不会清空 `font` / `littlefs` 分区。
LittleFS 首次挂载失败会自动格式化。

### 5. 字库分区（中文界面需要）

仓库不包含 `assets/zh-cn.ttf` 和 `font_data/*.bin`（专有字体，见下方“许可”）。
没有字体分区时，16 px 中文会退回 LVGL 内置思源黑体，24 px 标题字形不可用。
生成并写入 8 MB 字体分区：

```powershell
pip install fonttools pillow numpy

# 16/24 px 都使用 LVGL 自带的思源黑体（SIL OFL）
python tools\build_cjk_fonts.py
# 如果自备了更重的标题字体，可只覆盖 24 px
python tools\build_cjk_fonts.py --font-24 assets\zh-cn.ttf

python tools\pack_font_partition.py

python $env:IDF_PATH\components\partition_table\parttool.py `
  --port COM9 write_partition `
  --partition-name font `
  --input font_data\font_partition.bin
```

格式细节见 [doc/FONT_PARTITION.md](doc/FONT_PARTITION.md)。

### 6. 整片固件

把 bootloader、分区表、app、字体和空 LittleFS 合并成一个 16 MB 镜像：

```powershell
python tools\pack_firmware.py
esptool --chip esp32s3 -p COM9 write_flash 0x0 build\fake_pod_nano_remix_full_16mb.bin
```

需要先完成第 5 步生成 `font_data/font_partition.bin`。整片写入会把 NVS 一起覆盖
（设置和 Wi-Fi 校准数据丢失）。细节见 [doc/FULL_FIRMWARE.md](doc/FULL_FIRMWARE.md)。

## 日常开发

- **调试 shell**：跑在 USB-Serial-JTAG 上，`idf.py monitor` 里输入 `help` 查看命令。
  按键、持久化文件、烧录分区说明见 [doc/DEBUG_CONSOLE.md](doc/DEBUG_CONSOLE.md)。
- **配置**：`sdkconfig` 是**故意提交**的，保证大家的编译配置一致；改配置用
  `idf.py menuconfig`，然后有意识地提交 `sdkconfig`。可复用的默认值放
  [sdkconfig.defaults](sdkconfig.defaults) / [sdkconfig.defaults.esp32s3](sdkconfig.defaults.esp32s3)。
- **存储路径**：曲库根目录 `/sdcard/music`，封面缓存 `/sdcard/.cache`，
  播放数据 `/littlefs/data`；文件夹与嵌套文件夹各自成列表，不合并。
- **UI 原型**：`ui/` 是独立 HTML 原型（模拟数据），固件界面在 `components/pod_ui`；
  原型截图在 `ui/screenshots/`。
- **接口调整**：`main.c` 初始化失败时会退回内置的 bring-up 测试页（触摸计数），
  屏幕/触摸调试时很有用。
- **主机侧测试**：`tools/host_test/` 用 stub 头文件在 PC 上跑元数据解析
  （`pod_metadata_host.c` + `test_meta.c`）。

## 常见问题

**链接报 `espaudiocore_probe` 未定义** — 没打补丁，执行第 2 步的 `git apply`。

**中文缺字 / 24 px 标题字体不对** — 字体分区没写或写的是旧镜像，重做第 5 步；
也确认 `df -h` 里 `font` 分区存在（shell 里可查）。

**整片烧录后设置全丢** — 正常，整片镜像把 NVS 也覆盖成 0xFF。

**`git status` 一直显示 ` m espaudiocore`** — 正常，子模块工作区带着上面那个补丁；
不要为了“干净”去提交或还原它。

**只显示 bring-up 测试页** — `pod_ui_start()` 返回失败，看 monitor 日志里的
`pod_ui_start failed`，再用 shell 的 `heap` 看内存；常见原因是 PSRAM 配置不对或堆不足
（`ESP_ERR_NO_MEM`）。

**flash 小于 16 MB** — 分区表按 16 MB 规划，换 flash 需要同时改 [partitions.csv](partitions.csv)。

## 文档

- [doc/DEBUG_CONSOLE.md](doc/DEBUG_CONSOLE.md) — 调试 shell 命令、按键、持久化格式
- [doc/FONT_PARTITION.md](doc/FONT_PARTITION.md) — 字体分区二进制格式与生成
- [doc/FULL_FIRMWARE.md](doc/FULL_FIRMWARE.md) — 整片 16 MB 固件的布局与烧录
- [ui/README.md](ui/README.md) — UI 设计稿与交互说明
- [espaudiocore/README.md](espaudiocore/README.md) — 解码库能力与 API
- [patches/README.md](patches/README.md) — espaudiocore 本地补丁

## 许可

各组件源码按文件头部的 `SPDX-License-Identifier` 使用（多为 MIT）。
`espaudiocore` 见其仓库。`assets/zh-cn.ttf`（HANYI）与由它生成的
`font_data/*.bin` 属于专有字体数据，不随本仓库分发；如需使用请自备授权副本。
