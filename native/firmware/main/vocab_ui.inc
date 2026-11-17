/* SPDX-License-Identifier: Apache-2.0
 * Word cards. One word is learned through six screens:
 *   1 quiz (4 meanings)      2 answer view
 *   3 example recall         4 answer view
 *   5 bare-word recall       6 answer view
 * A correct quiz answer jumps straight to screen 2 (no "continue" button).
 * Review reuses screen 5 with 认识 / 模糊 / 忘记; fuzzy or forgotten cards go
 * back through the learning ladder.
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

typedef enum {
    VOCAB_PHASE_QUESTION = 0,
    VOCAB_PHASE_ANSWER,
} vocab_phase_t;

static struct {
    bool started, failed;
    vocab_screen_t screen;
    vocab_phase_t phase;
    vocab_card_t card;
    vocab_card_stage_t stage;
    vocab_stats_t stats;
    vocab_config_t config;
    bool revealed;
    char options[VOCAB_OPTION_ROWS][VOCAB_MEANING_BYTES];
    uint8_t option_count, answer_at;
    bool answered, answer_ok;
    bool booster;
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

/* The bundled CJK font has no IPA glyphs, so map the symbols it lacks to
   readable ASCII instead of drawing empty boxes. */
static const char* vocab_phonetic_text(char* out, size_t bytes, const char* in) {
    static const char* pairs[][2] = {
        {"tʃ", "ch"}, {"dʒ", "j"},  {"ˈ", "'"}, {"ˌ", ","}, {"ː", ":"},
        {"ə", "e"},   {"ɪ", "i"},   {"ʊ", "u"}, {"ʃ", "sh"}, {"ʒ", "zh"},
        {"ŋ", "ng"},  {"θ", "th"},  {"ð", "dh"}, {"ɔ", "o"}, {"ɒ", "o"},
        {"ɑ", "a"},   {"ɜ", "er"},  {"ʌ", "u"}, {"æ", "a"}, {"ɡ", "g"},
    };
    if (!out || bytes == 0) return in;
    size_t used = 0;
    const char* p = in;
    while (*p && used + 1 < bytes) {
        bool matched = false;
        for (unsigned i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
            const size_t n = strlen(pairs[i][0]);
            if (strncmp(p, pairs[i][0], n) == 0) {
                const size_t m = strlen(pairs[i][1]);
                for (size_t k = 0; k < m && used + 1 < bytes; k++) out[used++] = pairs[i][1][k];
                p += n;
                matched = true;
                break;
            }
        }
        if (!matched) out[used++] = *p++;
    }
    out[used] = 0;
    return out;
}

static void vocab_back_row(void) { }

/* header() ends with a 1-px grey rule at y=210; on e-ink it reads as a broken
   dotted line right above the word, so the vocab pages clear it. */
static void vocab_header(const char* title) {
    header(title);
    thingsos_rect(&app.canvas, 36, 209, 612, 3, 255);
}

/* Circular "undo" arrow at the top-right, mirroring the back glyph at the left. */
static void vocab_undo_icon(void) {
    static const signed char arc[][2] = {
        {14, 0}, {13, 6}, {10, 10}, {6, 13}, {0, 14}, {-6, 13},
        {-10, 10}, {-13, 6}, {-14, 0}, {-13, -6}, {-10, -10}, {-6, -13},
    };
    const int cx = 620, cy = 146;
    for (unsigned i = 0; i < sizeof(arc) / sizeof(arc[0]); i++)
        thingsos_rect(&app.canvas, cx + arc[i][0], cy + arc[i][1], 3, 3, 0);
    toolbar_stroke(cx + 6, cy - 20, cx + 16, cy - 12);
    toolbar_stroke(cx + 16, cy - 12, cx + 11, cy - 1);
}



static void vocab_bar(int y, unsigned done, unsigned total) {
    thingsos_rect(&app.canvas, 36, y, 612, 18, 0);
    thingsos_rect(&app.canvas, 38, y + 2, 608, 14, 235);
    if (total) {
        unsigned width = (unsigned)((uint64_t)608 * done / total);
        if (width) thingsos_rect(&app.canvas, 38, y + 2, (int)width, 14, 120);
    }
}

static const char* vocab_short_meaning(char* out, size_t bytes, const char* in) {
    size_t used = 0;
    while (*in && *in != ';' && strncmp(in, "ï¼", 3) != 0 && used + 1 < bytes && used < 24)
        out[used++] = *in++;
    if (*in && used + 1 < bytes) out[used++] = 0xE2, out[used++] = 0x80, out[used++] = 0xA6; /* … */
    out[used < bytes ? used : bytes - 1] = 0;
    return out;
}

static void vocab_build_quiz(void) {
    char distractors[3][VOCAB_MEANING_BYTES];
    if (!vocab_distractors(vu.card.id, distractors)) {
        for (unsigned i = 0; i < 3; i++) snprintf(distractors[i], VOCAB_MEANING_BYTES, "—");
    }
    vu.option_count = VOCAB_OPTION_ROWS;
    vu.answer_at = (uint8_t)(vu.card.id % VOCAB_OPTION_ROWS);
    unsigned k = 0;
    for (unsigned i = 0; i < VOCAB_OPTION_ROWS; i++) {
        char short_meaning[VOCAB_MEANING_BYTES];
        if (i == vu.answer_at)
            snprintf(vu.options[i], sizeof(vu.options[i]), "%s",
                     vocab_short_meaning(short_meaning, sizeof(short_meaning), vu.card.meaning));
        else
            snprintf(vu.options[i], sizeof(vu.options[i]), "%s",
                     vocab_short_meaning(short_meaning, sizeof(short_meaning), distractors[k++ % 3]));
    }
    vu.answered = false;
    vu.answer_ok = false;
}

/* Spoken word on every learning/review screen; stops any running audio first so
   pronunciation never layers over the music player or a previous take. */
static void vocab_play_file(const char* path) {
    if (!path || !*path) return;
    thingsos_audio_play(path, 0);
}

/* Word first, then the example sentence when that screen shows it. */
static void vocab_play_word(void) {
    char audio[256];
    if (vocab_audio_path(vu.card.id, audio, sizeof(audio))) vocab_play_file(audio);
}

static void vocab_play_stage_audio(void) {
    char example[256];
    if (vu.stage == VOCAB_STAGE_RECALL && vu.card.example_en[0] &&
        vocab_example_audio_path(vu.card.id, example, sizeof(example))) {
        vocab_play_file(example);
        return;
    }
    vocab_play_word();
}

static bool vocab_load_next(void) {
    vu.revealed = false;
    vu.answered = false;
    vu.phase = VOCAB_PHASE_QUESTION;
    if (!vocab_next(&vu.card, &vu.stage)) {
        vu.screen = VOCAB_SCREEN_DONE;
        return false;
    }
    if (vu.stage == VOCAB_STAGE_QUIZ) vocab_build_quiz();
    vocab_play_stage_audio();
    app.dirty = true;
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
    vu.phase = VOCAB_PHASE_QUESTION;
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

static void vocab_start_learn(void) {
    uint32_t now = vocab_now_min();
    vu.booster = false;
    vu.screen = VOCAB_SCREEN_LEARN;
    vocab_begin_session(now, &vu.config, 1);
    vocab_stats(now, &vu.stats);
    if (!vocab_load_next()) app.dirty = true;
}

static void vocab_start_review(void) {
    uint32_t now = vocab_now_min();
    vu.booster = false;
    vu.screen = VOCAB_SCREEN_REVIEW;
    vocab_begin_session(now, &vu.config, 2);
    vocab_stats(now, &vu.stats);
    if (!vocab_load_next()) app.dirty = true;
}

static void vocab_start_booster(void) {
    uint32_t now = vocab_now_min();
    vu.booster = true;
    vu.screen = VOCAB_SCREEN_LEARN;
    vocab_begin_session(now, &vu.config, 3);
    vocab_stats(now, &vu.stats);
    if (!vocab_load_next()) {
        vu.screen = VOCAB_SCREEN_REVIEW;
        vocab_begin_session(now, &vu.config, 2);
        vocab_load_next();
    }
}

static void vocab_do_undo(void) {
    uint32_t now = vocab_now_min();
    if (!vocab_undo(now)) return;
    vocab_stats(now, &vu.stats);
    if (vocab_current_card(&vu.card, &vu.stage)) {
        vu.phase = VOCAB_PHASE_QUESTION;
        vu.answered = false;
        vu.answer_ok = false;
        if (vu.stage == VOCAB_STAGE_QUIZ) vocab_build_quiz();
        app.dirty = true;
    }
}

/* Answer the current question, then show its answer screen. */
static void vocab_submit(vocab_grade_t grade) {
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
    if (graduated && vu.booster) {
        vu.booster = false;
        vu.screen = VOCAB_SCREEN_REVIEW;
        vocab_begin_session(now, &vu.config, 2);
        vocab_stats(now, &vu.stats);
        vocab_load_next();
        return;
    }
    vu.phase = VOCAB_PHASE_ANSWER;
    vocab_play_stage_audio();
    app.dirty = true;
}

static void vocab_draw_answer(void) {
    vocab_header(vu.screen == VOCAB_SCREEN_REVIEW ? "复习" : "学习");
    label(vu.card.word, 36, 258, 64, 612);
    if (vu.card.phonetic[0]) { char ipa[96]; label(vocab_phonetic_text(ipa, sizeof(ipa), vu.card.phonetic), 36, 308, 26, 612); }
    thingsos_rect(&app.canvas, 36, 330, 612, 1, 0);
    if (vu.card.pos[0]) {
        char head[VOCAB_MEANING_BYTES + 16];
        snprintf(head, sizeof(head), "%s %s", vu.card.pos, vu.card.meaning);
        label(head, 36, 396, 34, 612);
    } else {
        label(vu.card.meaning, 36, 396, 36, 612);
    }
    if (vu.card.example_en[0]) label(vu.card.example_en, 36, 496, 30, 612);
    if (vu.card.example_zh[0]) label(vu.card.example_zh, 36, 546, 30, 612);
    int section = 640;
    if (vu.card.root[0]) {
        char line[160];
        snprintf(line, sizeof(line), "词根：%s", vu.card.root);
        label(line, 36, section, 22, 612);
        section += 52;
    }
    if (vu.card.derived[0]) {
        char line[224];
        snprintf(line, sizeof(line), "派生：%s", vu.card.derived);
        label(line, 36, section, 22, 612);
        section += 52;
    }
    if (vu.card.phrase[0]) {
        char line[224];
        snprintf(line, sizeof(line), "短语：%s", vu.card.phrase);
        label(line, 36, section, 22, 612);
    }
    vocab_undo_icon();
    vocab_button(36, 1102, 290, 90, "下 一 词");
    vocab_button(358, 1102, 290, 90, "记 错 了");
}

static void vocab_draw_home(void) {
    vocab_header("背单词");
    char line[128];
    snprintf(line, sizeof(line), "连续 %u 天 · 今日新词 %u · 今日复习 %u",
             (unsigned)vu.stats.streak, (unsigned)vu.stats.new_today,
             (unsigned)vu.stats.reviews_today);
    label(line, 36, 196, 24, 612);
    const uint32_t total = vocab_count();
    const uint32_t learned = vocab_learned_count();
    thingsos_rect(&app.canvas, 36, 236, 612, 1, 0);

    label("学习进度", 36, 296, 28, 300);
    snprintf(line, sizeof(line), "%u / %u", (unsigned)learned, (unsigned)total);
    thingsos_font_size(&app.font, 26);
    thingsos_label_right(&app.font, &app.canvas, line, 36, 296, 612);
    vocab_bar(318, learned, total);
    snprintf(line, sizeof(line), "待学 %u · 已完成 %u%%", (unsigned)vocab_new_count(),
             total ? (unsigned)((uint64_t)learned * 100 / total) : 0u);
    label(line, 36, 366, 22, 612);

    label("复习进度", 36, 446, 28, 300);
    snprintf(line, sizeof(line), "%u / %u", (unsigned)vu.stats.reviews_today,
             (unsigned)vu.stats.due_today);
    thingsos_font_size(&app.font, 26);
    thingsos_label_right(&app.font, &app.canvas, line, 36, 446, 612);
    vocab_bar(468, vu.stats.reviews_today, vu.stats.due_today);
    label("到期词会用「认识 / 模糊 / 忘记」复习", 36, 516, 22, 612);

    vocab_button(36, 640, 612, 120, "开 始 学 习");
    vocab_button(36, 800, 612, 120, "开 始 复 习");
    label("学习：选释义 → 看例句回忆 → 只看单词回忆", 36, 980, 20, 612);
}

static void vocab_draw_quiz(void) {
    vocab_header(vu.screen == VOCAB_SCREEN_REVIEW ? "复习" : "学习");
    label(vu.card.word, 36, 258, 64, 612);
    if (vu.card.phonetic[0]) { char ipa[96]; label(vocab_phonetic_text(ipa, sizeof(ipa), vu.card.phonetic), 36, 308, 26, 612); }
    thingsos_rect(&app.canvas, 36, 330, 612, 1, 0);
    for (unsigned i = 0; i < vu.option_count; i++) {
        const int y = 410 + (int)i * 124;
        const bool marked = vu.answered && (uint8_t)i == vu.answer_at;
        app_outline(36, y, 612, 104);
        if (marked) app_outline(40, y + 4, 604, 96);
        char number[4] = {'A' + (char)i, 0, 0, 0};
        label(number, 56, y + 60, 30, 50);
        if (vu.card.pos[0]) label(vu.card.pos, 104, y + 60, 22, 80);
        label(vu.options[i], 190, y + 60, 26, 430);
    }
    if (!vu.answered) vocab_button(36, 1102, 612, 90, "看 答 案");
}

static void vocab_draw_recall(void) {
    vocab_header(vu.screen == VOCAB_SCREEN_REVIEW ? "复习" : "学习");
    label(vu.card.word, 36, 258, 64, 612);
    if (vu.card.phonetic[0]) { char ipa[96]; label(vocab_phonetic_text(ipa, sizeof(ipa), vu.card.phonetic), 36, 308, 26, 612); }
    thingsos_rect(&app.canvas, 36, 330, 612, 1, 0);
    if (vu.stage == VOCAB_STAGE_RECALL && vu.card.example_en[0])
        label(vu.card.example_en, 36, 400, 30, 612);
    if (vu.stage == VOCAB_STAGE_RECALL) {
        vocab_button(36, 1102, 290, 90, "认识");
        vocab_button(358, 1102, 290, 90, "不认识");
    } else {
        vocab_button(36, 1102, 190, 90, "认识");
        vocab_button(247, 1102, 190, 90, "模糊");
        vocab_button(458, 1102, 190, 90, "忘记了");
    }
}

static void vocab_draw_done(void) {
    vocab_header("背单词");
    label("今日完成", 36, 300, 40, 612);
    char line[96];
    snprintf(line, sizeof(line), "新词 %u · 复习 %u", (unsigned)vu.stats.new_today,
             (unsigned)vu.stats.reviews_today);
    label(line, 36, 380, 28, 612);
    snprintf(line, sizeof(line), "连续 %u 天 · 已学 %u / %u", (unsigned)vu.stats.streak,
             (unsigned)vocab_learned_count(), (unsigned)vocab_count());
    label(line, 36, 430, 26, 612);
    vocab_button(36, 560, 612, 110, "返回主页");
}

static bool vocab_draw(void) {
    if (app.page != THINGSOS_UI_VOCAB) {
        if (vu.started) vocab_stop();
        return false;
    }
    if (!vocab_start()) {
        vocab_header("背单词");
        label("词库未就绪", 36, 400, 34, 612);
        label("请把 bank.bin 放进 /sdcard/.thingsos/vocab/", 36, 450, 24, 612);
        char line[96];
        snprintf(line, sizeof(line), "SD 卡: %s", media() ? "已挂载" : "未挂载");
        label(line, 36, 500, 24, 612);
        label("按 HOME 键返回应用页", 36, 560, 22, 612);
        return true;
    }

    vocab_back_row();
    switch (vu.screen) {
        case VOCAB_SCREEN_HOME: vocab_draw_home(); break;
        case VOCAB_SCREEN_DONE: vocab_draw_done(); break;
        default:
            if (vu.phase == VOCAB_PHASE_ANSWER) vocab_draw_answer();
            else if (vu.stage == VOCAB_STAGE_QUIZ) vocab_draw_quiz();
            else vocab_draw_recall();
            break;
    }
    return true;
}

static void vocab_leave(void) { app_enter(THINGSOS_UI_APPS); }

static bool vocab_tap(int x, int y) {
    if (app.page != THINGSOS_UI_VOCAB) return false;
    if (x < 92 && y >= 74 && y < 205) {
        if (vu.screen == VOCAB_SCREEN_HOME) {
            vocab_leave();
        } else {
            vocab_stop();
            vu.screen = VOCAB_SCREEN_HOME;
            app.dirty = true;
        }
        return true;
    }
    (void)x;
    if (vu.failed) return true;
    /* The shell navigation is hidden on this page: tapping the bottom strip
       leaves the app, which is what the old "返回应用" row did. */
    if (y >= 1200) { vocab_leave(); return true; }

    if (vu.screen == VOCAB_SCREEN_HOME) {
        if (y >= 640 && y < 760) { vocab_start_learn(); return true; }
        if (y >= 800 && y < 920) { vocab_start_review(); return true; }
        return true;
    }
    if (vu.screen == VOCAB_SCREEN_DONE) {
        if (y >= 560 && y < 680) { vu.screen = VOCAB_SCREEN_HOME; app.dirty = true; }
        return true;
    }

    /* Tap the word / phonetic band to hear the pronunciation (explicit only). */
    if (y >= 180 && y < 340) {
        vocab_play_word();
        return true;
    }
    if (y >= 380 && y < 560 && vu.card.example_en[0]) {
        char example[256];
        if (vocab_example_audio_path(vu.card.id, example, sizeof(example)))
            vocab_play_file(example);
        return true;
    }

    if (vu.phase == VOCAB_PHASE_ANSWER) {
        if (x >= 580 && y >= 74 && y < 205) {
            vocab_do_undo();
            return true;
        }
        if (y >= 1102 && y < 1196) {
            if (x >= 358) {
                vocab_submit(VOCAB_AGAIN);
                vocab_load_next();
            } else {
                vocab_load_next();
            }
        }
        return true;
    }

    if (vu.stage == VOCAB_STAGE_QUIZ) {
        if (!vu.answered) {
            if (y >= 410 && y < 410 + 4 * 124) {
                const int index = (y - 410) / 124;
                if (index < (int)vu.option_count && x >= 36) {
                    vu.answered = true;
                    vu.answer_ok = (uint8_t)index == vu.answer_at;
                    app.dirty = true;
                    if (vu.answer_ok) vocab_submit(VOCAB_GOOD); /* straight to screen 2 */
                }
            } else if (y >= 1102 && y < 1196) {
                vocab_submit(VOCAB_AGAIN); /* 看答案 counts as "did not know" */
            }
        } else {
            if (vu.answer_ok) return true;
            vocab_submit(VOCAB_AGAIN);
        }
        return true;
    }

    /* Grade row sits at the bottom band, same geometry as the drawn buttons. */
    if (y >= 1102 && y < 1196) {
        if (vu.stage == VOCAB_STAGE_RECALL) {
            if (x >= 358) vocab_submit(VOCAB_AGAIN);
            else vocab_submit(VOCAB_GOOD);
        } else {
            if (x >= 458) vocab_submit(VOCAB_AGAIN);
            else if (x >= 247) vocab_submit(VOCAB_HARD);
            else vocab_submit(VOCAB_GOOD);
        }
    }
    return true;
}

static bool vocab_key(int action) {
    if (app.page != THINGSOS_UI_VOCAB || action == THINGSOS_KEY_NONE) return false;
    /* The middle key always leaves the app; it never grades. */
    if (action == THINGSOS_KEY_HOME || action == THINGSOS_KEY_MENU) {
        vocab_stop();
        vocab_leave();
        return true;
    }
    if (vu.failed) return false;
    if (vu.screen == VOCAB_SCREEN_HOME) {
        if (action == THINGSOS_KEY_NEXT) vocab_start_learn();
        else if (action == THINGSOS_KEY_PREVIOUS) vocab_start_review();
        return true;
    }
    if (vu.screen == VOCAB_SCREEN_DONE) {
        if (action == THINGSOS_KEY_NEXT || action == THINGSOS_KEY_PREVIOUS) {
            vu.screen = VOCAB_SCREEN_HOME;
            app.dirty = true;
        }
        return true;
    }
    if (vu.phase == VOCAB_PHASE_ANSWER) {
        if (action == THINGSOS_KEY_NEXT || action == THINGSOS_KEY_PREVIOUS) vocab_load_next();
        return true;
    }
    if (vu.stage == VOCAB_STAGE_QUIZ) {
        if (vu.answered && !vu.answer_ok) vocab_submit(VOCAB_AGAIN);
        return true;
    }
    /* Next = 不认识 (Again), Previous = 认识 (Good); no key maps to 模糊. */
    if (action == THINGSOS_KEY_NEXT) vocab_submit(VOCAB_AGAIN);
    else if (action == THINGSOS_KEY_PREVIOUS) vocab_submit(VOCAB_GOOD);
    return true;
}

