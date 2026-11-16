# 背单词（word cards）—— ThingsOS Metalio 0.2.0 + 补丁

离线单词卡 + FSRS-6 间隔重复，数据全在 SD 卡。本目录是“个人使用版”工具包：
官方源码 + 我们的补丁，官方发版后重新套用即可。

## 体积账本

| 项目 | 字节 |
|---|---|
| 官方 0.2.0 主应用（macOS 构建基线） | 7,002,240 |
| 加入背单词后 | 7,014,304 |
| **新增** | **+12,064（约 11.8 KiB）** |

上限 120 KiB（`scripts/check-size.sh`），当前余量充足；主应用分区 10.875 MiB。

## 目录

```
vocab-kit/
  scripts/import-upstream.sh   下载/校验官方源码包并打成 upstream/<ver> tag
  scripts/apply-and-build.sh   依赖检查 → 主机测试 → 固件构建 → JPEG 链接门禁 → 体积门禁
  scripts/check-size.sh        与基线比较，超过 120 KiB 失败
  scripts/backup-flash.sh      刷机前 16 MiB 整片备份
  scripts/flash-app.sh         只写主应用分区（0x220000）
  tools/vocab_pack.py          CSV + 音频 → bank.bin + audio/<id>.mp3
  tools/make_test_bank.py      生成 50 词测试包（含占位音频）
  docs/VOCAB.md                本文件
```

## 首次构建

```bash
cd thingsos-vocab
bash vocab-kit/scripts/apply-and-build.sh          # 产物在 build/native-idf/
```

前置：ESP-IDF v6.1（默认 `~/esp/esp-idf`，可用 `IDF_PATH` 覆盖）。构建脚本把
component 缓存放在 `build/component-cache`，不会写 `~/Library/Caches`。

已知事项：
- 官方源码包的 vendor 文件与其自带的 `sources.json` 哈希不一致（发布包重打包导致，
  77 个文件 + 5 个文档缺失），`check_deps.py` 会报告但不阻断，只对缺失的可编译文件报警。
- 源码包内字体文件名被压成 `??????.TTF`，`extract_zip.py` 会还原为 `方正屏显雅宋.TTF`。
- 上游 `wifi_tx_power_test.c` 在 macOS 上重定义 `gets()` 无法编译，构建脚本只编译
  `thingsos_vocab_test` 并跑对应的 CTest。

## 词库

CSV（UTF-8）列：`word, phonetic, pos, meaning, example_en, example_zh, root, group`。
音频命名 `<word>.mp3`（或 wav/m4a，打包时用 ffmpeg 转单声道 24 kHz/32 kbps）。

```bash
python3 vocab-kit/tools/vocab_pack.py \
  --csv my-bank.csv --audio-dir my-audio --out build/my-bank/.thingsos/vocab
```

把 `build/my-bank/.thingsos/vocab/` 整个拷到 SD 卡的 `/sdcard/.thingsos/vocab/`。

50 词测试包：

```bash
python3 vocab-kit/tools/make_test_bank.py --out build/vocab-test
# 产物：build/vocab-test/sd/.thingsos/vocab/{bank.bin,audio/,report.txt}
VOCAB_TEST_ROOT="$PWD/build/vocab-test/sd" ./build/native-host/thingsos_vocab_test
```

## 刷机（需要你显式授权）

```bash
bash vocab-kit/scripts/backup-flash.sh /dev/cu.usbmodem101   # 先备份 16 MiB
# 设备进入 BOOT 模式（设置 → BOOT 刷机，或按住 BOOT 上电）
bash vocab-kit/scripts/flash-app.sh /dev/cu.usbmodem101      # 只写主应用分区
```

## 官方发版后升级

```bash
bash vocab-kit/scripts/import-upstream.sh 0.2.1   # 下载 + 校验 + tag upstream/0.2.1
git rebase upstream/0.2.1                          # 冲突通常只在 main.c / apps_controls.inc 几行
bash vocab-kit/scripts/apply-and-build.sh
bash vocab-kit/scripts/flash-app.sh
git format-patch upstream/0.2.1..vocab -o vocab-kit/patches
```

注意：官方 OTA 会把自制固件覆盖回官方版，建议关闭自动更新；升级后重新套补丁刷回。
数据在 SD，升级不影响进度。回退用官网 USB 整包。

## 应用结构

- `native/src/vocab.c`：词库解析、队列、学习步、持久化（state.dat + journal.dat）
- `native/src/vocab_fsrs.c`：FSRS-6 记忆模型（21 权重）
- `native/firmware/main/vocab_ui.inc`：卡片界面、四档评分、按键
- 集成点：`ui.h` 页面枚举、`apps_controls.inc`（draw/tap/启动器/图标）、`main.c`（include + 按键）
- 数据：`/sdcard/.thingsos/vocab/{bank.bin,audio,state.dat,journal.dat,config.bin,review-log.csv}`
