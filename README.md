# 背单词（Vocabulary）— ThingsOS 电子墨水单词卡应用

这是 [ThingsOS](https://github.com/MindReset/read_pico_firmware) `0.2.0` 的一个分支，
在 Metalio E-Ink4-Plus（ESP32-S31，16 MB Flash / 16 MB PSRAM）上增加了一个
**不背单词风格**的单词学习应用：6588 词的词书、三轮学习、记忆曲线复习、
离线发音、统计概览与设置页。

> 本仓库包含上游完整源码；背单词应用位于 `native/src/vocab*.c` 与
> `native/firmware/main/vocab_ui.inc`，对上游的改动集中在这几处：
> `native/include/thingsos/ui.h`、`native/firmware/main/apps_controls.inc`、
> `native/firmware/main/main.c`、两个 `CMakeLists.txt`。补丁序列见 `patches/`。

## 功能

- **6588 词书**：词性按“一个词性一行”显示，四选一取最常用义项；
- **学习**：10 词一组，三轮（四选一 → 例句回想 → 裸词回想），失败的词自动进入重学组；
- **复习**：10 词一组单轮，认识/模糊/忘记三档；模糊只缩短间隔，忘记进入重学；
- **记忆曲线**：FSRS-6（可切经典模式），目标保持率可调；
- **离线发音**：Piper 生成的单词与例句 MP3，进屏自动朗读，可点按重播；
- **底栏与统计**：「我的」页显示今日/累计词数与学习时长；
- **设置页**：每日新词目标、复习上限、每组词数、保持率、算法、乱序、自动发音、答错处理、清空进度；
- **断点续学**：退出再进从组内原位继续。

## 快速开始

```bash
git clone https://github.com/SuSd-1/thingsos-vocab.git
cd thingsos-vocab

# 1. 构建主机测试（不需要硬件）
cmake -S native -B build/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --target thingsos_vocab_test -j
./build/host/thingsos_vocab_test

# 2. 构建设备固件（需要 ESP-IDF v6.1 与 esp32s31 支持）
. ~/esp/esp-idf/export.sh
cd native/firmware
idf.py --preview -B ../../build/native-idf -DTHINGSOS_BUILD_VERSION=0.2.0-vocab build

# 3. 只刷主应用分区（0x220000）
esptool --chip esp32s31 -p /dev/cu.usbmodem101 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x220000 ../../build/native-idf/ThingsOS_native.bin
```

词库与音频通过 USB 直连写入 SD 卡：
`/sdcard/.thingsos/vocab/{bank.bin, audio/<id>.mp3, audio/<id>_ex.mp3}`。

详细步骤见 [`docs/QUICKSTART.md`](docs/QUICKSTART.md)，
词书制作见 [`docs/VOCAB-DATA.md`](docs/VOCAB-DATA.md)。

## 目录

| 路径 | 内容 |
|---|---|
| `native/src/vocab.c`、`vocab_fsrs.c` | 词库、调度（FSRS-6）、组队计划 |
| `native/firmware/main/vocab_ui.inc` | 全部界面：首页、学习、复习、设置、统计 |
| `vocab-kit/scripts/` | 导入上游、构建、刷机、备份脚本 |
| `vocab-kit/tools/` | 词书导入、例句合并、Piper 发音生成、打包 |
| `patches/` | 相对上游 0.2.0 的补丁序列 |
| `samples/` | 10 词样例词库与 3 个示例音频 |
| `docs/` | 快速开始、词书数据管线、构建与排障 |

## 许可与署名

- 上游 ThingsOS：保留原 `LICENSE`（Apache-2.0）与 `THIRD_PARTY_NOTICES.md`；
- 例句：Tatoeba 语料（CC-BY 2.0，tatoeba.org）；
- 发音：Piper 语音模型（en_US-lessac-medium，模型自带许可）；
- 系统字体：方正屏显雅宋，授权见 `licenses/`。

---

# ThingsOS 0.2.0 — Metalio E-Ink4-Plus

Independent ESP-IDF firmware for the Metalio E-Ink4-Plus (ESP32-S31).

Version 0.2.0 adds Cubox reading sync, WeRead multi-device progress sync (with
matching-book prompt) and bounded chapter repair, adaptive portrait/landscape
wallpapers (684×1216 / 1216×684), GIF/reader image rendering updates, and website
feedback integration on top of 0.1.9 diagnostics.
See [release notes](native/docs/METALIO-0.2.0.md).
The application lives in `native/`; the project and default main/rescue firmware
version is defined by [`native/VERSION`](native/VERSION).

The pending offline WeRead reading-time feature has a separate E-Ink
authorization and durable outbox. Usage, recovery and validation boundaries:
[`WEREAD-OFFLINE-READTIME.md`](native/docs/WEREAD-OFFLINE-READTIME.md).
The adapted protocol carries its original copyleft licenses; see
[`READTIME-SOURCES`](licenses/READTIME-SOURCES.md).

```powershell
./native/build.ps1 -Rescue
./native/build.ps1
python ./native/package_metalio.py
```

Builds and packages are written under `build/`. These commands do not flash a
device or publish a release. Explicit device authorization is required.
Version 0.1.4 adds the fixes made after the 0.1.3 publication: the update check
pre-warms the release host after DHCP, connects with a five-second timeout and
retries up to three times, the status bar shows a bolt while the PMU reports
charging, the EPUB manifest ceiling rises to 2048 entries with the TOC reorder
buffer in PSRAM, and WiFi connect logging is quieted.
It retains the 0.1.3 clock fallback, brightness, panel rail and cover-retry
work, the 0.1.2 large-shelf/login, prefetch, TOC timeout and WiFi wake fixes,
plus the keyboard fixes and one-shot/date-range alarms.
Build and package outputs are written under `build/native-usb/`.
On 2026-11-02 the device was updated to a
later local 0.1.4 candidate with the internal charging bolt, charge detection
fixes and WiFi policy changes. Full readback and startup health passed; the
user confirmed the charging icon and unplug/replug behavior. This candidate
has not replaced the published package. See the
[device test record](native/docs/CHARGING-WIFI-DEVICE-20261102.md).

A subsequent local 0.1.4 candidate fixes EPUB inline-image decoding under
fragmented PSRAM and restores immediate rail shutdown after reader updates.
It has passed full readback and startup health. The user confirmed image
rendering, speed, static-page behavior, body-width sizing and long-image panning.
The bounded on-device results are tracked in the
[reader fix record](native/docs/EPUB-INLINE-IMAGES-20261102.md).

The WeRead cache failure for long text metadata is traced to the per-string
JSON limit. A bounded metadata scan and regression tests are recorded in the
[WeRead investigation](native/docs/WEREAD-LONG-CONTENT-20261102.md).

The next local candidate adds on-demand WeRead chapters with three-chapter
prefetch and removes the packer's 512-chapter policy limit. Source, boundaries
and validation are tracked in the
[chapter-reading record](native/docs/WEREAD-ONLINE-20261102.md).
Long-press menus retain whole-book caching alongside reading and notes. The
follow-up [cache/cancellation record](native/docs/WEREAD-CACHE-CANCEL-20261103.md)
covers bounded network waits, cancellation checkpoints and local packing measurements.

See [0.1.4 version notes](native/docs/METALIO-0.1.4.md),
[application documentation](native/README.md),
[board mapping and acceptance list](native/docs/METALIO-PORT.md), and
[upstream provenance](upstream-sources.json). Reference checkouts remain read-only;
upstream licenses and notices are retained.
