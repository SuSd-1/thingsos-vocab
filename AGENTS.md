# AGENTS.md — 本仓库的 Agent 操作规范

如果你是被用户用「仓库链接」召来的 Agent，请按本文件执行。

## 目标

在这台 **Metalio E-Ink4-Plus（ESP32-S31）** 上安装本仓库发布的
`ThingsOS Metalio v0.2.0 Vocab` 固件，并帮助用户导入词库。

## 第一步：确认前提（不满足就停止并询问用户）

1. 设备型号是 Metalio E-Ink4-Plus；
2. 设备**已经刷过原版 ThingsOS Metalio v0.2.0**（含引导与分区）；
3. 能看到串口：`ls /dev/cu.usbmodem*`（macOS）或 `COMx`（Windows）。

> 如果设备不是原版 0.2.0，**不要**刷本仓库的 app 包，先告诉用户。

## 第二步：安装固件

```bash
git clone --depth 1 https://github.com/SuSd-1/thingsos-vocab.git
cd thingsos-vocab
./vocab-kit/scripts/install-release.sh --yes --port <PORT>
```

成功判据：脚本输出 `boot health: confirmed (display / font / touch)`。

## 第三步：导入词库（可选，但推荐）

```bash
./vocab-kit/scripts/import-wordbook.sh <用户词书.xlsx> --jobs 4
# 或直接复制到已挂载的 SD：
./vocab-kit/scripts/import-wordbook.sh <用户词书.xlsx> --to-sd /Volumes/<SD>
```

没有词库时，先用 `samples/` 的 10 词样例验证应用能进入学习页。

## 绝对禁止

- 不要刷 `0x0`，不要 `erase-flash`（会清掉引导/NVS/系统设置）。
- 不要连续复位：连续 3 次未确认启动会进入救援系统。
- 遇到救援系统：断电 3 秒再上电一次，不要立刻重刷。
- 不要修改用户的词库内容（释义保持原样）。

## 更多

- 完整可复制提示词：`docs/AGENT-PROMPT.md`
- 环境与依赖清单：`docs/AGENT-SETUP.md`
- 词库数据格式：`docs/VOCAB-DATA.md`
- 升级与补丁：`docs/UPGRADE.md`
