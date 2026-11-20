# POD NANO debug console

## Hardware keys

- **BTN (GPIO48, active-low)**: short press toggles the pure-cover page.
- **BOOT (GPIO0)**:
  - short press: overlay bottom menu
  - long press (600 ms): screen off / on

## Persistence

- Settings: NVS namespace `podcfg` (brightness, volume, auto screen-off, gapless,
  per-song counter enable).
- Playback data: LittleFS `/littlefs/data/playback.bin` (play count, last
  track, total listened seconds; v1 files still load).
- Per-song counters: LittleFS `/littlefs/data/song_stats.bin`
  (SHA-256 of path, 64-byte records, loaded into PSRAM at boot).
- Favorites: song_stats `reserved[0]` bit 0; no separate favorites file.
- Song path map: LittleFS `/littlefs/data/song_paths.bin`
  (hash -> path, appended only when a song is first counted; used by the
  Listening most-played list). PSRAM cache is capped at 2048 entries; when
  full, the lowest-play-count path is replaced only by a higher-count song,
  and the file is compacted after 64 evictions.
- Up Next queue: LittleFS `/littlefs/data/queue.bin`
  (ordered track paths; restored before the Now Playing page is built).
- Library sources: LittleFS `/littlefs/data/library_sources.bin`
  (up to 16 folder playlists; restored on boot).
- `Auto-save data` options: `Every song`, `5/10/15/30/45/60 min`.
  - `Every song` writes both files before the next decoder starts.
  - Timed modes only write once the interval has elapsed **and** the decoder is
    idle; they never write while a song is playing or paused.
  - Settings `SAVE` writes immediately, even during playback.
- LittleFS playback data is loaded into RAM at boot; per-song counters and the
  restored queue use PSRAM.

## Flash layout

```text
factory   app       0x010000  4 MB
font      data 0x40 0x410000  8 MB
coredump  data     0xC10000  128 KB
littlefs  data     0xC30000  3904 KB (all remaining flash)
```


The debug shell runs on the **USB-Serial-JTAG** port (COM9 on the current
board), the same port used by `idf.py monitor`. Application logs and the shell
share this port. This build makes USB-Serial-JTAG the primary console; the
application no longer logs on UART0 (GPIO43/44), although the ROM bootloader
still does.

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
idf.py -p COM9 monitor
```

Prompt:

```text
pod>
```

## Commands

| Command | Description |
| --- | --- |
| `help` | List commands (from `esp_console`) |
| `heap` | Internal / SPIRAM / DMA / 8BIT heap blocks |
| `heapdump [internal\|spiram\|dma\|caps]` | Detailed heap info |
| `tasklist` / `tasks` | Task list with stack and core columns (`vTaskList`) |
| `taskload [seconds]` | CPU load table over a sampling window |
| `backtrace` | Current task backtrace |
| `backtrace_all` | All task backtraces |
| `espreg -r <addr>` | Read a 32-bit register, optional `-bit <bit>` |
| `espreg -w <addr> <value>` | Write a register, optional `-bit <bit>` |
| `md <addr> [bytes]` | Memory hexdump |
| `hexdump <file> [offset] [length]` | File hexdump |
| `ls [path]` | List a VFS directory |
| `cat <path> [path...]` | Print file contents |
| `stat <path>` | File type, size, mode |
| `df` | `/littlefs`, `/sdcard`, `/` filesystem usage |
| `i2c` | Scan I2C addresses 0x08..0x77 |
| `dac r <reg>` | Read a CS43131 register (5-byte control port) |
| `dac w <reg> <value>` | Write a CS43131 register |
| `chipinfo` | Chip revision, IDF version, MAC, reset reason, uptime |
| `uitab <0-4>` | Switch UI tabs without touch (latched to the LVGL task) |
| `uiqueue` | Open the Up Next overlay without touch |
| `uiplay <n>` | Play eligible entry `<n>` from the current list (debug) |
| `uinext` | Play the next track in the current queue (debug) |
| `uicovermode` | Toggle pure cover mode |
| `uiscreen` | Toggle screen power |
| `uistorage` | Open Settings > Storage usage |
| `uitouch` | Open Settings > Touch calibration |
| `uirepeat <0\|1\|2>` | Repeat off / all / one |
| `uitopall` | Open the full Most Played list |
| `uifav` | Toggle favorite for current track |
| `uifavlib` | Open the Favorites library |
| `uilyrics` | Toggle the lyrics view |
| `uiautosave <min>` | Set LittleFS autosave interval for testing |
| `uiautooff <sec>` | Set auto screen-off timeout for testing |
| `uisave` | Save settings and playback data now |
| `uinav` | Toggle the bottom navigation bar |
| `uifiles` | Open `/sdcard` in the Files tab without touch |
| `uiscroll <y>` | Scroll the active Library/Files list to a Y offset |
| `coredump` | Show the flash coredump partition |
| `echo [text...]` | Echo arguments |
| `reboot` | Restart |

## Output format

The command output follows `D:\LiClock-self-use\src\Serial_cmd.cpp`:

- `PRINT_INFO` = bright cyan
- `PRINT_SUCCESS` = bright green
- `PRINT_WARNING` = bright yellow
- `PRINT_ERROR` = bright red
- table headers use bright magenta
- `tasklist` uses the same `vTaskList` header/columns
- `taskload` uses the same CPU-load table and per-row colour thresholds
- `ls` uses `Directory:`, `[DIR]`, `[FILE]`, `[UNKN]`
- `espreg` uses the same `-r/-w`, optional `-bit`, and binary-field output

## Notes

- `espreg -w`, `dac w`, and `md` on invalid addresses can crash or mute the
  device. They are intentionally explicit and print a warning.
- `uitab` and `uiqueue` only latch a request; the LVGL timer task performs the
  actual object changes, because LVGL is not thread-safe with the REPL task.
- `ls`, `cat`, `stat`, and `df` operate through VFS. `/sdcard` and `/littlefs`
  only appear after the storage layer mounts them; the storage layer is not
  wired into this bring-up build yet.
- Coredump is stored in the `coredump` partition (128 KB at `0xC10000`).
  A full flash (`idf.py flash`) is required once to install the new bootloader
  and partition table; an app-only flash will not create that partition.
  The full flash replaces the old custom multi-app layout (app0..app3/UAC), so
  back up the current 16 MB flash first if those are still needed.
- After a panic, `idf.py -p COM9 coredump-info` reads the ELF core dump from
  flash (the `coredump` partition must be the one in `partitions.csv`).
