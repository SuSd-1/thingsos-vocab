# mogo 移植同步：数据目录与兼容约定

> 2026-11-20，mogo 侧背单词移植已开始。本文只同步 ThingsOS Vocab 需要配合的
> 一项改动，不改学习逻辑、按键或音频。

## 唯一改动：数据目录

ThingsOS Vocab 目前把数据放在 `/sdcard/.thingsos/vocab/`；mogo 使用
`/sdcard/vocab/` 作为规范目录，并兼容探测旧目录。两边要共用同一份词库和
进度，需要把 ThingsOS 的路径常量改为 `vocab`：

`native/src/vocab.c`：

```c
static void path_of(char *buf, size_t bytes, const char *name) {
    snprintf(buf, bytes, "%s/vocab/%s", g_root, name);
}
```

`ensure_dirs()` 中对应把 `.thingsos/vocab` 改成 `vocab`，音频路径
`.../vocab/audio/%u.mp3` 同步。

数据文件名、格式、学习进度算法均不变；`state.dat` 只绑定词库哈希，不含
绝对路径，所以目录搬迁不会丢进度。

## 迁移

```sh
# 卡上只做一次；改名后 ThingsOS 和 mogo 都指向 /sdcard/vocab
mv /sdcard/.thingsos/vocab /sdcard/vocab
```

如果暂时不重编 ThingsOS，mogo 会按
`/sdcard/vocab` → `/sdcard/.thingsos/vocab` → `/sdcard/mogo/vocab`
的顺序探测，旧目录可以直接用；但规范目录以 `/sdcard/vocab` 为准。

## 不需要同步的部分

- 四键/电源 1 s/3 s 语义：mogo 侧按 ThingsOS 的逻辑复刻，ThingsOS 不改。
- 词库打包、发音生成、音频格式：不变。
- 刷新：mogo 用 `APP_REDRAW_*` 意图，不引入 ThingsOS 三档设置。

## 验证

- 重编后确认 `/sdcard/vocab/bank.bin`、`state.dat`、`audio/` 可读写。
- 旧目录不再写入；新目录进度与旧目录一致。
- 词库哈希变化时按原规则清空进度。
