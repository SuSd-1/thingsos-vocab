# 跟随上游 ThingsOS 升级

本项目基于 ThingsOS `0.2.0`，改动分两类：

| 类型 | 内容 | 升级成本 |
| --- | --- | --- |
| 新增（外挂） | `native/src/vocab*.c`、`native/firmware/main/vocab_ui.inc`、`native/include/thingsos/vocab.h`、`vocab-kit/`、`tests/` | 低，基本不冲突 |
| 修改（OS 内部） | 刷新策略、控制中心、实体键、启动器图标、显示设置 | 高，需要解冲突 |

## 冲突热点文件

```
native/firmware/components/thingsos_board/board.c
native/firmware/components/thingsos_board/board.h
native/firmware/components/thingsos_board/panel_waveform.c
native/firmware/components/thingsos_board/panel_waveform.h
native/include/thingsos/refresh_policy.h
native/firmware/main/main.c
native/firmware/main/alignment.inc
native/firmware/main/apps_controls.inc
native/firmware/main/vocab_ui.inc
native/include/thingsos/vocab.h
native/src/vocab.c
```

## 升级步骤

```bash
# 1. 新上游放到 references/ 或按原脚本导入
vocab-kit/scripts/import-upstream.sh <新上游目录>

# 2. 新分支，逐个重放补丁
git checkout -b upstream-<版本> upstream/<版本>
git am patches/*.patch

# 3. 有冲突时按文件处理：
#    - board.c 的 present() 分支顺序与波形选择
#    - refresh_policy.h 的清理频率
#    - main.c 的按键派发与启动器表
#    - apps_controls.inc 的控制中心命中区与图标
# 4. 构建 + 主机测试
./build/native-host/thingsos_vocab_test
# 5. 刷机验证：图标、首页、三档显示刷新、控制中心、四选一/撤回
# 6. 重建补丁序列并打标签
git format-patch upstream/<版本>..HEAD -o patches
git tag "thingsos-<版本>-vocab-v<x.y.z>"
```

## 发布命名

```
标题：ThingsOS v<上游版本> Vocab v<应用版本>
例如：ThingsOS v0.2.0 Vocab v0.0.1
```

软件包只包含 **app 镜像**（刷 `0x220000`），前提是设备已刷入对应的原版 ThingsOS。

## 验证清单

- [ ] 启动健康（display / font / touch）
- [ ] 启动器图标正常，点击进入背单词
- [ ] 首页学习/复习可点，实体键上页=学习、下页=复习
- [ ] 系统显示三档：快速（DU）、均衡（裁剪 GL16 差分）、清晰（全表 GC16 每页清理）
- [ ] 控制中心亮度/音量/冷暖加减与震动
- [ ] 四选一、第二轮提示、第三轮无例句
- [ ] 撤回改判与返回
- [ ] 体积在预算内：`vocab-kit/scripts/check-size.sh`
