#!/usr/bin/env python3
"""Generate a 50-word test bank (real definitions, placeholder audio) and pack it.

    python3 vocab-kit/tools/make_test_bank.py --out build/vocab-test

Produces <out>/bank.csv, <out>/audio-src/<word>.mp3 and the packed SD bundle in
<out>/sd (.thingsos/vocab layout). Replace bank.csv + audio with your own list
later; the packing step is identical.
"""
from __future__ import annotations

import argparse
import csv
import shutil
import subprocess
import sys
from pathlib import Path

WORDS = [
    ("abandon", "/əˈbændən/", "v.", "放弃；抛弃", "They abandoned the plan.", "他们放弃了计划。", "a-band-on", "L1"),
    ("abstract", "/ˈæbstrækt/", "adj.", "抽象的", "Abstract ideas are hard.", "抽象概念很难。", "abs-tract", "L1"),
    ("academic", "/ˌækəˈdemɪk/", "adj.", "学术的", "Academic writing takes practice.", "学术写作需要练习。", "academy", "L1"),
    ("accompany", "/əˈkʌmpəni/", "v.", "陪伴", "She accompanied me home.", "她陪我回家。", "ac-company", "L1"),
    ("accumulate", "/əˈkjuːmjəleɪt/", "v.", "积累", "Dust accumulates quickly.", "灰尘积累得很快。", "ac-cumul", "L1"),
    ("accurate", "/ˈækjərət/", "adj.", "准确的", "The data is accurate.", "数据是准确的。", "ac-cur", "L1"),
    ("achieve", "/əˈtʃiːv/", "v.", "实现；达到", "She achieved her goal.", "她实现了目标。", "a-chieve", "L1"),
    ("acquire", "/əˈkwaɪə/", "v.", "获得", "He acquired new skills.", "他获得了新技能。", "ac-quire", "L1"),
    ("adapt", "/əˈdæpt/", "v.", "适应", "Plants adapt to light.", "植物适应光照。", "ad-apt", "L1"),
    ("adequate", "/ˈædɪkwət/", "adj.", "足够的", "The supply is adequate.", "供应是足够的。", "ad-equate", "L1"),
    ("adjust", "/əˈdʒʌst/", "v.", "调整", "Adjust the volume.", "调整音量。", "ad-just", "L1"),
    ("admire", "/ədˈmaɪə/", "v.", "钦佩", "I admire her courage.", "我钦佩她的勇气。", "ad-mire", "L1"),
    ("adopt", "/əˈdɒpt/", "v.", "采用；收养", "They adopted a new method.", "他们采用了新方法。", "ad-opt", "L1"),
    ("advance", "/ədˈvɑːns/", "v.", "前进；促进", "Science advances quickly.", "科学进步很快。", "ad-vance", "L1"),
    ("advocate", "/ˈædvəkeɪt/", "v.", "提倡", "She advocates reform.", "她提倡改革。", "ad-voc", "L2"),
    ("affect", "/əˈfekt/", "v.", "影响", "Weather affects mood.", "天气影响情绪。", "af-fect", "L2"),
    ("afford", "/əˈfɔːd/", "v.", "负担得起", "We cannot afford it.", "我们负担不起。", "af-ford", "L2"),
    ("aggressive", "/əˈɡresɪv/", "adj.", "好斗的；积极进取的", "An aggressive strategy.", "激进的策略。", "ag-gress", "L2"),
    ("alarm", "/əˈlɑːm/", "n.", "警报；闹钟", "The alarm rang at six.", "闹钟六点响了。", "a-larm", "L2"),
    ("alter", "/ˈɔːltə/", "v.", "改变", "Nothing altered the plan.", "没有什么改变计划。", "al-ter", "L2"),
    ("ambition", "/æmˈbɪʃən/", "n.", "雄心", "Her ambition is clear.", "她的雄心很明显。", "ambi-tion", "L2"),
    ("analyze", "/ˈænəlaɪz/", "v.", "分析", "Analyze the results.", "分析结果。", "ana-lyze", "L2"),
    ("ancient", "/ˈeɪnʃənt/", "adj.", "古老的", "An ancient city remains.", "一座古城留存。", "anci-ent", "L2"),
    ("annual", "/ˈænjuəl/", "adj.", "年度的", "The annual report is out.", "年度报告已发布。", "ann-ual", "L2"),
    ("anxiety", "/æŋˈzaɪəti/", "n.", "焦虑", "Exam anxiety is common.", "考试焦虑很常见。", "anx-iety", "L2"),
    ("apparent", "/əˈpærənt/", "adj.", "明显的", "The reason is apparent.", "原因很明显。", "ap-parent", "L2"),
    ("appeal", "/əˈpiːl/", "v.", "吸引；呼吁", "The idea appeals to me.", "这个想法吸引我。", "ap-peal", "L2"),
    ("appreciate", "/əˈpriːʃieɪt/", "v.", "欣赏；感激", "I appreciate your help.", "我感激你的帮助。", "ap-preci", "L2"),
    ("approach", "/əˈprəʊtʃ/", "n.", "方法", "A new approach works.", "新方法奏效。", "ap-proach", "L2"),
    ("appropriate", "/əˈprəʊpriət/", "adj.", "合适的", "Wear appropriate clothes.", "穿合适的衣服。", "ap-propri", "L2"),
    ("approve", "/əˈpruːv/", "v.", "批准", "The board approved it.", "董事会批准了它。", "ap-prove", "L2"),
    ("argue", "/ˈɑːɡjuː/", "v.", "争论；主张", "They argue about money.", "他们为钱争吵。", "ar-gue", "L2"),
    ("arrange", "/əˈreɪndʒ/", "v.", "安排", "Arrange the meeting.", "安排会议。", "ar-range", "L2"),
    ("assess", "/əˈses/", "v.", "评估", "Assess the risk first.", "先评估风险。", "as-sess", "L2"),
    ("assign", "/əˈsaɪn/", "v.", "分配", "Assign each task.", "分配每项任务。", "as-sign", "L2"),
    ("assume", "/əˈsjuːm/", "v.", "假设；承担", "Do not assume it works.", "别假设它有效。", "as-sume", "L2"),
    ("attach", "/əˈtætʃ/", "v.", "附上", "Attach the file.", "附上文件。", "at-tach", "L2"),
    ("attempt", "/əˈtempt/", "n.", "尝试", "One more attempt.", "再试一次。", "at-tempt", "L2"),
    ("attitude", "/ˈætɪtjuːd/", "n.", "态度", "A positive attitude helps.", "积极态度有帮助。", "atti-tude", "L2"),
    ("attract", "/əˈtrækt/", "v.", "吸引", "The show attracts crowds.", "演出吸引人群。", "at-tract", "L2"),
    ("authority", "/ɔːˈθɒrəti/", "n.", "权威；当局", "The local authority agreed.", "当地政府同意了。", "author-ity", "L3"),
    ("available", "/əˈveɪləbl/", "adj.", "可用的", "Is the room available?", "房间可用吗？", "a-vail", "L3"),
    ("aware", "/əˈweə/", "adj.", "意识到的", "Be aware of the risk.", "注意风险。", "a-ware", "L3"),
    ("balance", "/ˈbæləns/", "n.", "平衡", "Keep your balance.", "保持平衡。", "bal-ance", "L3"),
    ("benefit", "/ˈbenɪfɪt/", "n.", "好处", "Exercise has benefits.", "锻炼有好处。", "bene-fit", "L3"),
    ("budget", "/ˈbʌdʒɪt/", "n.", "预算", "The budget is tight.", "预算紧张。", "bud-get", "L3"),
    ("campaign", "/kæmˈpeɪn/", "n.", "运动；活动", "The campaign succeeded.", "活动成功了。", "camp-aign", "L3"),
    ("capacity", "/kəˈpæsəti/", "n.", "容量；能力", "The hall has capacity.", "大厅有容量。", "cap-acity", "L3"),
    ("challenge", "/ˈtʃælɪndʒ/", "n.", "挑战", "Face the challenge.", "面对挑战。", "chal-lenge", "L3"),
    ("circumstance", "/ˈsɜːkəmstəns/", "n.", "情况", "Under these circumstances.", "在这种情况下。", "circum-stance", "L3"),
]

COLUMNS = ["word", "phonetic", "pos", "meaning", "example_en", "example_zh", "root", "group", "derived", "phrase"]


def placeholder_audio(path: Path) -> bool:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return False
    result = subprocess.run(
        [ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", "anullsrc=r=24000:cl=mono", "-t", "0.6",
         "-ac", "1", "-ar", "24000", "-b:a", "32k", str(path)],
        capture_output=True,
    )
    return result.returncode == 0 and path.is_file()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path("build/vocab-test"))
    args = parser.parse_args()
    out = args.out
    audio_src = out / "audio-src"
    audio_src.mkdir(parents=True, exist_ok=True)

    csv_path = out / "bank.csv"
    extras = {
        "achieve": ("achievement n. 成就；achievable adj. 可实现的", "achieve a goal 实现目标"),
        "academic": ("academy n. 学院；academically adv. 学术上", "academic writing 学术写作"),
        "accompany": ("accompaniment n. 伴随物", "be accompanied by 由…陪同"),
        "accumulate": ("accumulation n. 积累；accumulative adj. 累积的", "accumulate wealth 积累财富"),
        "adapt": ("adaptation n. 适应；adaptive adj. 适应性的", "adapt to 适应"),
    }
    rows = [list(row) + list(extras.get(row[0], ("", ""))) for row in WORDS]
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(COLUMNS)
        writer.writerows(rows)

    generated = 0
    for row in WORDS:
        if placeholder_audio(audio_src / f"{row[0]}.mp3"):
            generated += 1
    print(f"wrote {csv_path} ({len(WORDS)} words), placeholder audio: {generated}")

    sd = out / "sd" / ".thingsos" / "vocab"
    cmd = [sys.executable, str(Path(__file__).with_name("vocab_pack.py")),
           "--csv", str(csv_path), "--audio-dir", str(audio_src), "--out", str(sd)]
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
