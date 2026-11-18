# 给 Agent 的安装提示词（可整段复制）

```text
帮我在这台设备上安装「ThingsOS Metalio v0.2.0 Vocab」固件，仓库地址：

https://github.com/SuSd-1/thingsos-vocab

【设备前提，请先确认，不满足就停下来问我】
1. 设备是「无名科技 Metalio E-Ink4-Plus」（ESP32-S31，16MB Flash）。
2. 设备已经刷过原版 ThingsOS Metalio v0.2.0（含引导与分区）。
   如果没刷过原版，请不要继续，先告诉我。
3. 电脑能通过 USB 看到串口（macOS 是 /dev/cu.usbmodem*，Windows 是 COMx）。

【安装步骤】
1. 克隆仓库：
   git clone --depth 1 https://github.com/SuSd-1/thingsos-vocab.git
   cd thingsos-vocab
2. 运行一键安装（把 <PORT> 换成实际串口）：
   ./vocab-kit/scripts/install-release.sh --yes --port <PORT>
   如果脚本说找不到 esptool，请使用 ESP-IDF 6.1 自带的 python -m esptool。
3. 等待脚本输出：
   boot health: confirmed (display / font / touch)
   才算安装成功。

【注意事项，必须遵守】
- 只允许刷 0x220000 的 app 分区；绝对不要刷 0x0，也不要执行 erase-flash。
- 刷完不要连续复位设备：连续 3 次未确认启动会进入救援系统。
- 如果看到救援系统（rescue）：彻底断电（拔 USB）等 3 秒再上电一次即可，不要立刻重刷。
- 刷机前如果 SD 卡可读，先备份 SD 上的 .thingsos/vocab 目录。

【词库】
- 如果我已经有词库：把 bank.bin 与 audio/ 放到 SD 卡
  /sdcard/.thingsos/vocab/ 下（访达里按 Cmd+Shift+. 显示隐藏文件夹）。
- 如果我给你 Excel 词书：运行
  ./vocab-kit/scripts/import-wordbook.sh <我的词书.xlsx> --jobs 4
  或加 --to-sd /Volumes/<SD卡名> 直接复制到 SD 卡。
- 如果暂时没有词库：先用仓库 samples/ 里的 10 词样例验证能进学习页。

【完成标准】
- 脚本输出 boot health: confirmed；
- 设备重启后能进入「背单词」应用；
- 首页显示学习/复习数量；
- 点进学习页能看到单词和选项。

【过程中请告诉我】
- 实际使用的串口；
- 每一步的输出摘要；
- 如果失败，贴出完整报错，不要自行反复重试。
```
