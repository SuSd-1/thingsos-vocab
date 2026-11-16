/* SPDX-License-Identifier: Apache-2.0
 * Word cards: home, learning ladder (quiz -> example -> bare word) and review.
 * All canvas/input state stays on the UI task; every screen uses the shared
 * header / rule / outlined-button language of the launcher.
 */
#include "thingsos/vocab.h"

#define VOCAB_UX_MINUTES_2020 26297280u
#define VOCAB_OPTION_ROWS 4

typedef enum {
    VOCAB_SCREEN_HOME = 0,
    VOCAB_SCREEN_LEARN,
    VOCAB_SCREEN_REVIEW,
    VOCAB_SCREEN_DONE,
} vocab_screen_t;

static struct {
    bool started, failed;
    vocab_screen_t screen;
    vocab_card_t card;
    vocab_card_stage_t stage;
    vocab_stats_t stats;
    vocab_config_t config;
    bool revealed;
    char options[VOCAB_OPTION_ROWS][VOCAB_MEANING_BYTES];
    uint8_t option_count, answer_at;
    bool answered, answer_ok;
    bool booster;
    uint32_t learned_before;
} vu;

static uint32_t vocab_now_min(void) {
    uint32_t seconds = 0;
    if (!thingsos_board_time(&seconds)) return 0;
    uint32_t minutes = seconds / 60u;
    return minutes > VOCAB_UX_MINUTES_2020 ? minutes - VOCAB_UX_MINUTES_2020 : 0u;
}

static void vocab_button(int x, int y, int w, int h, const char* text) {
    app_outline(x, y, w, h);
    centered_label(text, x, y + h / 2 + 11, 30, w);
}

static void vocab_bar(int y, unsigned done, unsigned total) {
    thingsos_rect(&app.canvas, 36, y, 612, 18, 0);
    thingsos_rect(&app.canvas, 38, y + 2, 608, 14, 235);
    if (total) {
        unsigned width = (unsigned)((uint64_t)608 * done / total);
        if (width) thingsos_rect(&app.canvas, 38, y + 2, (int)width, 14, 120);
    }
}

static void vocab_build_quiz(void) {
    char distractors[3][VOCAB_MEANING_BYTES];
    if (!vocab_distractors(vu.card.id, distractors)) {
        snprintf(distractors[0], sizeof(distractors[0]), "—");
        snprintf(distractors[1], sizeof(distractors[1]), "—");
        snprintf(distractors[2], sizeof(distractors[2]), "—");
    }
    vu.option_count = VOCAB_OPTION_ROWS;
    vu.answer_at = (uint8_t)(vu.card.id % VOCAB_OPTION_ROWS);
    unsigned k = 0;
    for (unsigned i = 0; i < VOCAB_OPTION_ROWS; i++) {
        if (i == vu.answer_at)
            snprintf(vu.options[i], sizeof(vu.options[i]), "%s", vu.card.meaning);
        else
            snprintf(vu.options[i], sizeof(vu.options[i]), "%s", distractors[k++ % 3]);
    }
    vu.answered = false;
    vu.answer_ok = false;
}

static bool vocab_load_next(void) {
    vu.revealed = false;
    vu.answered = false;
    if (!vocab_next(&vu.card, &vu.stage)) {
        vu.screen = VOCAB_SCREEN_DONE;
        return false;
    }
    if (vu.stage == VOCAB_STAGE_QUIZ) vocab_build_quiz();
    char audio[256];
    if (vu.config.auto_audio && vocab_audio_path(vu.card.id, audio, sizeof(audio)))
        thingsos_audio_play(audio, 0);
    app.force_refresh = true;
    return true;
}

static bool vocab_start(void) {
    if (vu.started) return true;
    uint32_t now = vocab_now_min();
    vocab_config_load("/sdcard", &vu.config);
    if (!vocab_open("/sdcard", now)) {
        vu.failed = true;
        vu.started = true;
        return false;
    }
    vu.started = true;
    vu.failed = false;
    vu.screen = VOCAB_SCREEN_HOME;
    vocab_stats(now, &vu.stats);
    return true;
}

static void vocab_stop(void) {
    if (!vu.started) return;
    thingsos_audio_stop();
    vocab_close(vocab_now_min());
    vu.started = false;
    vu.failed = false;
}

static void vocab_start_session(bool learn) {
    uint32_t now = vocab_now_min();
    vu.booster = false;
    vu.screen = learn ? VOCAB_SCREEN_LEARN : VOCAB_SCREEN_REVIEW;
    vocab_begin_session(now, &vu.config, learn ? 1 : 2);
    vocab_stats(now, &vu.stats);
    vu.learned_before = vocab_learned_count();
    if (!vocab_load_next()) app.force_refresh = true;
}

static void vocab_start_booster(void) {
    uint32_t now = vocab_now_min();
    vu.booster = true;
    vu.screen = VOCAB_SCREEN_LEARN;
    vocab_begin_session(now, &vu.config, 3);
    vocab_stats(now, &vu.stats);
    vu.learned_before = vocab_learned_count();
    if (!vocab_load_next()) {
        vu.screen = VOCAB_SCREEN_REVIEW;
        vocab_begin_session(now, &vu.config, 2);
        vocab_load_next();
    }
}

static void vocab_after_answer(vocab_grade_t grade) {
    uint32_t now = vocab_now_min();
    const uint32_t learned_before = vocab_learned_count();
    if (!vocab_answer(now, grade)) return;
    const bool graduated = vocab_learned_count() > learned_before;
    vocab_stats(now, &vu.stats);

    if (vu.screen == VOCAB_SCREEN_REVIEW) {
        if (grade == VOCAB_AGAIN || grade == VOCAB_HARD) {
            vocab_start_booster();
            return;
        }
        vocab_load_next();
        return;
    }
    /* Learning ladder. */
    if (graduated && vu.booster) {
        vu.booster = false;
        vu.screen = VOCAB_SCREEN_REVIEW;
        vocab_begin_session(now, &vu.config, 2);
        vocab_stats(now, &vu.stats);
        vocab_load_next();
        return;
    }
    if (graduated) {
        vocab_load_next();
        return;
    }
    vocab_load_next();
}

static void vocab_draw_home(void) {
    header("背单词");
    char line[128];
    snprintf(line, sizeof(line), "连续 %u 天 · 今日新词 %u · 今日复习 %u",
             (unsigned)vu.stats.streak, (unsigned)vu.stats.new_today,
             (unsigned)vu.stats.reviews_today);
    label(line, 36, 210, 24, 612);

    const uint32_t total = vocab_count();
    const uint32_t learned = vocab_learned_count();
    const uint32_t fresh = vocab_new_count();
    thingsos_rect(&app.canvas, 36, 262, 612, 1, 0);
    label("学习进度", 36, 320, 28, 300);
    snprintf(line, sizeof(line), "%u / %u", (unsigned)learned, (unsigned)total);
    thingsos_font_size(&app.font, 26);
    thingsos_label_right(&app.font, &app.canvas, line, 36, 320, 612);
    vocab_bar(342, learned, total);
    snprintf(line, sizeof(line), "待学 %u · 已完成 %u%%", (unsigned)fresh,
             total ? (unsigned)((uint64_t)learned * 100 / total) : 0u);
    label(line, 36, 390, 22, 612);

    label("复习进度", 36, 470, 28, 300);
    snprintf(line, sizeof(line), "%u / %u", (unsigned)vu.stats.reviews_today,
             (unsigned)vu.stats.due_today);
    thingsos_font_size(&app.font, 26);
    thingsos_label_right(&app.font, &app.canvas, line, 36, 470, 612);
    vocab_bar(492, vu.stats.reviews_today, vu.stats.due_today);
    snprintf(line, sizeof(line), "到期 %u 词", (unsigned)vu.stats.due_today);
    label(line, 36, 540, 22, 612);

    vocab_button(36, 640, 612, 120, "开 始 学 习");
    vocab_button(36, 800, 612, 120, "开 始 复 习");
    label("学习：4 选 1 → 看例句回忆 → 只看单词回忆", 36, 980, 20, 612);
    label("复习：认识 / 模糊 / 不认识，模糊与不认识会回到学习三关", 36, 1015, 20, 612);
    nav();
}

static void vocab_draw_quiz(void) {
    thingsos_rect(&app.canvas, 36, 236, 612, 1, 0);
    label(vu.card.word, 36, 320, 56, 612);
    if (vu.card.phonetic[0]) label(vu.card.phonetic, 36, 364, 24, 612);
    label("选出正确的中文释义", 36, 424, 24, 612);

    for (unsigned i = 0; i < vu.option_count; i++) {
        const int y = 470 + (int)i * 126;
        const bool marked = vu.answered && (uint8_t)i == vu.answer_at;
        app_outline(36, y, 612, 106);
        if (marked) app_outline(40, y + 4, 604, 98);
        char number[8];
        snprintf(number, sizeof(number), "%c", 'A' + (int)i);
        label(number, 56, y + 62, 30, 60);
        label(vu.options[i], 110, y + 62, 27, 520);
    }
    if (vu.answered) {
        label(vu.answer_ok ? "答对了，进入下一关" : "再认一下：正确释义已标出", 36, 1030, 24, 612);
        vocab_button(36, 1060, 612, 80, "继 续");
    }
    nav();
}

static void vocab_draw_card(void) {
    label(vu.screen == VOCAB_SCREEN_REVIEW ? "复习" : "学习", 36, 210, 24, 612);
    char line[128];
    snprintf(line, sizeof(line), "今日 %u · 待复习 %u · 连续 %u 天",
             (unsigned)vu.stats.new_today, (unsigned)vu.stats.due_today,
             (unsigned)vu.stats.streak);
    thingsos_font_size(&app.font, 22);
    thingsos_label_right(&app.font, &app.canvas, line, 36, 210, 612);
    thingsos_rect(&app.canvas, 36, 236, 612, 1, 0);

    label(vu.card.word, 36, 340, 60, 612);
    if (vu.card.phonetic[0]) label(vu.card.phonetic, 36, 390, 24, 612);

    if (vu.stage == VOCAB_STAGE_RECALL && vu.card.example_en[0]) {
        label(vu.card.example_en, 36, 450, 26, 612);
        if (vu.card.example_zh[0]) label(vu.card.example_zh, 36, 494, 24, 612);
    }

    if (vu.revealed) {
        thingsos_rect(&app.canvas, 36, 540, 612, 1, 0);
        label(vu.card.meaning, 36, 620, 44, 612);
        if (vu.stage != VOCAB_STAGE_RECALL && vu.card.example_en[0])
            label(vu.card.example_en, 36, 700, 24, 612);
        if (vu.card.root[0]) {
            char root_line[128];
            snprintf(root_line, sizeof(root_line), "词根：%s", vu.card.root);
            label(root_line, 36, 760, 20, 612);
        }
        vocab_button(36, 980, 190, 96, "不认识");
        vocab_button(247, 980, 190, 96, "模糊");
        vocab_button(458, 980, 190, 96, "认识");
    } else {
        centered_label(vu.stage == VOCAB_STAGE_RECALL ? "回想它的中文意思，再点按核对"
                                                      : "只想这个单词的意思，再点按核对",
                       36, 640, 28, 612);
        vocab_button(36, 980, 612, 96, "显示释义");
    }
    nav();
}

static void vocab_draw_done(void) {
    header("背单词");
    label(vu.screen == VOCAB_SCREEN_DONE ? "今日完成" : "", 36, 300, 40, 612);
    char line[96];
    snprintf(line, sizeof(line), "新词 %u · 复习 %u", (unsigned)vu.stats.new_today,
             (unsigned)vu.stats.reviews_today);
    label(line, 36, 380, 28, 612);
    snprintf(line, sizeof(line), "连续 %u 天 · 已学 %u / %u", (unsigned)vu.stats.streak,
             (unsigned)vocab_learned_count(), (unsigned)vocab_count());
    label(line, 36, 430, 26, 612);
    vocab_button(36, 560, 612, 110, "返回主页");
    vocab_button(36, 700, 612, 110, "返回应用");
    nav();
}

static bool vocab_draw(void) {
    if (app.page != THINGSOS_UI_VOCAB) {
        if (vu.started) vocab_stop();
        return false;
    }
    if (!vocab_start()) {
        header("背单词");
        label("词库未就绪", 36, 400, 34, 612);
        label("请把 bank.bin 放进 /sdcard/.thingsos/vocab/", 36, 450, 24, 612);
        label("用 vocab-kit/tools/vocab_pack.py 生成", 36, 494, 24, 612);
        char line[96];
        snprintf(line, sizeof(line), "SD 卡: %s", media() ? "已挂载" : "未挂载");
        label(line, 36, 540, 24, 612);
        label("或按 HOME 键返回应用页", 36, 600, 22, 612);
        vocab_button(36, 640, 612, 96, "返回应用");
        nav();
        return true;
    }

    switch (vu.screen) {
        case VOCAB_SCREEN_HOME: vocab_draw_home(); break;
        case VOCAB_SCREEN_LEARN:
        case VOCAB_SCREEN_REVIEW:
            if (vu.stage == VOCAB_STAGE_QUIZ) vocab_draw_quiz();
            else vocab_draw_card();
            break;
        default: vocab_draw_done(); break;
    }
    return true;
}

static bool vocab_tap(int x, int y) {
    if (app.page != THINGSOS_UI_VOCAB) return false;
    if (y >= 1098) return false; /* bottom navigation stays with the shell */
    if (vu.failed) {
        if (y >= 640 && y < 760) app_enter(THINGSOS_UI_APPS);
        return true;
    }

    if (vu.screen == VOCAB_SCREEN_HOME) {
        if (y >= 640 && y < 760) { vocab_start_session(true); return true; }
        if (y >= 800 && y < 920) { vocab_start_session(false); return true; }
        return true;
    }

    if (vu.screen == VOCAB_SCREEN_DONE) {
        if (y >= 560 && y < 670) { vu.screen = VOCAB_SCREEN_HOME; app.dirty = true; return true; }
        if (y >= 700 && y < 810) { app_enter(THINGSOS_UI_APPS); return true; }
        return true;
    }

    if (vu.stage == VOCAB_STAGE_QUIZ) {
        if (vu.answered) {
            if (y >= 1060) {
                vocab_after_answer(vu.answer_ok ? VOCAB_GOOD : VOCAB_AGAIN);
            }
            return true;
        }
        if (y >= 470 && y < 470 + 4 * 126) {
            const int index = (y - 470) / 126;
            if (index < (int)vu.option_count) {
                vu.answered = true;
                vu.answer_ok = (uint8_t)index == vu.answer_at;
                app.force_refresh = true;
            }
        }
        return true;
    }

    if (!vu.revealed) {
        vu.revealed = true;
        vocab_reveal();
        char audio[256];
        if (vocab_audio_path(vu.card.id, audio, sizeof(audio)))
            thingsos_audio_play(audio, 0);
        app.dirty = true;
        app.force_refresh = true;
        return true;
    }

    if (y >= 980 && y < 1076) {
        if (x >= 458) { vocab_after_answer(VOCAB_GOOD); return true; }
        if (x >= 247) { vocab_after_answer(VOCAB_HARD); return true; }
        if (x >= 36) { vocab_after_answer(VOCAB_AGAIN); return true; }
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
    if (vu.failed) return false;
    if (vu.screen == VOCAB_SCREEN_HOME) {
        if (action == THINGSOS_KEY_NEXT) vocab_start_session(true);
        else if (action == THINGSOS_KEY_PREVIOUS) vocab_start_session(false);
        return true;
    }
    if (vu.screen == VOCAB_SCREEN_DONE) {
        if (action == THINGSOS_KEY_NEXT || action == THINGSOS_KEY_PREVIOUS) {
            vu.screen = VOCAB_SCREEN_HOME;
            app.dirty = true;
        }
        return true;
    }
    if (vu.stage == VOCAB_STAGE_QUIZ) {
        if (vu.answered) {
            if (action == THINGSOS_KEY_NEXT || action == THINGSOS_KEY_MENU)
                vocab_after_answer(vu.answer_ok ? VOCAB_GOOD : VOCAB_AGAIN);
            return true;
        }
        return true;
    }
    if (action == THINGSOS_KEY_MENU) {
        if (!vu.revealed) {
            vu.revealed = true;
            vocab_reveal();
            app.dirty = app.force_refresh = true;
        }
        return true;
    }
    if (!vu.revealed) {
        vu.revealed = true;
        vocab_reveal();
        app.dirty = app.force_refresh = true;
        return true;
    }
    if (action == THINGSOS_KEY_NEXT) {
        vocab_after_answer(VOCAB_GOOD);
        return true;
    }
    if (action == THINGSOS_KEY_PREVIOUS) {
        vocab_after_answer(VOCAB_AGAIN);
        return true;
    }
    return true;
}
