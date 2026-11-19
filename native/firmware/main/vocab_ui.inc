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
#include "esp_heap_caps.h"
#include "thingsos/display_buffer.h"

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
static bool vu_region_pending;
static int vu_region_x, vu_region_y, vu_region_w, vu_region_h;
static uint64_t vu_time_last_us;
static uint32_t vu_time_pending_ms;
static bool vu_confirm_clear;
static bool vu_full_requested;
static uint8_t vu_pending_audio;
static bool vu_hint_valid;
static int vu_hint_y, vu_hint_h;
static uint8_t vu_region_count;
static uint32_t vu_history[8];
static uint8_t vu_hist_grade[8];
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
    bool graded;                 /* answer already recorded (short-press wrong) */
    uint8_t pending_grade;       /* grade to record when leaving the quiz */
    char option_words[VOCAB_OPTION_ROWS][VOCAB_WORD_BYTES];
    char option_pos[VOCAB_OPTION_ROWS][8];
    char option_mean[VOCAB_OPTION_ROWS][VOCAB_MEANING_BYTES];
    uint8_t option_count, answer_at;
    bool answered, answer_ok;
    bool hint_shown;
    bool undo_active;
    uint32_t undo_return_id;
    uint32_t undo_card_id;
    uint8_t undo_prev_grade;
    vocab_screen_t undo_screen;
    vocab_phase_t undo_phase;
    vocab_card_stage_t undo_stage;
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

/* Split "adj. 中立的；中性的" into the part of speech and the gloss. */
static void vocab_split_pos(const char* in, char* pos, size_t pos_bytes,
                            char* mean, size_t mean_bytes) {
    pos[0] = 0;
    mean[0] = 0;
    if (!in) return;
    const char* p = in;
    size_t n = 0;
    bool dotted = false;
    while (*p && *p != ' ' && *p != '\n' && n + 1 < pos_bytes) {
        if (*p == '.') { if (n + 1 < pos_bytes) pos[n++] = '.'; p++; dotted = true; break; }
        pos[n++] = *p++;
    }
    pos[n] = 0;
    if (dotted && n >= 1) {
        while (*p == ' ') p++;
        snprintf(mean, mean_bytes, "%s", p);
    } else {
        pos[0] = 0;
        snprintf(mean, mean_bytes, "%s", in);
    }
}

#define VOCAB_OPTION_TOP 350
#define VOCAB_OPTION_HEIGHT 172
#define VOCAB_OPTION_PITCH 186

static int vocab_option_index(int y) {
    if (y < VOCAB_OPTION_TOP || y >= VOCAB_OPTION_TOP + 4 * VOCAB_OPTION_PITCH) return -1;
    return (y - VOCAB_OPTION_TOP) / VOCAB_OPTION_PITCH;
}

static void vocab_build_quiz(void) {
    char dmeans[3][VOCAB_MEANING_BYTES];
    char dwords[3][VOCAB_WORD_BYTES];
    if (!vocab_distractors_ex(vu.card.id, dmeans, dwords)) {
        for (unsigned i = 0; i < 3; i++) {
            snprintf(dmeans[i], VOCAB_MEANING_BYTES, "—");
            snprintf(dwords[i], VOCAB_WORD_BYTES, "—");
        }
    }
    vu.option_count = VOCAB_OPTION_ROWS;
    vu.answer_at = (uint8_t)(vu.card.id % VOCAB_OPTION_ROWS);
    unsigned k = 0;
    for (unsigned i = 0; i < VOCAB_OPTION_ROWS; i++) {
        const bool self = i == vu.answer_at;
        const char* src = self ? vu.card.meaning : dmeans[k % 3];
        const char* word = self ? vu.card.word : dwords[k % 3];
        if (!self) k++;
        char short_meaning[VOCAB_MEANING_BYTES];
        vocab_short_meaning(short_meaning, sizeof(short_meaning), src);
        snprintf(vu.option_words[i], sizeof(vu.option_words[i]), "%s", word);
        vocab_split_pos(short_meaning, vu.option_pos[i], sizeof(vu.option_pos[i]),
                        vu.option_mean[i], sizeof(vu.option_mean[i]));
    }
    vu.answered = false;
    vu.answer_ok = false;
    vu.revealed = false;
    vu.graded = false;
    vu.pending_grade = VOCAB_AGAIN;
    vu.hint_shown = false;
    ESP_LOGI("vocab", "quiz word='%s' id=%u answer_at=%u", vu.card.word,
             (unsigned)vu.card.id, (unsigned)vu.answer_at);
    for (unsigned i = 0; i < vu.option_count; i++)
        ESP_LOGI("vocab", "  option %u word='%s' pos='%s' mean='%s'",
                 i, vu.option_words[i], vu.option_pos[i], vu.option_mean[i]);
}

/* Spoken word on every learning/review screen; stops any running audio first so
   pronunciation never layers over the music player or a previous take. */
/* Presentation: one clean full-frame refresh per changed screen. The earlier
   regional experiment used thingsos_board_present_audio_diff_clean(), which
   still scans full GC16 phases over the band (935 ms) and leaves text lighter;
   a真正轻量的 DU/GL16 区域接口在 board 层实现后再启用。 */
/* Diagnostic: compare the whole frame with the previous one and log the
   bounding box of the difference, so we can see which rows/columns the word
   change actually touches. */
static uint8_t* vu_frame_shadow;
static bool vu_frame_shadow_ready(void) {
    if (vu_frame_shadow) return true;
    vu_frame_shadow = heap_caps_malloc(THINGSOS_PANEL_BYTES, MALLOC_CAP_SPIRAM);
    return vu_frame_shadow != NULL;
}
static void vocab_frame_diff_log(const thingsos_canvas_t* canvas) {
    if (!vu_frame_shadow_ready() || canvas->width != 684) return;
    const int stride = canvas->width / 2;
    int minx = stride, maxx = -1, miny = canvas->height, maxy = -1;
    for (int y = 0; y < canvas->height; y++) {
        const uint8_t* cur = canvas->pixels + (size_t)y * stride;
        const uint8_t* old = vu_frame_shadow + (size_t)y * stride;
        for (int b = 0; b < stride; b++) {
            if (cur[b] != old[b]) {
                if (b < minx) minx = b;
                if (b > maxx) maxx = b;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
        memcpy(vu_frame_shadow + (size_t)y * stride, cur, (size_t)stride);
    }
    if (maxx < 0) ESP_LOGI("vocab", "frame unchanged");
    else ESP_LOGI("vocab", "word '%s' frame diff x=%d..%d y=%d..%d",
                  vu.card.word, minx * 2, maxx * 2 + 1, miny, maxy);
}

static esp_err_t vocab_region_present(const thingsos_canvas_t* canvas) {
    vocab_frame_diff_log(canvas);
    /* Refresh tiers live in the system display settings; this page only asks
       the board to present, so the two settings cannot conflict. */
    vu_full_requested = false;
    return thingsos_board_present(canvas);
}

static void vocab_note_full_refresh(void) { vu_full_requested = true; }

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
    if (vu_pending_audio) {
        vu_pending_audio = 0;
        vocab_play_stage_audio();
    }
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
    vu.hint_shown = false;
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
/* Record the answer without leaving the quiz (used when a wrong option is
   tapped and the options reveal in place). */
static bool vocab_answer_now(vocab_grade_t grade) {
    uint32_t now = vocab_now_min();
    if (!vocab_answer(now, grade)) return false;
    if (vu_hist_count == 0 || vu_history[(vu_hist_pos + 7) % 8] != vu.card.id) {
        vu_history[vu_hist_pos] = vu.card.id;
        vu_hist_grade[vu_hist_pos] = (uint8_t)grade;
        vu_hist_pos = (uint8_t)((vu_hist_pos + 1) % 8);
        if (vu_hist_count < 8) vu_hist_count++;
    }
    vocab_stats(now, &vu.stats);
    return true;
}

static bool vocab_hold(int x, int y, uint64_t elapsed) {
    if (app.page != THINGSOS_UI_VOCAB || vu.failed) return false;
    if (vu.screen != VOCAB_SCREEN_LEARN && vu.screen != VOCAB_SCREEN_REVIEW) return false;
    if (vu.phase != VOCAB_PHASE_QUESTION || vu.stage != VOCAB_STAGE_QUIZ || vu.revealed) return false;
    if (elapsed < 300 || x < 36 || x >= 648) return false;
    const int idx = vocab_option_index(y);
    if (idx < 0 || idx >= (int)vu.option_count) return false;
    vu.revealed = true;
    vu.answered = true;
    vu.answer_ok = (uint8_t)idx == vu.answer_at;
    vu.graded = false;
    vu.pending_grade = vu.answer_ok ? VOCAB_GOOD : VOCAB_AGAIN;
    if (app.haptic_enabled) thingsos_board_haptic(25);
    app.dirty = true;
    return true;
}

static void vocab_submit(vocab_grade_t grade) {
    uint32_t now = vocab_now_min();
    const uint32_t learned_before = vocab_learned_count();
    if (!vocab_answer(now, grade)) return;
    if (vu_hist_count == 0 || vu_history[(vu_hist_pos + 7) % 8] != vu.card.id) {
        vu_history[vu_hist_pos] = vu.card.id;
        vu_hist_grade[vu_hist_pos] = (uint8_t)grade;
        vu_hist_pos = (uint8_t)((vu_hist_pos + 1) % 8);
        if (vu_hist_count < 8) vu_hist_count++;
    }
    const bool graduated = vocab_learned_count() > learned_before;
    vocab_stats(now, &vu.stats);

    if (vu.screen == VOCAB_SCREEN_REVIEW) {
        /* Forgot: repeat it at the end of this review session. Hard only
           shortens the interval; it does not start a relearn round. */
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
    vu_pending_audio = 1;   /* played from the loop after this frame is shown */
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
    const uint8_t state = vocab_current_state();
    const char* name = state == 0 ? "新词" : state == 1 ? "学习中" :
                       state == 2 ? "复习" : state == 3 ? "重学" : "学习";
    if (phase == 2)
        snprintf(line, sizeof(line), "%s · %u/%u", name, (unsigned)index, (unsigned)total);
    else
        snprintf(line, sizeof(line), "%s · 第%u轮 · %u/%u", name, (unsigned)round,
                 (unsigned)index, (unsigned)total);
    label(line, 36, 1070, 22, 520);
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
    /* due_today already excludes cards answered today (their next due is in
       the future), so it is the remaining review count itself. */
    const unsigned review_left = vu.stats.due_today;
    char line[64];
    snprintf(line, sizeof(line), "连续 %u 天 · 已学 %u / %u", (unsigned)vu.stats.streak,
             (unsigned)vocab_learned_count(), (unsigned)vocab_count());
    label(line, 36, 830, 24, 612);
    app_outline(36, 900, 300, 180);
    app_outline(348, 900, 300, 180);
    label("学习", 64, 962, 40, 240);
    if (!learn_left && vu.stats.new_today) label("已完成", 64, 1046, 34, 240);
    else { snprintf(line, sizeof(line), "%u", learn_left); label(line, 64, 1050, 60, 240); }
    label("复习", 376, 962, 40, 240);
    if (!review_left && vu.stats.due_today) label("已完成", 376, 1046, 34, 240);
    else { snprintf(line, sizeof(line), "%u", review_left); label(line, 376, 1050, 60, 240); }
}

static const char* const vocab_setting_names[10] = {
    "每日新词目标", "每组词数", "目标保持率", "学习算法",
    "随机选词", "自动发音", "朗读例句", "答错处理", "清空学习进度", "恢复默认设置",
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
    for (int i = 0; i < 10; i++) {
        const int y = 236 + i * 70;
        label(vocab_setting_names[i], 36, y, 26, 360);
        value[0] = 0;
        switch (i) {
            case 0: snprintf(value, sizeof(value), "%u", (unsigned)vu.config.new_limit); break;
            case 1: snprintf(value, sizeof(value), "%u", (unsigned)vu.config.group_size); break;
            case 2: snprintf(value, sizeof(value), "%u%%", (unsigned)vu.config.target_percent); break;
            default: break;
        }
        if (i < 3) {
            thingsos_font_size(&app.font, 26);
            thingsos_label_right(&app.font, &app.canvas, value, 36, y, 500);
            vocab_button(548, y - 34, 44, 48, "－");
            vocab_button(600, y - 34, 44, 48, "＋");
        } else if (i <= 7) {
            const char* opt_a = "关";
            const char* opt_b = "开";
            unsigned selected = 0;
            switch (i) {
                case 3: opt_a = "经典"; opt_b = "FSRS";
                        selected = vu.config.mode == VOCAB_MODE_CLASSIC ? 0 : 1; break;
                case 4: selected = vu.config.shuffle ? 1 : 0; break;
                case 5: selected = vu.config.auto_audio ? 1 : 0; break;
                case 6: selected = vu.config.auto_example ? 1 : 0; break;
                case 7: opt_a = "组末"; opt_b = "本轮"; selected = vu.config.error_mode ? 1 : 0; break;
            }
            for (unsigned seg = 0; seg < 2; seg++) {
                const int bx = 382 + (int)seg * 136;
                app_outline(bx, y - 34, 128, 48);
                centered_label(seg == 0 ? opt_a : opt_b, bx, y + 2, 26, 128);
                if (seg == selected) {
                    /* Selected option: inner frame plus a filled marker.
                       Drawing-only, so it cannot land outside the box. */
                    app_outline(bx + 4, y - 30, 120, 40);
                    thingsos_rect(&app.canvas, bx + 10, y - 9, 10, 10, 0);
                }
            }
        } else {
            vocab_button(470, y - 34, 178, 48, i == 8 ? "清 空" : "恢 复");
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
    if (vu.card.phonetic[0]) {
        char ipa[96];
        label(vocab_phonetic_text(ipa, sizeof(ipa), vu.card.phonetic), 36, 308, 26, 612);
    }
    thingsos_rect(&app.canvas, 36, 330, 612, 1, 0);
    char second[VOCAB_MEANING_BYTES + 16];
    for (unsigned i = 0; i < vu.option_count; i++) {
        const int y = VOCAB_OPTION_TOP + (int)i * VOCAB_OPTION_PITCH;
        const bool marked = vu.answered && (uint8_t)i == vu.answer_at;
        app_outline(36, y, 612, VOCAB_OPTION_HEIGHT);
        if (marked) app_outline(40, y + 4, 604, VOCAB_OPTION_HEIGHT - 8);
        const char* first = vu.revealed ? vu.option_words[i] : vu.option_pos[i];
        if (vu.revealed && vu.option_pos[i][0])
            snprintf(second, sizeof(second), "%s %s", vu.option_mean[i], vu.option_pos[i]);
        else
            snprintf(second, sizeof(second), "%s", vu.option_mean[i]);
        label(first, 60, y + 66, 34, 556);
        label(second, 60, y + 126, 28, 556);
    }
    app_outline(36, 1102, 612, 90);
    centered_label(vu.revealed ? "继 续" : "看 答 案", 36, 1102 + 57, 34, 612);
}

static void vocab_draw_recall(void) {
    vocab_draw_round_progress();
    vocab_undo_icon();
    vocab_header(vu.screen == VOCAB_SCREEN_REVIEW ? "复习" : "学习");
    label(vu.card.word, 36, 258, 64, 612);
    if (vu.card.phonetic[0]) {
        char ipa[96];
        label(vocab_phonetic_text(ipa, sizeof(ipa), vu.card.phonetic), 36, 308, 26, 612);
    }
    thingsos_rect(&app.canvas, 36, 330, 612, 1, 0);
    thingsos_rect(&app.canvas, 36, 566, 612, 1, 90);
    if (vu.stage == VOCAB_STAGE_RECALL) {
        /* Round 2: the example is always visible; 提示 only reveals the gloss. */
        int lines = 0;
        if (vu.card.example_en[0])
            lines = vocab_draw_wrapped(vu.card.example_en, 36, 622, 32, 4);
        if (vu.hint_shown && vu.card.example_zh[0])
            vocab_draw_wrapped(vu.card.example_zh, 36, 622 + lines * 40 + 6, 32, 3);
        vocab_button(36, 1102, 190, 90, "认识");
        vocab_button(247, 1102, 190, 90, "提示");
        vocab_button(458, 1102, 190, 90, "不认识");
    } else {
        /* Round 3: bare word only, no example. */
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
    if (app.force_refresh) vu_full_requested = true;
    if (app.page != THINGSOS_UI_VOCAB) {
        if (vu.started) vocab_stop();
        return false;
    }
    /* Follow the system font size, clamped so the fixed card geometry never
       overlaps: 85%..115% of the design size. Only this page is affected. */
    if (app.font.ui_size_percent > 115) app.font.ui_size_percent = 115;
    if (app.font.ui_size_percent < 85) app.font.ui_size_percent = 85;
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

static void vocab_return_from_undo(void) {
    if (!vocab_load_by_id(vu.undo_return_id, &vu.card, &vu.stage)) return;
    vu.screen = vu.undo_screen;
    vu.phase = vu.undo_phase;
    vu.undo_active = false;
    app.dirty = true;
}

static void vocab_undo_toggle(void) {
    if (vu.undo_active) { vocab_return_from_undo(); return; }
    if (vu_hist_count < 2) return;
    const uint8_t newest_slot = (uint8_t)((vu_hist_pos + 7) % 8);
    const uint8_t prev = vu_history[newest_slot] == vu.card.id
                             ? (uint8_t)((vu_hist_pos + 8 - 2) % 8)
                             : newest_slot;
    const uint32_t prev_id = vu_history[prev];
    if (prev_id == vu.card.id) return;
    vu.undo_return_id = vu.card.id;
    vu.undo_screen = vu.screen;
    vu.undo_phase = vu.phase;
    vu.undo_stage = vu.stage;
    vu.undo_card_id = prev_id;
    vu.undo_prev_grade = vu_hist_grade[prev];
    if (!vocab_load_by_id(prev_id, &vu.card, &vu.stage)) return;
    vu.phase = VOCAB_PHASE_ANSWER;
    vu.undo_active = true;
    app.dirty = true;
}

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
        if (y >= 900 && y < 1080) {
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
        const int row = (y - 196) / 70;
        if (row >= 0 && row < 10 && x >= 380) {
            const bool plus = x >= 600;
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
                        int v = (int)vu.config.group_size + (plus ? 5 : -5);
                        if (v < 5) v = 5;
                        if (v > 20) v = 20;
                        vu.config.group_size = (uint8_t)v;
                        /* Not started yet: apply the new group size at once. */
                        vocab_replan_idle(vocab_now_min());
                    } else save = false;
                    break;
                case 2:
                    if (x >= 548) {
                        int v = (int)vu.config.target_percent + (plus ? 5 : -5);
                        if (v < 80) v = 80;
                        if (v > 95) v = 95;
                        vu.config.target_percent = (uint8_t)v;
                    } else save = false;
                    break;
                case 3:
                    vu.config.mode = (x >= 518) ? VOCAB_MODE_FSRS : VOCAB_MODE_CLASSIC;
                    vocab_abandon_plan();
                    break;
                case 4:
                    vu.config.shuffle = (x >= 518);
                    vocab_abandon_plan();
                    break;
                case 5: vu.config.auto_audio = (x >= 518); break;
                case 6: vu.config.auto_example = (x >= 518); break;
                case 7: vu.config.error_mode = (x >= 518) ? 1 : 0; break;
                case 8: vu_confirm_clear = true; save = false; break;
                case 9: vocab_config_defaults(&vu.config); break;
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

    if ((vu.screen == VOCAB_SCREEN_LEARN || vu.screen == VOCAB_SCREEN_REVIEW) &&
        x >= 580 && y >= 74 && y < 205) {
        vocab_undo_toggle();
        return true;
    }
    if (vu.phase == VOCAB_PHASE_ANSWER) {
        if (vu_peeking) return true;
        if (y >= 600 && y < 780 && vu.card.example_en[0]) {
            char example[256];
            if (vocab_example_audio_path(vu.card.id, example, sizeof(example)))
                vocab_play_file(example);
            return true;
        }
        if (y >= 1102 && y < 1196) {
            if (vu.undo_active) {
                const uint32_t now = vocab_now_min();
                if (x >= 358) {
                    if (vu.undo_prev_grade != VOCAB_AGAIN)
                        vocab_regrade_by_id(now, vu.undo_card_id, VOCAB_AGAIN);
                } else {
                    if (vu.undo_prev_grade == VOCAB_AGAIN)
                        vocab_regrade_by_id(now, vu.undo_card_id, VOCAB_GOOD);
                }
                vocab_return_from_undo();
                return true;
            }
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
        const int idx = vocab_option_index(y);
        const bool hit = idx >= 0 && idx < (int)vu.option_count && x >= 36 && x < 648;
        if (!vu.revealed) {
            if (hit) {
                if ((uint8_t)idx == vu.answer_at) {
                    vu.answered = true;
                    vu.answer_ok = true;
                    app.dirty = true;
                    vocab_submit(VOCAB_GOOD);   /* correct: straight to the answer */
                } else {
                    vu.revealed = true;         /* wrong: stay and reveal */
                    vu.answered = true;
                    vu.answer_ok = false;
                    vu.pending_grade = VOCAB_AGAIN;
                    if (!vu.graded) { vocab_answer_now(VOCAB_AGAIN); vu.graded = true; }
                    app.dirty = true;
                }
            } else if (y >= 1102 && y < 1196) {
                vocab_submit(VOCAB_AGAIN);      /* 看答案 counts as "did not know" */
            }
        } else {
            if (hit && (uint8_t)idx == vu.answer_at) {
                if (!vu.graded) { vu.graded = true; vocab_submit(VOCAB_GOOD); }
                else { vu.phase = VOCAB_PHASE_ANSWER; app.dirty = true; }
            } else if (y >= 1102 && y < 1196) {
                if (!vu.graded) {
                    vu.graded = true;
                    vocab_submit(vu.pending_grade == VOCAB_GOOD ? VOCAB_GOOD : VOCAB_AGAIN);
                } else {
                    vu.phase = VOCAB_PHASE_ANSWER;
                    app.dirty = true;
                }
            }
        }
        return true;
    }

    /* Grade row sits at the bottom band, same geometry as the drawn buttons. */
    if (y >= 1102 && y < 1196) {
        if (vu.stage == VOCAB_STAGE_RECALL) {
            if (x >= 458) vocab_submit(VOCAB_AGAIN);
            else if (x >= 247) {
                if (!vu.hint_shown) { vu.hint_shown = true; app.dirty = true; }
            } else vocab_submit(VOCAB_GOOD);
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
        /* 上页 = 学习, 下页 = 复习. */
        if (action == THINGSOS_KEY_PREVIOUS) vocab_start_learn();
        else if (action == THINGSOS_KEY_NEXT) vocab_start_review();
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

