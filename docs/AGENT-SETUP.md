# Agent 使用指南（环境与依赖）

推荐把本仓库和设备交给 Agent 完成：刷机、导入词书、生成发音、按需改代码。
下面列出 Agent 需要准备的环境，以及每一步的检查命令。

## 1. 固件刷写

| 依赖 | 检查 |
| --- | --- |
| USB 串口 | `ls /dev/cu.usbmodem*`（macOS） |
| esptool（支持 esp32s31） | `esptool --chip esp32s31 version`，或 ESP-IDF 自带的 `python -m esptool` |
| 原版固件 | 设备需已刷 **ThingsOS Metalio v0.2.0** |

一键：

```bash
./vocab-kit/scripts/install-release.sh --yes
```

脚本会下载 Release、校验 SHA256、只写 `0x220000`，并等待 `Startup health confirmed`。

## 2. 词书转换

| 依赖 | 用途 |
| --- | --- |
| Python 3.11 + venv | 运行转换工具 |
| `openpyxl` | 读取 Excel |
| `opencc-python-reimplemented` | 繁体 → 简体 |
| `eng-to-ipa` | 音标 |
| `piper-tts` | 离线发音 |
| `ffmpeg` | 写 MP3 |
| Piper 音色 `en_US-lessac-medium` | 约 60 MB |
| Tatoeba `cmn.txt`（可选） | 中英例句匹配 |

默认路径（可用环境变量覆盖）：

```
VOCAB_VENV=~/.venvs/piper
PIPER_MODEL=~/piper-voices/en_US-lessac-medium.onnx
TATOEBA=~/vocab-data/cmn.txt
```

一键：

```bash
./vocab-kit/scripts/import-wordbook.sh 我的词书.xlsx --jobs 4
# 可选：--no-audio、--to-sd /Volumes/<SD>、--tatoeba <文件>
```

产出目录 `build/book/sd/vocab/`：

```
bank.bin
audio/<id>.mp3
audio/<id>_ex.mp3
```

## 3. 从源码构建（可选）

| 依赖 | 检查 |
| --- | --- |
| ESP-IDF v6.1（含 esp32s31） | `idf.py --version` |
| 组件缓存 | `IDF_COMPONENT_CACHE_PATH` |

```bash
. ~/esp/esp-idf/export.sh
cd native/firmware
idf.py --preview -B ../../build/native-idf -DTHINGSOS_BUILD_VERSION=0.2.0-vocab build
```

## 4. 验收清单

- [ ] 串口出现设备
- [ ] `install-release.sh` 输出 `boot health: confirmed`
- [ ] SD 卡存在 `vocab/bank.bin`
- [ ] 背单词首页显示学习/复习数量
- [ ] 发音可播放（`audio/<id>.mp3`）

## 5. 常见问题

| 现象 | 处理 |
| --- | --- |
| 刷完进救援系统 | 断电 3 秒再上电一次；不要连续复位 |
| `no /dev/cu.usbmodem*` | 换数据线 / 确认 BOOT 模式 |
| esptool 不支持 esp32s31 | 用 ESP-IDF 自带 Python 的 esptool |
| 词库未就绪 | 检查 `bank.bin` 是否在 `vocab/` |
| 例句缺失 | 提供 Tatoeba 语料，或让 Agent 生成后写入 CSV |
