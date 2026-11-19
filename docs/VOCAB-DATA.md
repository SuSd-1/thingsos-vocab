# 词书数据管线

词库是 SD 卡上的 `bank.bin`，由 CSV 打包而来；发音是 `audio/<id>.mp3`
（例句为 `<id>_ex.mp3`）。全部工具在 `vocab-kit/tools/`。

## CSV 格式（10 列）

| 列 | 必填 | 说明 |
|---|---|---|
| `word` | ✅ | 单词，决定音频文件名与排序 |
| `meaning` | ✅ | 中文释义；每个词性一行（用换行分隔） |
| `phonetic` | 推荐 | 音标；设备字体缺 IPA 字形，显示时会自动转 ASCII |
| `pos` | 可选 | 词性缩写（`n.` `v.` `adj.` `adv.` …） |
| `example_en` / `example_zh` | 可选 | 例句与译文 |
| `root` / `derived` / `phrase` | 可选 | 词根 / 派生 / 短语，答案页按段显示 |
| `group` | 可选 | 预留（分组/List） |

示例见 [`samples/wordbook-sample.csv`](../samples/wordbook-sample.csv)。

## 一键导入（推荐）

```bash
./vocab-kit/scripts/import-wordbook.sh 我的词书.xlsx --jobs 4
```

自动准备 venv、依赖与音色，跑完导入/发音/打包；产出一份可直接拷到 SD 的目录。

## 从 Excel 导入（手动步骤）

```bash
~/.venvs/piper/bin/python vocab-kit/tools/import_xlsx.py \
  --xlsx 单词背诵记录.xlsx \
  --tatoeba ~/vocab-data/cmn.txt \
  --out build/book/bank.csv
```

- 自动补音标（离线 CMU→IPA）；
- 用 Tatoeba 中英对照语料匹配例句（CC-BY 2.0）；
- 康熙部首 / 兼容汉字 / 繁体统一转简体，避免设备缺字。

## 生成发音（Piper，离线）

```bash
python -m venv ~/.venvs/piper && ~/.venvs/piper/bin/pip install piper-tts
mkdir -p ~/piper-voices && cd ~/piper-voices
# 模型：en_US-lessac-medium.onnx / .onnx.json（huggingface: rhasspy/piper-voices）

python3 vocab-kit/tools/make_audio.py \
  --csv build/book/bank.csv --out build/book/audio \
  --model ~/piper-voices/en_US-lessac-medium.onnx --examples --jobs 4
```

输出 `<word>.mp3` 与 `<word>_ex.mp3`，已存在的会跳过（可断点续跑）。

## 打包

```bash
python3 vocab-kit/tools/vocab_pack.py \
  --csv build/book/bank.csv --audio-dir build/book/audio \
  --out build/book/sd/vocab
```

产物：`bank.bin`（v4 索引，40 B/词）+ `audio/<id>.mp3` + `audio/<id>_ex.mp3`。
`bank.bin` 与 `state.dat` 绑定词库哈希：换词库会清空学习进度。

## 例句补全（可选）

Tatoeba 覆盖不到的词，可以由模型批量生成，再用
`merge_examples.py` 合并并校验（原词必须出现、6–14 词、中文非空）：

```bash
python3 vocab-kit/tools/merge_examples.py \
  --csv build/book/bank.csv --examples build/book/batches \
  --out build/book/bank.csv
```

## 字体检查

设备使用系统字体（方正屏显雅宋），只有简体字形。导入后建议做一次覆盖检查：
逐字比对 `meaning/example_zh` 与字体 `cmap`，缺失即说明需要做简体/部首归一。
