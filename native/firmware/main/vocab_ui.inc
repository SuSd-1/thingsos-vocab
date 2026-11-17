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
#include "esp_timer.h"

#define VOCAB_UX_MINUTES_2020 26297280u
#define VOCAB_OPTION_ROWS 4

typedef enum {
    VOCAB_SCREEN_HOME = 0,
    VOCAB_SCREEN_LEARN,
    VOCAB_SCREEN_REVIEW,
    VOCAB_SCREEN_DONE,
    VOCAB_SCREEN_SETTINGS,
    VOCAB_SCREEN_STATS,
} vocab_screen_t;

typedef enum {
    VOCAB_PHASE_QUESTION = 0,
    VOCAB_PHASE_ANSWER,
} vocab_phase_t;

static uint8_t vu_chain_example;
static uint64_t vu_time_last_us;
static uint32_t vu_time_pending_ms;
static bool vu_confirm_clear;
static uint32_t vu_history[8];
static uint8_t vu_hist_count, vu_hist_pos;
static bool vu_peeking;
static uint32_t vu_peek_return;

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
        {"tʃ", "ch"}, {"dʒ", "j"},  {"ʧ", "ch"}, {"ʤ", "j"},  {"ˈ", "'"}, {"ˌ", ","},
        {"ː", ":"},   {"ˑ", ":"},   {"̃", ""},   {"ʲ", "j"},   {"ʷ", "w"},
        {"ə", "e"},   {"ɪ", "i"},   {"ʊ", "u"}, {"ʃ", "sh"}, {"ʒ", "zh"},
        {"ŋ", "ng"},  {"θ", "th"},  {"ð", "dh"}, {"ɔ", "o"}, {"ɒ", "o"},
        {"ɑ", "a"},   {"ɜ", "er"},  {"ʌ", "u"}, {"æ", "a"}, {"ɡ", "g"},
        {"ɛ", "e"},   {"ɐ", "a"},   {"ɹ", "r"}, {"ɾ", "t"}, {"ʔ", ""},
        {"ɚ", "er"},  {"ɝ", "er"},  {"ʉ", "u"}, {"ɨ", "i"}, {"ɫ", "l"},
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

/* header() ends with a 1-px grey rule at y=210; on e-ink it reads as a broken
   dotted line right above the word, so the vocab pages clear it. */
static void vocab_header(const char* title) {
    header(title);
    thingsos_rect(&app.canvas, 36, 209, 612, 3, 255);
}

/* Circular "undo" arrow at the top-right, mirroring the back glyph at the left. */
static void vocab_undo_icon(void) {
    static const signed char ring[12][2] = {
        {11, 0}, {10, 6}, {6, 10}, {0, 11}, {-6, 10}, {-10, 6},
        {-11, 0}, {-10, -6}, {-6, -10}, {0, -11}, {6, -10}, {10, -6},
    };
    const int cx = 620, cy = 148;
    for (int i = 0; i < 12; i++)
        toolbar_stroke(cx + ring[i][0], cy + ring[i][1],
                       cx + ring[(i + 1) % 12][0], cy + ring[(i + 1) % 12][1]);
    /* A real glyph keeps the arrow perfectly centred inside the ring. */
    label("←", cx - 11, cy + 9, 26, 30);
}




static size_t vocab_utf8_width(const char* s) {
    const unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;
    if ((c & 0xF8) == 0xF0) return 4;
    if ((c & 0xF0) == 0xE0) return 3;
    return 2;
}

/* Up to two part-of-speech clauses, kept under 20 characters so the option
   stays on one centred line. */
static const char* vocab_short_meaning(char* out, size_t bytes, const char* in) {
    size_t used = 0;
    int chars = 0;
    while (*in && *in != '\n' && chars < 20) {
        const size_t w = vocab_utf8_width(in);
        const bool cut = *in == ';' || strncmp(in, "\xEF\xBC\x9B", 3) == 0;
        if (cut) {
            /* try the next part of speech if it still fits */
            const char* next = in + (w == 1 ? 1 : 3);
            while (*next == ' ') next++;
            int extra = 0;
            const char* probe = next;
            while (*probe && *probe != '\n' && extra < 14) {
                const size_t pw = vocab_utf8_width(probe);
                probe += pw;
                extra++;
            }
            if (*next && *next != '\n' && chars + 1 + extra <= 20) {
                out[used++] = ' '; chars++;
                while (*in && *in != '\n' && chars < 20) {
                    const size_t w2 = vocab_utf8_width(in);
                    if (used + w2 + 1 >= bytes) break;
                    memcpy(out + used, in, w2);
                    used += w2; in += w2; chars++;
                }
                continue;
            }
            break;
        }
        if (used + w + 1 >= bytes) break;
        memcpy(out + used, in, w);
        used += w; in += w; chars++;
    }
    out[used] = 0;
    return out;
}

/* Two balanced lines so an option fills its box instead of ending in dots. */
static void vocab_draw_option_text(const char* text, int x, int y, int width) {
    label(text, x, y + 60, 26, width);
}

static uint32_t vocab_next_cp(const char* s, unsigned* bytes) {
    const unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *bytes = 1; return c; }
    if ((c & 0xE0) == 0xC0) { *bytes = 2; return ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0) {
        *bytes = 3;
        return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    *bytes = 4;
    return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
           ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
}

/* Wrap using the font's real advance widths: nothing overflows, so the renderer
   never truncates the tail of a line, and words break at spaces when possible. */
static int vocab_draw_wrapped(const char* text, int x, int y, int size, int max_lines) {
    const int max_width = 612;
    char line[VOCAB_EXAMPLE_BYTES * 2];
    thingsos_font_size(&app.font, size);
    size_t start = 0;
    int lines = 0;
    while (text[start] && lines < max_lines) {
        size_t i = start, last_space = (size_t)-1;
        int used = 0;
        while (text[i]) {
            unsigned bytes;
            const uint32_t cp = vocab_next_cp(text + i, &bytes);
            const int advance = thingsos_glyph(&app.font, NULL, cp, 0, 0);
            if (used + advance > max_width) break;
            used += advance;
            if (cp == ' ') last_space = i;
            i += bytes;
        }
        if (text[i] && last_space != (size_t)-1 && last_space > start) i = last_space;
        size_t len = i - start;
        while (len && text[start + len - 1] == ' ') len--;
        if (len + 1 >= sizeof(line)) len = sizeof(line) - 2;
        memcpy(line, text + start, len);
        line[len] = 0;
        if (line[0]) label(line, x, y + lines * (size + 8), size, 612);
        lines++;
        start = i;
        while (text[start] == ' ') start++;
        if (i == start && !text[i]) break;
        if (!text[i]) break;
    }
    return lines;
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
static void vocab_time_flush(void) {
    if (vu_time_pending_ms < 1000) return;
    const uint32_t secs = vu_time_pending_ms / 1000;
    vu_time_pending_ms -= secs * 1000;
    vocab_add_seconds(secs);
}

static bool vocab_studying(void) {
    return app.page == THINGSOS_UI_VOCAB && !app.asleep &&
           (vu.screen == VOCAB_SCREEN_LEARN || vu.screen == VOCAB_SCREEN_REVIEW);
}

static void vocab_time_tick(void) {
    if (!vocab_studying()) {
        vu_time_last_us = 0;
        vocab_time_flush();
        return;
    }
    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (vu_time_last_us && now > vu_time_last_us) vu_time_pending_ms += (uint32_t)((now - vu_time_last_us) / 1000);
    vu_time_last_us = now;
    if (vu_time_pending_ms >= 30000) vocab_time_flush();
}

static void vocab_play_word(void) {
    char audio[256];
    if (vocab_audio_path(vu.card.id, audio, sizeof(audio))) vocab_play_file(audio);
}

/* Every screen speaks the word itself. The example sentence is only played
   when the learner taps the example line. */
static void vocab_play_stage_audio(void) {
    if (!vu.config.auto_audio) return;
    vocab_play_word();
}

/* The audio service is single-channel: when the word finishes we start the
   example sentence so the answer page reads "word, then sentence". */
static void vocab_audio_tick(void) {
    vocab_time_tick();
    if (!vu_chain_example || app.page != THINGSOS_UI_VOCAB) return;
    thingsos_audio_status_t status;
    thingsos_audio_status(&status);
    if (status.state != THINGSOS_AUDIO_IDLE && status.state != THINGSOS_AUDIO_ERROR) return;
    vu_chain_example = 0;
    char example[256];
    if (vocab_example_audio_path(vu.card.id, example, sizeof(example)))
        thingsos_audio_play(example, 0);
}

static bool vocab_load_next(void) {
    vu_peeking = false;
    vu_chain_example = 0;
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
    vocab_time_flush();
    vu_chain_example = 0;
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


/* Answer the current question, then show its answer screen. */
static void vocab_submit(vocab_grade_t grade) {
    uint32_t now = vocab_now_min();
    const uint32_t learned_before = vocab_learned_count();
    if (!vocab_answer(now, grade)) return;
    if (vu_hist_count == 0 || vu_history[(vu_hist_pos + 7) % 8] != vu.card.id) {
        vu_history[vu_hist_pos] = vu.card.id;
        vu_hist_pos = (uint8_t)((vu_hist_pos + 1) % 8);
        if (vu_hist_count < 8) vu_hist_count++;
    }
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
    {
        char example[256];
        vu_chain_example = (vu.config.auto_example &&
                            vocab_example_audio_path(vu.card.id, example, sizeof(example))) ? 1 : 0;
    }
    app.dirty = true;
}

/* One part of speech per line, exactly as the book splits the gloss. */
/* Where we are in the current group: round and card inside the round. */
static void vocab_draw_round_progress(void) {
    uint16_t group;
    uint8_t round, index, total, phase;
    if (!vocab_plan_progress(&group, &round, &index, &total, &phase)) return;
    char line[64];
    if (phase)
        snprintf(line, sizeof(line), "重学 %u/%u", (unsigned)index, (unsigned)total);
    else
        snprintf(line, sizeof(line), "第 %u 轮 · %u/%u", (unsigned)round, (unsigned)index,
                 (unsigned)total);
    label(line, 36, 1070, 22, 400);
}

static void vocab_draw_meaning(const char* text, int y) {
    char buf[VOCAB_MEANING_BYTES];
    snprintf(buf, sizeof(buf), "%s", text);
    char* line = buf;
    for (int row = 0; line && *line && row < 3; row++) {
        char* next = strchr(line, '\n');
        if (next) *next = 0;
        label(line, 36, y + row * 46, 32, 612);
        line = next ? next + 1 : NULL;
    }
}

static void vocab_draw_answer(void) {
    vocab_draw_round_progress();
    vocab_header(vu.screen == VOCAB_SCREEN_REVIEW ? "复习" : "学习");
    label(vu.card.word, 36, 258, 64, 612);
    if (vu.card.phonetic[0]) { char ipa[96]; label(vocab_phonetic_text(ipa, sizeof(ipa), vu.card.phonetic), 36, 308, 26, 612); }
    thingsos_rect(&app.canvas, 36, 330, 612, 1, 0);
    vocab_draw_meaning(vu.card.meaning, 392);
    /* Fixed bands: the example keeps its place whether the gloss is one line or three. */
    thingsos_rect(&app.canvas, 36, 566, 612, 1, 90);
    int example_lines = vu.card.example_en[0] ? vocab_draw_wrapped(vu.card.example_en, 36, 622, 32, 3) : 0;
    if (vu.card.example_zh[0])
        vocab_draw_wrapped(vu.card.example_zh, 36, 622 + example_lines * 40 + 6, 32, 3);
    int section = 770;
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

static void vocab_gear_icon(void) {
    static const signed char ring[12][2] = {
        {10, 0}, {9, 5}, {5, 9}, {0, 10}, {-5, 9}, {-9, 5},
        {-10, 0}, {-9, -5}, {-5, -9}, {0, -10}, {5, -9}, {9, -5},
    };
    static const signed char dir[8][2] = {
        {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1},
    };
    const int cx = 620, cy = 146;
    for (int i = 0; i < 12; i++)
        toolbar_stroke(cx + ring[i][0], cy + ring[i][1],
                       cx + ring[(i + 1) % 12][0], cy + ring[(i + 1) % 12][1]);
    for (int i = 0; i < 8; i++)
        toolbar_stroke(cx + dir[i][0] * 12, cy + dir[i][1] * 12,
                       cx + dir[i][0] * 15, cy + dir[i][1] * 15);
    thingsos_rect(&app.canvas, cx - 3, cy - 3, 6, 6, 0);
}

static void vocab_draw_home(void) {
    vocab_header("背单词");
    vocab_gear_icon();
    const unsigned learn_left = vu.stats.new_today < vu.config.new_limit
                                    ? vu.config.new_limit - vu.stats.new_today : 0;
    unsigned review_done = vu.stats.reviews_today;
    if (review_done > vu.stats.due_today) review_done = vu.stats.due_today;
    const unsigned review_left = vu.stats.due_today > review_done
                                     ? vu.stats.due_today - review_done : 0;
    char line[64];
    app_outline(36, 560, 300, 190);
    app_outline(348, 560, 300, 190);
    label("学习", 64, 628, 40, 240);
    if (!learn_left && vu.stats.new_today) label("已完成", 64, 712, 36, 240);
    else { snprintf(line, sizeof(line), "%u", learn_left); label(line, 64, 716, 64, 240); }
    label("复习", 376, 628, 40, 240);
    if (!review_left && vu.stats.due_today) label("已完成", 376, 712, 36, 240);
    else { snprintf(line, sizeof(line), "%u", review_left); label(line, 376, 716, 64, 240); }
    snprintf(line, sizeof(line), "连续 %u 天 · 已学 %u / %u", (unsigned)vu.stats.streak,
             (unsigned)vocab_learned_count(), (unsigned)vocab_count());
    label(line, 36, 830, 24, 612);
    label("每日新词与复习上限在右上角「设置」里调整", 36, 880, 22, 612);
}

static const char* const vocab_setting_names[11] = {
    "每日新词目标", "每日复习上限", "每组词数", "目标保持率", "学习算法",
    "乱序学习", "自动发音", "朗读例句", "答错处理", "清空学习进度", "恢复默认设置",
};

static void vocab_format_span(uint32_t seconds, char* out, size_t bytes) {
    const uint32_t hours = seconds / 3600u;
    const uint32_t minutes = (seconds % 3600u) / 60u;
    if (hours) snprintf(out, bytes, "%u 小时 %u 分", (unsigned)hours, (unsigned)minutes);
    else if (minutes) snprintf(out, bytes, "%u 分 %u 秒", (unsigned)minutes, (unsigned)(seconds % 60u));
    else snprintf(out, bytes, "%u 秒", (unsigned)seconds);
}

static void vocab_draw_bottom_bar(int active) {
    const char* labels[3] = {"首页", "我的", "设置"};
    thingsos_rect(&app.canvas, 0, 1106, 684, 1, 120);
    for (int i = 0; i < 3; i++) {
        const int cx = i * 228 + 114;
        if (i == active) thingsos_rect(&app.canvas, cx - 26, 1186, 52, 3, 0);
        centered_label(labels[i], i * 228, 1170, 26, 228);
    }
}

static void vocab_draw_stats(void) {
    vocab_header("我的学习");
    char line[96], span[48];
    uint32_t now = vocab_now_min();
    vocab_stats(now, &vu.stats);
    snprintf(line, sizeof(line), "今日学习 %u 词 · 今日复习 %u 词",
             (unsigned)vu.stats.new_today, (unsigned)vu.stats.reviews_today);
    label(line, 36, 280, 28, 612);
    snprintf(line, sizeof(line), "累计已学 %u / %u", (unsigned)vu.stats.learned,
             (unsigned)vu.stats.total);
    label(line, 36, 356, 28, 612);
    vocab_format_span(vu.stats.today_seconds, span, sizeof(span));
    snprintf(line, sizeof(line), "今日用时 %s", span);
    label(line, 36, 432, 26, 612);
    vocab_format_span(vu.stats.total_seconds, span, sizeof(span));
    snprintf(line, sizeof(line), "累计用时 %s", span);
    label(line, 36, 500, 26, 612);
    snprintf(line, sizeof(line), "连续学习 %u 天", (unsigned)vu.stats.streak);
    label(line, 36, 568, 26, 612);
}

static void vocab_draw_settings(void) {
    vocab_header("设置");
    char value[32];
    for (int i = 0; i < 11; i++) {
        const int y = 236 + i * 70;
        label(vocab_setting_names[i], 36, y, 26, 360);
        value[0] = 0;
        switch (i) {
            case 0: snprintf(value, sizeof(value), "%u", (unsigned)vu.config.new_limit); break;
            case 1: snprintf(value, sizeof(value), "%u", (unsigned)vu.config.review_limit); break;
            case 2: snprintf(value, sizeof(value), "%u", (unsigned)vu.config.group_size); break;
            case 3: snprintf(value, sizeof(value), "%u%%", (unsigned)vu.config.target_percent); break;
            case 4: snprintf(value, sizeof(value), "%s",
                             vu.config.mode == VOCAB_MODE_CLASSIC ? "经典" : "FSRS"); break;
            case 5: snprintf(value, sizeof(value), "%s", vu.config.shuffle ? "开" : "关"); break;
            case 6: snprintf(value, sizeof(value), "%s", vu.config.auto_audio ? "开" : "关"); break;
            case 7: snprintf(value, sizeof(value), "%s", vu.config.auto_example ? "开" : "关"); break;
            case 8: snprintf(value, sizeof(value), "%s", vu.config.error_mode ? "本轮" : "组末"); break;
            default: break;
        }
        if (i < 9) {
            thingsos_font_size(&app.font, 26);
            thingsos_label_right(&app.font, &app.canvas, value, 36, y, 500);
        } else {
            vocab_button(470, y - 34, 178, 48, i == 9 ? "清 空" : "恢 复");
        }
        if (i < 4) {
            vocab_button(548, y - 34, 44, 48, "－");
            vocab_button(600, y - 34, 44, 48, "＋");
        }
        thingsos_rect(&app.canvas, 36, y + 18, 612, 1, 210);
    }
    if (vu_confirm_clear) {
        thingsos_rect(&app.canvas, 90, 470, 504, 252, 255);
        thingsos_rect(&app.canvas, 90, 470, 504, 2, 0);
        thingsos_rect(&app.canvas, 90, 720, 504, 2, 0);
        centered_label("确定清空全部学习进度？", 90, 542, 30, 504);
        centered_label("词库与发音不会被删除", 90, 592, 22, 504);
        vocab_button(120, 632, 180, 70, "取消");
        vocab_button(384, 632, 180, 70, "确定");
    }
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
        vocab_draw_option_text(vu.options[i], 104, y, 520);
    }
    if (!vu.answered) vocab_button(36, 1102, 612, 90, "看 答 案");
}

static void vocab_draw_recall(void) {
    vocab_draw_round_progress();
    vocab_header(vu.screen == VOCAB_SCREEN_REVIEW ? "复习" : "学习");
    label(vu.card.word, 36, 258, 64, 612);
    if (vu.card.phonetic[0]) {
        char ipa[96];
        label(vocab_phonetic_text(ipa, sizeof(ipa), vu.card.phonetic), 36, 308, 26, 612);
    }
    thingsos_rect(&app.canvas, 36, 330, 612, 1, 0);
    /* Middle band stays empty while recalling: the gloss appears only after the
       grade, on the answer screen, so the bands never move. */
    thingsos_rect(&app.canvas, 36, 566, 612, 1, 90);
    if (vu.card.example_en[0]) vocab_draw_wrapped(vu.card.example_en, 36, 622, 32, 4);
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

    if (vu.screen == VOCAB_SCREEN_HOME) vocab_draw_bottom_bar(0);
    else if (vu.screen == VOCAB_SCREEN_STATS) vocab_draw_bottom_bar(1);
    else if (vu.screen == VOCAB_SCREEN_SETTINGS) vocab_draw_bottom_bar(2);
    switch (vu.screen) {
        case VOCAB_SCREEN_HOME: vocab_draw_home(); break;
        case VOCAB_SCREEN_SETTINGS: vocab_draw_settings(); break;
        case VOCAB_SCREEN_STATS: vocab_draw_stats(); break;
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
        } else if (vu.screen == VOCAB_SCREEN_SETTINGS) {
            vu.screen = VOCAB_SCREEN_HOME;
            vu_confirm_clear = false;
            app.dirty = true;
        } else {
            vocab_stop();
            vu.screen = VOCAB_SCREEN_HOME;
            app.dirty = true;
        }
        return true;
    }
    (void)x;
    if (vu.failed) return true;
    /* No bottom-edge exit: the panel's last rows receive stray taps and an
       implicit leave looked like a crash. Leaving is MENU / HOME / "<" only. */

    if (vu.screen == VOCAB_SCREEN_HOME || vu.screen == VOCAB_SCREEN_STATS ||
        vu.screen == VOCAB_SCREEN_SETTINGS) {
        if (vu.screen == VOCAB_SCREEN_SETTINGS && vu_confirm_clear) {
            /* handled below */
        } else if (y >= 1110) {
            vu_confirm_clear = false;
            vu.screen = x < 228 ? VOCAB_SCREEN_HOME
                        : x < 456 ? VOCAB_SCREEN_STATS
                                  : VOCAB_SCREEN_SETTINGS;
            app.dirty = true;
            return true;
        }
    }
    if (vu.screen == VOCAB_SCREEN_HOME) {
        if (x >= 580 && y < 205) { vu.screen = VOCAB_SCREEN_SETTINGS; app.dirty = true; return true; }
        if (y >= 560 && y < 750) {
            if (x < 348) vocab_start_learn();
            else vocab_start_review();
            return true;
        }
        return true;
    }
    if (vu.screen == VOCAB_SCREEN_SETTINGS) {
        if (vu_confirm_clear) {
            if (y >= 620 && y < 712) {
                if (x >= 120 && x < 300) {
                    vu_confirm_clear = false;
                    app.dirty = true;
                } else if (x >= 384 && x < 564) {
                    vu_confirm_clear = false;
                    vocab_reset_progress();
                    vu.started = true;
                    uint32_t now = vocab_now_min();
                    vocab_stats(now, &vu.stats);
                    app.dirty = true;
                }
            }
            return true;
        }
        const int row = (y - 200) / 70;
        if (row >= 0 && row < 11 && x >= 380) {
            bool plus = x >= 600;
            bool save = true;
            switch (row) {
                case 0:
                    if (x >= 548) {
                        int v = (int)vu.config.new_limit + (plus ? 5 : -5);
                        if (v < 5) v = 5;
                        if (v > 200) v = 200;
                        vu.config.new_limit = (uint16_t)v;
                    } else save = false;
                    break;
                case 1:
                    if (x >= 548) {
                        int v = (int)vu.config.review_limit + (plus ? 50 : -50);
                        if (v < 50) v = 50;
                        if (v > 2000) v = 2000;
                        vu.config.review_limit = (uint16_t)v;
                    } else save = false;
                    break;
                case 2:
                    if (x >= 548) {
                        int v = (int)vu.config.group_size + (plus ? 5 : -5);
                        if (v < 5) v = 5;
                        if (v > 20) v = 20;
                        vu.config.group_size = (uint8_t)v;
                    } else save = false;
                    break;
                case 3:
                    if (x >= 548) {
                        int v = (int)vu.config.target_percent + (plus ? 5 : -5);
                        if (v < 80) v = 80;
                        if (v > 95) v = 95;
                        vu.config.target_percent = (uint8_t)v;
                    } else save = false;
                    break;
                case 4:
                    vu.config.mode = vu.config.mode == VOCAB_MODE_CLASSIC ? VOCAB_MODE_FSRS
                                                                         : VOCAB_MODE_CLASSIC;
                    break;
                case 5:
                    vu.config.shuffle = !vu.config.shuffle;
                    vocab_abandon_plan();
                    break;
                case 6: vu.config.auto_audio = !vu.config.auto_audio; break;
                case 7: vu.config.auto_example = !vu.config.auto_example; break;
                case 8: vu.config.error_mode = vu.config.error_mode ? 0 : 1; break;
                case 9: vu_confirm_clear = true; save = false; break;
                case 10: vocab_config_defaults(&vu.config); break;
            }
            if (save) vocab_config_save("/sdcard", &vu.config);
            app.dirty = true;
            return true;
        }
        return true;
    }
    if (vu.screen == VOCAB_SCREEN_STATS) {
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

    if (vu.phase == VOCAB_PHASE_ANSWER) {
        if (x >= 580 && y >= 74 && y < 205) {
            if (vu_peeking) {
                if (vocab_load_by_id(vu_peek_return, &vu.card, &vu.stage)) {
                    vu.phase = VOCAB_PHASE_ANSWER;
                    vu_peeking = false;
                    app.dirty = true;
                }
                return true;
            }
            for (uint8_t k = 1; k < vu_hist_count; k++) {
                const uint32_t id = vu_history[(vu_hist_pos + 8 - 1 - k) % 8];
                if (id == vu.card.id) continue;
                const uint32_t original = vu.card.id;
                if (vocab_load_by_id(id, &vu.card, &vu.stage)) {
                    vu_peek_return = original;
                    vu_peeking = true;
                    vu.phase = VOCAB_PHASE_ANSWER;
                    app.dirty = true;
                }
                break;
            }
            return true;
        }
        if (vu_peeking) return true;
        if (y >= 600 && y < 780 && vu.card.example_en[0]) {
            char example[256];
            if (vocab_example_audio_path(vu.card.id, example, sizeof(example)))
                vocab_play_file(example);
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
    if (vu.screen == VOCAB_SCREEN_STATS || vu.screen == VOCAB_SCREEN_SETTINGS) {
        if (action == THINGSOS_KEY_NEXT || action == THINGSOS_KEY_PREVIOUS) {
            vu.screen = VOCAB_SCREEN_HOME;
            vu_confirm_clear = false;
            app.dirty = true;
        }
        return true;
    }
    if (vu.screen == VOCAB_SCREEN_STATS) {
        if (action == THINGSOS_KEY_NEXT || action == THINGSOS_KEY_PREVIOUS) {
            vu.screen = VOCAB_SCREEN_HOME;
            app.dirty = true;
        }
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

