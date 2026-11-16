// SPDX-License-Identifier: Apache-2.0
/* Word-card bank, FSRS session and durable progress.
 *
 * Files under <root>/.thingsos/vocab/
 *   bank.bin        immutable word bank (produced by vocab_pack.py)
 *   state.dat       snapshot: 24-byte header + one 28-byte record per card + CRC
 *   journal.dat     append-only (id + record) log replayed over the snapshot
 *   config.bin      16-byte settings block
 *   review-log.csv  append-only review history for offline weight training
 */
#include "thingsos/vocab.h"
#include "thingsos/vocab_fsrs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VOCAB_STATE_MAGIC "VOCS"
#define VOCAB_BANK_MAGIC "VOCB"
#define VOCAB_CONFIG_MAGIC "VOCC"
#define VOCAB_VERSION 1u
#define VOCAB_INDEX_ENTRY 28u
#define VOCAB_JOURNAL_ENTRY 32u
#define VOCAB_CONFIG_BYTES 16u
#define VOCAB_UNDO_DEPTH 16u
#define VOCAB_STATE_HEADER 24u
#define VOCAB_LEARN_AGAIN_MIN 1u
#define VOCAB_LEARN_GOOD_MIN 10u
#define VOCAB_RELEARN_MIN 10u
#define VOCAB_GRADUATE_MIN_DAYS 4u

typedef struct {
    FILE *file;
    uint32_t count;
    uint32_t index_off;
    uint32_t strings_off;
    uint32_t bank_hash;
} vocab_bank_t;

typedef struct {
    uint32_t word_off;
    uint32_t meaning_off;
    uint32_t example_en_off;
    uint32_t example_zh_off;
    uint32_t root_off;
    uint16_t phonetic_off;
    uint16_t group_off;
} vocab_index_t;

typedef struct {
    uint32_t id;
    vocab_state_record_t record;
    uint32_t queue_pos;
    uint16_t new_today;
    uint16_t reviews_today;
} vocab_undo_t;

_Static_assert(sizeof(vocab_state_record_t) == 28, "state record layout");

static vocab_bank_t g_bank;
static vocab_state_record_t *g_records;
static uint32_t g_count;
static uint32_t g_bank_hash;
static char g_root[160];
static bool g_open;

static uint32_t *g_queue;
static uint32_t g_queue_len;
static uint32_t g_queue_pos;
static uint32_t g_current;
static bool g_current_valid;
static bool g_revealed;
static vocab_card_stage_t g_stage;

static uint32_t g_now;
static uint32_t g_last_day;
static uint16_t g_streak;
static uint16_t g_new_today;
static uint16_t g_reviews_today;
static vocab_config_t g_config;

static vocab_undo_t g_undo[VOCAB_UNDO_DEPTH];
static unsigned g_undo_count;

/* ---------------------------------------------------------------- utility */

static uint32_t crc32_bytes(const void *data, size_t bytes) {
    const unsigned char *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (bytes--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

uint32_t vocab_date_to_day(uint32_t yyyymmdd) {
    uint32_t y = yyyymmdd / 10000u, m = yyyymmdd / 100u % 100u, d = yyyymmdd % 100u;
    if (y < 2020u) y = 2020u;
    if (m < 1u) m = 1u;
    if (m > 12u) m = 12u;
    if (d < 1u) d = 1u;
    if (d > 31u) d = 31u;
    y -= (m <= 2u);
    const uint32_t era = y / 400u;
    const uint32_t yoe = y - era * 400u;
    const uint32_t doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
    const uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const uint32_t days = era * 146097u + doe; /* days since 0000-era epoch */
    const uint32_t epoch = 719468u + 18262u;   /* 1970-01-01 + 50 years */
    return days >= epoch ? days - epoch : 0u;
}

uint32_t vocab_day_to_date(uint32_t day) {
    uint32_t z = day + 18262u + 719468u;
    const uint32_t era = z / 146097u;
    const uint32_t doe = z - era * 146097u;
    const uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint32_t y = yoe + era * 400u;
    const uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const uint32_t mp = (5u * doy + 2u) / 153u;
    const uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
    const uint32_t m = mp < 10u ? mp + 3u : mp - 9u;
    y += (m <= 2u);
    return y * 10000u + m * 100u + d;
}

uint32_t vocab_minutes_from_epoch(uint32_t yyyymmdd, unsigned hour, unsigned minute) {
    return vocab_date_to_day(yyyymmdd) * VOCAB_MINUTE_PER_DAY + (hour % 24u) * 60u +
           (minute % 60u);
}

static void path_of(char *buf, size_t bytes, const char *name) {
    snprintf(buf, bytes, "%s/.thingsos/vocab/%s", g_root, name);
}

static bool ensure_dir(const char *path) {
    if (mkdir(path, 0775) == 0) return true;
    return errno == EEXIST;
}

static bool ensure_dirs(void) {
    char path[256];
    snprintf(path, sizeof(path), "%s/.thingsos", g_root);
    if (!ensure_dir(path)) return false;
    snprintf(path, sizeof(path), "%s/.thingsos/vocab", g_root);
    if (!ensure_dir(path)) return false;
    snprintf(path, sizeof(path), "%s/.thingsos/vocab/audio", g_root);
    return ensure_dir(path);
}

/* ------------------------------------------------------------------ config */

void vocab_config_defaults(vocab_config_t *cfg) {
    if (!cfg) return;
    cfg->mode = VOCAB_MODE_FSRS;
    cfg->target_percent = 90;
    cfg->new_limit = 20;
    cfg->review_limit = 200;
    cfg->auto_audio = true;
}

bool vocab_config_load(const char *root, vocab_config_t *cfg) {
    vocab_config_defaults(cfg);
    char path[256];
    snprintf(path, sizeof(path), "%s/.thingsos/vocab/config.bin", root);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char raw[VOCAB_CONFIG_BYTES];
    size_t got = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (got != sizeof(raw) || memcmp(raw, VOCAB_CONFIG_MAGIC, 4) != 0) return false;
    uint32_t crc = 0;
    memcpy(&crc, raw + 12, 4);
    if (crc != crc32_bytes(raw, 12)) return false;
    cfg->mode = (vocab_mode_t)raw[4];
    cfg->target_percent = raw[5];
    uint16_t value = 0;
    memcpy(&value, raw + 6, 2);
    cfg->new_limit = value;
    memcpy(&value, raw + 8, 2);
    cfg->review_limit = value;
    cfg->auto_audio = raw[10] != 0;
    if (cfg->mode != VOCAB_MODE_CLASSIC) cfg->mode = VOCAB_MODE_FSRS;
    if (cfg->target_percent < 80 || cfg->target_percent > 95) cfg->target_percent = 90;
    if (cfg->new_limit == 0 || cfg->new_limit > 200) cfg->new_limit = 20;
    if (cfg->review_limit < 20 || cfg->review_limit > 2000) cfg->review_limit = 200;
    return true;
}

bool vocab_config_save(const char *root, const vocab_config_t *cfg) {
    char path[256];
    snprintf(path, sizeof(path), "%s/.thingsos/vocab/config.bin", root);
    unsigned char raw[VOCAB_CONFIG_BYTES] = {0};
    memcpy(raw, VOCAB_CONFIG_MAGIC, 4);
    raw[4] = (unsigned char)cfg->mode;
    raw[5] = cfg->target_percent;
    memcpy(raw + 6, &cfg->new_limit, 2);
    memcpy(raw + 8, &cfg->review_limit, 2);
    raw[10] = cfg->auto_audio ? 1 : 0;
    uint32_t crc = crc32_bytes(raw, 12);
    memcpy(raw + 12, &crc, 4);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(raw, 1, sizeof(raw), f) == sizeof(raw);
    fflush(f);
    fclose(f);
    return ok;
}

/* -------------------------------------------------------------------- bank */

static bool read_string(uint32_t offset, char *buf, size_t bytes) {
    if (!buf || bytes == 0) return false;
    buf[0] = 0;
    if (!g_bank.file) return false;
    if (fseek(g_bank.file, (long)(g_bank.strings_off + offset), SEEK_SET) != 0) return false;
    size_t used = 0;
    for (;;) {
        int ch = fgetc(g_bank.file);
        if (ch == EOF || ch == 0) break;
        if (used + 1 < bytes) buf[used++] = (char)ch;
    }
    buf[used] = 0;
    return true;
}

static bool read_index(uint32_t id, vocab_index_t *index) {
    if (!g_bank.file || id >= g_bank.count) return false;
    if (fseek(g_bank.file, (long)(g_bank.index_off + id * VOCAB_INDEX_ENTRY), SEEK_SET) != 0)
        return false;
    unsigned char raw[VOCAB_INDEX_ENTRY];
    if (fread(raw, 1, sizeof(raw), g_bank.file) != sizeof(raw)) return false;
    memset(index, 0, sizeof(*index));
    memcpy(&index->word_off, raw + 0, 4);
    memcpy(&index->meaning_off, raw + 4, 4);
    memcpy(&index->example_en_off, raw + 8, 4);
    memcpy(&index->example_zh_off, raw + 12, 4);
    memcpy(&index->root_off, raw + 16, 4);
    memcpy(&index->phonetic_off, raw + 20, 2);
    memcpy(&index->group_off, raw + 22, 2);
    return true;
}

static bool bank_open(void) {
    char path[256];
    path_of(path, sizeof(path), "bank.bin");
    g_bank.file = fopen(path, "rb");
    if (!g_bank.file) return false;
    unsigned char head[32];
    if (fread(head, 1, sizeof(head), g_bank.file) != sizeof(head)) return false;
    if (memcmp(head, VOCAB_BANK_MAGIC, 4) != 0) return false;
    uint16_t version = 0, count = 0;
    memcpy(&version, head + 4, 2);
    memcpy(&count, head + 6, 2);
    if (version != VOCAB_VERSION || count == 0 || count > 20000u) return false;
    memcpy(&g_bank.index_off, head + 8, 4);
    memcpy(&g_bank.strings_off, head + 12, 4);
    memcpy(&g_bank.bank_hash, head + 16, 4);
    uint32_t stored = 0;
    memcpy(&stored, head + 28, 4);
    if (stored != crc32_bytes(head, 28)) return false;
    g_bank.count = count;
    return true;
}

static void bank_close(void) {
    if (g_bank.file) fclose(g_bank.file);
    memset(&g_bank, 0, sizeof(g_bank));
}

/* ------------------------------------------------------------------- state */

static void state_path(char *buf, size_t bytes) { path_of(buf, bytes, "state.dat"); }
static void journal_path(char *buf, size_t bytes) { path_of(buf, bytes, "journal.dat"); }

static void record_default(vocab_state_record_t *record) {
    memset(record, 0, sizeof(*record));
    record->state = VOCAB_STATE_NEW;
}

static bool state_load(void) {
    g_count = g_bank.count;
    g_records = calloc(g_count, sizeof(vocab_state_record_t));
    if (!g_records) return false;
    for (uint32_t i = 0; i < g_count; i++) record_default(&g_records[i]);

    char path[256];
    state_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return true;
    unsigned char head[VOCAB_STATE_HEADER];
    if (fread(head, 1, sizeof(head), f) != sizeof(head) ||
        memcmp(head, VOCAB_STATE_MAGIC, 4) != 0) {
        fclose(f);
        return true;
    }
    uint16_t version = 0, stored_count = 0, streak = 0, new_today = 0, reviews = 0;
    uint32_t last_day = 0, bank_hash = 0;
    memcpy(&version, head + 4, 2);
    memcpy(&stored_count, head + 6, 2);
    memcpy(&last_day, head + 8, 4);
    memcpy(&streak, head + 12, 2);
    memcpy(&new_today, head + 14, 2);
    memcpy(&reviews, head + 16, 2);
    memcpy(&bank_hash, head + 20, 4);
    if (version != VOCAB_VERSION || stored_count != (uint16_t)g_count ||
        bank_hash != g_bank_hash) {
        fclose(f);
        return true;
    }
    size_t bytes = (size_t)g_count * sizeof(vocab_state_record_t);
    vocab_state_record_t *tmp = malloc(bytes);
    if (!tmp) {
        fclose(f);
        return false;
    }
    size_t got = fread(tmp, 1, bytes, f);
    uint32_t stored_crc = 0;
    bool ok = got == bytes && fread(&stored_crc, 1, 4, f) == 4 &&
              stored_crc == crc32_bytes(tmp, bytes);
    fclose(f);
    if (ok) {
        memcpy(g_records, tmp, bytes);
        g_last_day = last_day;
        g_streak = streak;
        g_new_today = new_today;
        g_reviews_today = reviews;
    }
    free(tmp);
    return true;
}

static bool state_save(void) {
    char path[256], journal[256], tmp_path[320];
    state_path(path, sizeof(path));
    journal_path(journal, sizeof(journal));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    size_t bytes = (size_t)g_count * sizeof(vocab_state_record_t);
    unsigned char head[VOCAB_STATE_HEADER] = {0};
    memcpy(head, VOCAB_STATE_MAGIC, 4);
    uint16_t version = VOCAB_VERSION, count = (uint16_t)g_count;
    memcpy(head + 4, &version, 2);
    memcpy(head + 6, &count, 2);
    memcpy(head + 8, &g_last_day, 4);
    memcpy(head + 12, &g_streak, 2);
    memcpy(head + 14, &g_new_today, 2);
    memcpy(head + 16, &g_reviews_today, 2);
    memcpy(head + 20, &g_bank_hash, 4);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;
    bool ok = fwrite(head, 1, sizeof(head), f) == sizeof(head) &&
              fwrite(g_records, 1, bytes, f) == bytes;
    uint32_t crc = ok ? crc32_bytes(g_records, bytes) : 0;
    ok = ok && fwrite(&crc, 1, 4, f) == 4;
    fflush(f);
    fclose(f);
    if (!ok) {
        remove(tmp_path);
        return false;
    }
    remove(path);
    if (rename(tmp_path, path) != 0) return false;
    FILE *j = fopen(journal, "wb");
    if (j) fclose(j);
    return true;
}

static void journal_append(uint32_t id, const vocab_state_record_t *record) {
    char path[256];
    journal_path(path, sizeof(path));
    FILE *f = fopen(path, "ab");
    if (!f) return;
    unsigned char raw[VOCAB_JOURNAL_ENTRY] = {0};
    memcpy(raw, &id, 4);
    memcpy(raw + 4, record, sizeof(*record));
    fwrite(raw, 1, sizeof(raw), f);
    fflush(f);
    fclose(f);
}

static void journal_replay(void) {
    char path[256];
    journal_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char raw[VOCAB_JOURNAL_ENTRY];
    while (fread(raw, 1, sizeof(raw), f) == sizeof(raw)) {
        uint32_t id = 0;
        memcpy(&id, raw, 4);
        if (id >= g_count) continue;
        memcpy(&g_records[id], raw + 4, sizeof(vocab_state_record_t));
    }
    fclose(f);
}

static void log_review(uint32_t id, int grade, uint32_t now_min,
                       const vocab_state_record_t *record) {
    char path[256];
    path_of(path, sizeof(path), "review-log.csv");
    bool fresh = access(path, F_OK) != 0;
    FILE *f = fopen(path, "a");
    if (!f) return;
    if (fresh) fputs("id,day,minute,grade,elapsed_days,stability,difficulty\n", f);
    fprintf(f, "%u,%u,%u,%d,%u,%.4f,%.4f\n", (unsigned)id,
            (unsigned)(now_min / VOCAB_MINUTE_PER_DAY),
            (unsigned)(now_min % VOCAB_MINUTE_PER_DAY), grade,
            (unsigned)record->elapsed, (double)record->stability,
            (double)record->difficulty);
    fclose(f);
}

/* -------------------------------------------------------------- scheduling */

static uint32_t graduate_days(const vocab_config_t *cfg, float stability) {
    float days = vocab_fsrs_interval(stability, (float)cfg->target_percent / 100.0f);
    if (days < 1.0f) days = 1.0f;
    return (uint32_t)days;
}

static void schedule_record(vocab_state_record_t *record, vocab_grade_t grade,
                            uint32_t now_min, const vocab_config_t *cfg) {
    const uint32_t day = now_min / VOCAB_MINUTE_PER_DAY;
    uint32_t elapsed_min = record->last_min && now_min > record->last_min
                               ? now_min - record->last_min
                               : 0;
    uint16_t elapsed_days = (uint16_t)(elapsed_min / VOCAB_MINUTE_PER_DAY);
    const bool same_day = record->last_min != 0 && elapsed_min > 0 &&
                          elapsed_min < VOCAB_MINUTE_PER_DAY;

    if (cfg->mode == VOCAB_MODE_CLASSIC) {
        static const uint16_t table[8] = {1, 1, 2, 4, 7, 15, 30, 60};
        unsigned step = record->reps;
        if (record->state == VOCAB_STATE_NEW) step = 0;
        uint32_t days;
        if (grade == VOCAB_AGAIN) {
            step = 0;
            record->state = record->state == VOCAB_STATE_REVIEW ||
                                    record->state == VOCAB_STATE_RELEARNING
                                ? VOCAB_STATE_RELEARNING
                                : VOCAB_STATE_LEARNING;
            record->due_min = now_min + VOCAB_RELEARN_MIN;
            record->lapses++;
            days = 0;
        } else {
            if (grade == VOCAB_HARD) {
                days = table[step < 8 ? step : 7] > 1 ? table[step < 8 ? step : 7] / 2 : 1;
            } else if (grade == VOCAB_GOOD) {
                step = step + 1 > 7 ? 7 : step + 1;
                days = table[step];
            } else {
                step = step + 2 > 7 ? 7 : step + 2;
                days = table[step];
            }
            record->state = VOCAB_STATE_REVIEW;
            record->due_min = now_min + days * VOCAB_MINUTE_PER_DAY;
        }
        record->reps = (uint16_t)step;
        record->last_min = now_min;
        record->elapsed = elapsed_days;
        return;
    }

    vocab_fsrs_memory_t memory = {record->stability, record->difficulty};
    if (record->state == VOCAB_STATE_NEW) {
        vocab_fsrs_initial((int)grade, &memory);
    } else {
        float elapsed = same_day ? 0.0f : (elapsed_days ? (float)elapsed_days : 1.0f);
        vocab_fsrs_review(&memory, elapsed, (int)grade, same_day ? 1 : 0);
    }

    const bool learning = record->state == VOCAB_STATE_NEW ||
                          record->state == VOCAB_STATE_LEARNING ||
                          record->state == VOCAB_STATE_RELEARNING;
    if (learning) {
        if (grade == VOCAB_AGAIN) {
            record->state = record->state == VOCAB_STATE_NEW ? VOCAB_STATE_LEARNING
                                                             : VOCAB_STATE_RELEARNING;
            record->due_min = now_min + VOCAB_LEARN_AGAIN_MIN;
            record->lapses++;
        } else if (grade == VOCAB_HARD) {
            record->due_min = now_min + VOCAB_LEARN_GOOD_MIN / 2;
            if (record->state == VOCAB_STATE_NEW) record->state = VOCAB_STATE_LEARNING;
        } else if (grade == VOCAB_GOOD && record->state == VOCAB_STATE_NEW) {
            record->state = VOCAB_STATE_LEARNING;
            record->due_min = now_min + VOCAB_LEARN_GOOD_MIN;
        } else {
            record->state = VOCAB_STATE_REVIEW;
            record->due_min =
                now_min + graduate_days(cfg, memory.stability) * VOCAB_MINUTE_PER_DAY;
        }
    } else if (grade == VOCAB_AGAIN) {
        record->state = VOCAB_STATE_RELEARNING;
        record->lapses++;
        record->due_min = now_min + VOCAB_RELEARN_MIN;
    } else {
        record->due_min =
            now_min + graduate_days(cfg, memory.stability) * VOCAB_MINUTE_PER_DAY;
    }

    record->stability = memory.stability;
    record->difficulty = memory.difficulty;
    record->reps++;
    record->last_min = now_min;
    record->elapsed = elapsed_days;
}

/* ------------------------------------------------------------------ queue */

static void queue_push(uint32_t id) {
    if (!g_queue || g_queue_len >= g_count) return;
    g_queue[g_queue_len++] = id;
}

static void queue_build(uint32_t now_min, const vocab_config_t *cfg) {
    g_queue_len = 0;
    g_queue_pos = 0;
    if (!g_queue || !g_records) return;
    uint32_t new_added = 0, due_added = 0;
    for (uint32_t i = 0; i < g_count; i++) {
        vocab_state_record_t *record = &g_records[i];
        if (record->state != VOCAB_STATE_NEW && record->due_min && record->due_min <= now_min) {
            if (due_added < cfg->review_limit) {
                queue_push(i);
                due_added++;
            }
        }
    }
    for (uint32_t i = 0; i < g_count && new_added < cfg->new_limit; i++) {
        if (g_records[i].state == VOCAB_STATE_NEW) {
            /* Interleave new cards after the first few due cards. */
            queue_push(i);
            new_added++;
        }
    }
}

static bool queue_reinsert(uint32_t id) {
    if (!g_queue || g_queue_len >= g_count) return false;
    uint32_t at = g_queue_pos + 3;
    if (at > g_queue_len) at = g_queue_len;
    memmove(g_queue + at + 1, g_queue + at, (g_queue_len - at) * sizeof(uint32_t));
    g_queue[at] = id;
    g_queue_len++;
    return true;
}

/* ------------------------------------------------------------------- API */

bool vocab_open(const char *root, uint32_t now_min) {
    if (!root || !*root) return false;
    vocab_close(now_min);
    snprintf(g_root, sizeof(g_root), "%s", root);
    g_now = now_min;
    if (!ensure_dirs()) return false;
    if (!bank_open()) {
        bank_close();
        return false;
    }
    g_bank_hash = g_bank.bank_hash;
    if (!state_load()) {
        bank_close();
        free(g_records);
        g_records = NULL;
        return false;
    }
    journal_replay();
    g_queue = calloc(g_count ? g_count : 1, sizeof(uint32_t));
    vocab_config_load(g_root, &g_config);
    g_open = true;
    g_current_valid = false;
    return g_queue != NULL;
}

void vocab_close(uint32_t now_min) {
    if (g_open) {
        g_now = now_min ? now_min : g_now;
        state_save();
    }
    bank_close();
    free(g_records);
    free(g_queue);
    g_records = NULL;
    g_queue = NULL;
    g_count = 0;
    g_queue_len = g_queue_pos = 0;
    g_undo_count = 0;
    g_open = false;
}

bool vocab_opened(void) { return g_open; }
uint32_t vocab_count(void) { return g_count; }

bool vocab_begin_day(uint32_t now_min, const vocab_config_t *cfg) {
    if (!g_open) return false;
    if (cfg) g_config = *cfg;
    g_now = now_min;
    const uint32_t day = now_min / VOCAB_MINUTE_PER_DAY;
    if (day != g_last_day) {
        g_streak = (g_last_day && day == g_last_day + 1) ? (uint16_t)(g_streak + 1) : 1;
        g_last_day = day;
        g_new_today = 0;
        g_reviews_today = 0;
    }
    queue_build(now_min, &g_config);
    return true;
}

bool vocab_next(vocab_card_t *out, vocab_card_stage_t *stage) {
    if (!g_open || !out || !g_queue) return false;
    while (g_queue_pos < g_queue_len) {
        uint32_t id = g_queue[g_queue_pos];
        if (id >= g_count) {
            g_queue_pos++;
            continue;
        }
        vocab_state_record_t *record = &g_records[id];
        if (record->state != VOCAB_STATE_NEW && record->due_min && record->due_min > g_now) {
            g_queue_pos++;
            continue;
        }
        g_current = id;
        g_current_valid = true;
        g_revealed = false;

        vocab_index_t index;
        if (!read_index(id, &index)) {
            g_queue_pos++;
            continue;
        }
        memset(out, 0, sizeof(*out));
        out->id = id;
        read_string(index.word_off, out->word, sizeof(out->word));
        read_string(index.phonetic_off, out->phonetic, sizeof(out->phonetic));
        read_string(index.meaning_off, out->meaning, sizeof(out->meaning));
        read_string(index.example_en_off, out->example_en, sizeof(out->example_en));
        read_string(index.example_zh_off, out->example_zh, sizeof(out->example_zh));
        read_string(index.root_off, out->root, sizeof(out->root));
        read_string(index.group_off, out->group, sizeof(out->group));

        if (record->state == VOCAB_STATE_NEW) {
            g_stage = VOCAB_STAGE_WORD;
        } else if (record->state == VOCAB_STATE_REVIEW && (record->reps % 3u) == 1u) {
            g_stage = VOCAB_STAGE_AUDIO;
        } else {
            g_stage = VOCAB_STAGE_RECALL;
        }
        if (stage) *stage = g_stage;
        return true;
    }
    g_current_valid = false;
    return false;
}

bool vocab_answer(uint32_t now_min, vocab_grade_t grade) {
    if (!g_open || !g_current_valid) return false;
    if (grade < VOCAB_AGAIN || grade > VOCAB_EASY) return false;
    g_now = now_min;
    vocab_state_record_t *record = &g_records[g_current];
    const bool was_new = record->state == VOCAB_STATE_NEW;

    vocab_undo_t *undo = &g_undo[g_undo_count % VOCAB_UNDO_DEPTH];
    undo->id = g_current;
    undo->record = *record;
    undo->queue_pos = g_queue_pos;
    undo->new_today = g_new_today;
    undo->reviews_today = g_reviews_today;
    g_undo_count++;

    schedule_record(record, grade, now_min, &g_config);
    journal_append(g_current, record);
    log_review(g_current, (int)grade, now_min, record);
    g_queue_pos++;
    if (was_new && g_new_today < 0xFFFF) g_new_today++;
    if (g_reviews_today < 0xFFFF) g_reviews_today++;

    if (record->state == VOCAB_STATE_LEARNING || record->state == VOCAB_STATE_RELEARNING)
        queue_reinsert(g_current);
    g_current_valid = false;
    return true;
}

bool vocab_undo(uint32_t now_min) {
    if (!g_open || g_undo_count == 0) return false;
    g_undo_count--;
    vocab_undo_t *undo = &g_undo[g_undo_count % VOCAB_UNDO_DEPTH];
    if (undo->id >= g_count) return false;
    g_records[undo->id] = undo->record;
    g_queue_pos = undo->queue_pos;
    g_new_today = undo->new_today;
    g_reviews_today = undo->reviews_today;
    g_current = undo->id;
    g_current_valid = true;
    g_now = now_min ? now_min : g_now;
    /* Tombstone the last journal entry so a crash cannot replay the undone step. */
    char path[256];
    journal_path(path, sizeof(path));
    FILE *f = fopen(path, "rb+");
    if (f) {
        if (fseek(f, -(long)VOCAB_JOURNAL_ENTRY, SEEK_END) == 0) {
            unsigned char zero[VOCAB_JOURNAL_ENTRY] = {0};
            fwrite(zero, 1, sizeof(zero), f);
            fflush(f);
        }
        fclose(f);
    }
    return true;
}

bool vocab_stats(uint32_t now_min, vocab_stats_t *out) {
    if (!g_open || !out) return false;
    memset(out, 0, sizeof(*out));
    g_now = now_min ? now_min : g_now;
    out->new_today = g_new_today;
    out->reviews_today = g_reviews_today;
    out->streak = g_streak;
    for (uint32_t i = 0; i < g_count; i++) {
        const vocab_state_record_t *record = &g_records[i];
        if (record->state != VOCAB_STATE_NEW) {
            out->learned++;
            if (record->due_min && record->due_min <= g_now) out->due_today++;
        }
        out->total++;
    }
    out->finished = g_queue_pos >= g_queue_len;
    return true;
}

bool vocab_audio_path(uint32_t id, char *buf, size_t bytes) {
    if (!buf || bytes == 0) return false;
    snprintf(buf, bytes, "%s/.thingsos/vocab/audio/%u.mp3", g_root, (unsigned)id);
    return access(buf, R_OK) == 0;
}

uint32_t vocab_current_id(void) { return g_current_valid ? g_current : 0xFFFFFFFFu; }
vocab_card_stage_t vocab_current_stage(void) { return g_stage; }
bool vocab_current_revealed(void) { return g_revealed; }
void vocab_reveal(void) { g_revealed = true; }

bool vocab_export_log(const char *path) {
    if (!path || !*path) return false;
    char source[256];
    path_of(source, sizeof(source), "review-log.csv");
    FILE *in = fopen(source, "rb");
    if (!in) return false;
    FILE *out = fopen(path, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buffer[512];
    size_t got;
    bool ok = true;
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, got, out) != got) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok;
}
