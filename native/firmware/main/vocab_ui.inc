/* SPDX-License-Identifier: Apache-2.0
 * Offline word cards. Same visual language as the rest of the launcher:
 * header(), single-pixel rules, 36 px side margins, outlined buttons.
 */
#include "thingsos/vocab.h"
static struct {
    bool started, ready, failed, revealed, finished;
    vocab_card_t card;
    vocab_card_stage_t stage;
    vocab_stats_t stats;
    vocab_config_t config;
    uint32_t opened_min;
    char audio[256];
    bool audio_ok;
} vocab_ui;

#define VOCAB_UX_MINUTES_2020 26297280u /* Unix minutes at 2020-01-01 */

static uint32_t vocab_now_min(void) {
    uint32_t seconds = 0;
    if (!thingsos_board_time(&seconds)) return vocab_ui.opened_min;
    uint32_t minutes = seconds / 60u;
    return minutes > VOCAB_UX_MINUTES_2020 ? minutes - VOCAB_UX_MINUTES_2020 : 0u;
}

static bool vocab_load_next(void) {
    if (vocab_next(&vocab_ui.card, &vocab_ui.stage)) {
        vocab_ui.revealed = false;
        vocab_ui.finished = false;
        vocab_ui.audio_ok = vocab_audio_path(vocab_ui.card.id, vocab_ui.audio, sizeof(vocab_ui.audio));
        if (vocab_ui.config.auto_audio && vocab_ui.audio_ok)
            thingsos_audio_play(vocab_ui.audio, 0);
        return true;
    }
    vocab_ui.finished = true;
    return false;
}

static bool vocab_start(void) {
    if (vocab_ui.started) return true;
    uint32_t now = vocab_now_min();
    vocab_config_load("/sdcard", &vocab_ui.config);
    if (!vocab_open("/sdcard", now)) {
        vocab_ui.failed = true;
        vocab_ui.started = true;
        return false;
    }
    vocab_begin_day(now, &vocab_ui.config);
    vocab_ui.opened_min = now;
    vocab_ui.started = true;
    vocab_ui.failed = false;
    vocab_stats(now, &vocab_ui.stats);
    vocab_load_next();
    return true;
}

static void vocab_stop(void) {
    if (!vocab_ui.started) return;
    thingsos_audio_stop();
    vocab_close(vocab_now_min());
    vocab_ui.started = false;
    vocab_ui.ready = false;
}

static void vocab_button(int x, int y, int w, int h, const char* text, bool strong) {
    app_outline(x, y, w, h);
    if (strong) thingsos_rect(&app.canvas, x + 3, y + 3, w - 6, h - 6, 0);
    int baseline = y + h / 2 + 12;
    if (strong) {
        thingsos_font_size(&app.font, 30);
        thingsos_label_right(&app.font, &app.canvas, text, x + 12, baseline, w - 24);
    } else {
        centered_label(text, x, baseline, 30, w);
    }
}

static bool vocab_draw(void) {
    if (app.page != THINGSOS_UI_VOCAB) {
        if (vocab_ui.started) vocab_stop();
        return false;
    }
    if (!vocab_start()) {
        header("背单词");
        label("词库未就绪", 36, 420, 34, 612);
        label("请用 vocab_pack.py 生成词库并拷到 SD 卡", 36, 480, 24, 612);
        label("/sdcard/.thingsos/vocab/", 36, 530, 24, 612);
        nav();
        return true;
    }
    if (vocab_ui.finished) {
        vocab_stats(vocab_now_min(), &vocab_ui.stats);
        header("背单词");
        label("今日完成", 36, 360, 40, 612);
        char line[96];
        snprintf(line, sizeof(line), "新词 %u · 复习 %u", (unsigned)vocab_ui.stats.new_today,
                 (unsigned)vocab_ui.stats.reviews_today);
        label(line, 36, 430, 28, 612);
        snprintf(line, sizeof(line), "连续 %u 天 · 待复习 %u", (unsigned)vocab_ui.stats.streak,
                 (unsigned)vocab_ui.stats.due_today);
        label(line, 36, 480, 28, 612);
        vocab_button(36, 640, 612, 96, "返回应用", false);
        nav();
        vocab_ui.ready = true;
        return true;
    }

    header("背单词");
    char line[128];
    const unsigned learned = (unsigned)vocab_ui.stats.new_today;
    snprintf(line, sizeof(line), "今日 %u · 待复习 %u · 连续 %u 天", learned,
             (unsigned)vocab_ui.stats.due_today, (unsigned)vocab_ui.stats.streak);
    label(line, 36, 214, 24, 612);
    thingsos_rect(&app.canvas, 36, 240, 612, 1, 0);

    /* Card */
    thingsos_font_size(&app.font, 52);
    app.font.dither = true;
    thingsos_label_right(&app.font, &app.canvas, vocab_ui.card.word, 36, 430, 612);
    if (vocab_ui.card.phonetic[0]) centered_label(vocab_ui.card.phonetic, 36, 480, 26, 612);

    if (vocab_ui.revealed) {
        thingsos_rect(&app.canvas, 36, 520, 612, 1, 0);
        label(vocab_ui.card.meaning, 36, 580, 32, 612);
        if (vocab_ui.card.example_en[0]) label(vocab_ui.card.example_en, 36, 660, 24, 612);
        if (vocab_ui.card.example_zh[0]) label(vocab_ui.card.example_zh, 36, 700, 24, 612);
        if (vocab_ui.card.root[0]) label(vocab_ui.card.root, 36, 780, 22, 612);
        vocab_button(36, 860, 148, 96, "不认识", false);
        vocab_button(192, 860, 148, 96, "模糊", false);
        vocab_button(348, 860, 148, 96, "认识", true);
        vocab_button(504, 860, 144, 96, "秒答", false);
    } else {
        centered_label(vocab_ui.stage == VOCAB_STAGE_AUDIO ? "听发音，回想释义" : "想一想，再点按看释义",
                       36, 700, 26, 612);
        vocab_button(36, 860, 612, 96, "显示释义", false);
    }
    if (!vocab_ui.audio_ok) label("无发音", 36, 820, 20, 200);
    nav();
    vocab_ui.ready = true;
    return true;
}

static bool vocab_answer_grade(vocab_grade_t grade) {
    uint32_t now = vocab_now_min();
    if (!vocab_answer(now, grade)) return false;
    vocab_stats(now, &vocab_ui.stats);
    vocab_load_next();
    app.dirty = true;
    return true;
}

static bool vocab_tap(int x, int y) {
    if (app.page != THINGSOS_UI_VOCAB) return false;
    if (vocab_ui.failed) {
        if (y >= 640 && y < 740) {
            app_enter(THINGSOS_UI_APPS);
            return true;
        }
        return true;
    }
    if (vocab_ui.finished) {
        if (y >= 640) app_enter(THINGSOS_UI_APPS);
        return true;
    }
    if (!vocab_ui.revealed) {
        vocab_ui.revealed = true;
        vocab_reveal();
        if (vocab_ui.audio_ok) thingsos_audio_play(vocab_ui.audio, 0);
        app.dirty = true;
        return true;
    }
    if (y >= 860 && y < 960) {
        if (x >= 504) return vocab_answer_grade(VOCAB_EASY);
        if (x >= 348) return vocab_answer_grade(VOCAB_GOOD);
        if (x >= 192) return vocab_answer_grade(VOCAB_HARD);
        if (x >= 36) return vocab_answer_grade(VOCAB_AGAIN);
    }
    if (x >= 480 && y >= 760 && y < 850 && vocab_ui.audio_ok) {
        thingsos_audio_play(vocab_ui.audio, 0);
        return true;
    }
    return true;
}

static bool vocab_key(int action) {
    if (app.page != THINGSOS_UI_VOCAB || action == THINGSOS_KEY_NONE) return false;
    if (action == THINGSOS_KEY_HOME) {
        vocab_stop();
        app_enter(THINGSOS_UI_APPS);
        return true;
    }
    if (vocab_ui.failed || vocab_ui.finished) return true;
    if (action == THINGSOS_KEY_MENU) {
        if (!vocab_ui.revealed) {
            vocab_ui.revealed = true;
            vocab_reveal();
            if (vocab_ui.audio_ok) thingsos_audio_play(vocab_ui.audio, 0);
        } else if (vocab_ui.audio_ok) {
            thingsos_audio_play(vocab_ui.audio, 0);
        }
        app.dirty = true;
        return true;
    }
    if (action == THINGSOS_KEY_NEXT) {
        if (!vocab_ui.revealed) {
            vocab_ui.revealed = true;
            vocab_reveal();
            app.dirty = true;
            return true;
        }
        return vocab_answer_grade(VOCAB_GOOD);
    }
    if (action == THINGSOS_KEY_PREVIOUS) {
        if (!vocab_ui.revealed) {
            vocab_ui.revealed = true;
            vocab_reveal();
            app.dirty = true;
            return true;
        }
        return vocab_answer_grade(VOCAB_AGAIN);
    }
    return true;
}
