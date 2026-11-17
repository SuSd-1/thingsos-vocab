# 快速开始

目标：在 Metalio E-Ink4-Plus（ESP32-S31，16 MB Flash）上跑起背单词应用，
并写入一份可用的词库与发音。

## 0. 前置条件

| 项目 | 要求 |
|---|---|
| 设备 | Metalio E-Ink4-Plus（ESP32-S31），已装 ThingsOS 0.2.x |
| 电脑 | macOS / Linux，能访问串口（`/dev/cu.usbmodem*`） |
| ESP-IDF | v6.1（`idf.py --preview`，target `esp32s31`） |
| 词库 | `bank.bin` + `audio/<id>.mp3` + `audio/<id>_ex.mp3` |

## 1. 构建

```bash
git clone https://github.com/SuSd-1/thingsos-vocab.git
cd thingsos-vocab

# 主机测试（不需要设备）
cmake -S native -B build/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --target thingsos_vocab_test -j
./build/host/thingsos_vocab_test

# 设备固件
. ~/esp/esp-idf/export.sh
cd native/firmware
idf.py --preview -B ../../build/native-idf -DTHINGSOS_BUILD_VERSION=0.2.0-vocab build
```

产物：`build/native-idf/ThingsOS_native.bin`。

## 2. 备份（强烈建议）

```bash
bash vocab-kit/scripts/backup-flash.sh /dev/cu.usbmodem101
```

整片 16 MiB 备份，出问题可 `esptool write-flash 0x0 <备份>.bin` 恢复。

## 3. 只刷主应用分区

```bash
esptool --chip esp32s31 -p /dev/cu.usbmodem101 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash 0x220000 build/native-idf/ThingsOS_native.bin
```

> 不要写整片，除非你在恢复备份。刷完只复位一次，等 30 秒；连续复位会触发
> ThingsOS 的引导保护（3 次未确认启动 → 进入 rescue）。

## 4. 写入词库与发音

1. 设备上 **设置 → USB 直连模式 → 开启**；
2. 电脑上出现 `NO NAME` 卷；
3. 拷贝：

```bash
mkdir -p "/Volumes/NO NAME/.thingsos/vocab"
cp bank.bin "/Volumes/NO NAME/.thingsos/vocab/"
mkdir -p "/Volumes/NO NAME/.thingsos/vocab/audio"
cp audio/*.mp3 "/Volumes/NO NAME/.thingsos/vocab/audio/"
```

4. 推出磁盘，设备上关闭 USB 直连。

目录结构：

```
/sdcard/.thingsos/vocab/
├── bank.bin            词库
├── audio/0.mp3         单词发音（按词库 id 命名）
├── audio/0_ex.mp3      例句发音
├── state.dat           学习进度（自动生成）
├── journal.dat         答题日志
├── plan.dat            当前组/轮计划
├── time.dat            学习时长
└── config.bin          设置
```

## 5. 首次使用

- 首页：**学习**（今日还需学的新词数）/ **复习**（今日还需复习数）；
- 点卡片开始；右上角齿轮进入设置；
- 底部「首页 / 我的 / 设置」切换；
- 学习按 10 词一组、三轮推进；复习按 10 词一组单轮。

## 常见问题

| 现象 | 处理 |
|---|---|
| 提示「词库未就绪」 | SD 未挂载或 `bank.bin` 路径不对 |
| 文字有空洞 | 词库文本含设备字体没有的字（繁体/部首），重新导入并做简体归一 |
| 没声音 | 检查 `audio/<id>.mp3` 是否存在、设备音量、蓝牙是否占用音频 |
| 点按钮无反应 | 先看是否处于「回看上一个词」状态，再点一次图标返回 |
| 无法刷机 | 设备休眠时先唤醒；仍不行长按电源键关机再开 |
