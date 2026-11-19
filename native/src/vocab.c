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
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#define VOCAB_LOG(...) ESP_LOGI("vocab", __VA_ARGS__)
static void *vocab_alloc(size_t count, size_t size) {
    void *mem = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return mem ? mem : calloc(count, size);
}
#else
#define VOCAB_LOG(...) ((void)0)
static void *vocab_alloc(size_t count, size_t size) { return calloc(count, size); }
#endif

#define VOCAB_STATE_MAGIC "VOCS"
#define VOCAB_BANK_MAGIC "VOCB"
#define VOCAB_CONFIG_MAGIC "VOCC"
#define VOCAB_VERSION 1u
#define VOCAB_BANK_VERSION 4u
#define VOCAB_INDEX_ENTRY 40u
#define VOCAB_JOURNAL_ENTRY 32u
#define VOCAB_CONFIG_BYTES 20u
#define VOCAB_MINUTES_2020 26297280u  /* minutes from 1970-01-01 to 2020-01-01 */
#define VOCAB_UNDO_DEPTH 16u
#define VOCAB_STATE_HEADER 32u
#define VOCAB_STATE_VERSION 2u
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
    uint32_t entry_bytes; /* 32 (v3) or 40 (v4) */
} vocab_bank_t;

typedef struct {
    uint32_t word_off;
    uint32_t meaning_off;
    uint32_t example_en_off;
    uint32_t example_zh_off;
    uint32_t root_off;
    uint32_t phonetic_off;
    uint32_t group_off;
    uint32_t pos_off;
    uint32_t derived_off;
    uint32_t phrase_off;
} vocab_index_t;

typedef struct {
    uint32_t id;
    vocab_state_record_t record;   /* state before the answer */
    uint32_t queue_pos;
    uint16_t new_today;
    uint16_t reviews_today;
    vocab_state_record_t after;    /* state right after the answer */
    uint32_t after_pos;
    uint16_t after_new_today;
    uint16_t after_reviews_today;
} vocab_undo_t;

_Static_assert(sizeof(vocab_state_record_t) == 28, "state record layout");

static vocab_bank_t g_bank;
static vocab_index_t *g_index;
static vocab_state_record_t *g_records;
static uint32_t g_count;
static uint32_t g_bank_hash;
static char g_root[160];
static bool g_open;

static int g_focus;
static bool g_exhausted;
static uint32_t g_current;
static bool g_current_valid;
static vocab_card_stage_t g_stage;

static uint32_t g_now;
static uint32_t g_resume_id = 0xFFFFFFFFu;
static uint32_t g_resume_day;
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
    cfg->new_limit = 60;
    cfg->review_limit = 200;
    cfg->auto_audio = true;
    cfg->group_size = 10;
    cfg->error_mode = 0;
    cfg->shuffle = false;
    cfg->auto_example = true;
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
    memcpy(&crc, raw + 16, 4);
    if (crc != crc32_bytes(raw, 16)) return false;
    cfg->mode = (vocab_mode_t)raw[4];
    cfg->target_percent = raw[5];
    uint16_t value = 0;
    memcpy(&value, raw + 6, 2);
    cfg->new_limit = value;
    memcpy(&value, raw + 8, 2);
    cfg->review_limit = value;
    cfg->auto_audio = raw[10] != 0;
    cfg->group_size = raw[11];
    cfg->error_mode = raw[12];
    cfg->shuffle = raw[13] != 0;
    cfg->auto_example = raw[14] != 0;
    if (cfg->mode != VOCAB_MODE_CLASSIC) cfg->mode = VOCAB_MODE_FSRS;
    if (cfg->group_size < 5 || cfg->group_size > 20) cfg->group_size = 10;
    if (cfg->error_mode > 1) cfg->error_mode = 0;
    if (cfg->target_percent < 80 || cfg->target_percent > 95) cfg->target_percent = 90;
    if (cfg->new_limit == 0 || cfg->new_limit > 200) cfg->new_limit = 60;
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
    raw[11] = cfg->group_size;
    raw[12] = cfg->error_mode;
    raw[13] = cfg->shuffle ? 1 : 0;
    raw[14] = cfg->auto_example ? 1 : 0;
    raw[15] = 0;
    uint32_t crc = crc32_bytes(raw, 16);
    memcpy(raw + 16, &crc, 4);
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
    if (id >= g_bank.count) return false;
    if (g_index) {
        *index = g_index[id];
        return true;
    }
    if (!g_bank.file) return false;
    if (fseek(g_bank.file, (long)(g_bank.index_off + id * g_bank.entry_bytes), SEEK_SET) != 0)
        return false;
    unsigned char raw[VOCAB_INDEX_ENTRY];
    const size_t want = g_bank.entry_bytes ? g_bank.entry_bytes : VOCAB_INDEX_ENTRY;
    if (fread(raw, 1, want, g_bank.file) != want) return false;
    memset(index, 0, sizeof(*index));
    memcpy(&index->word_off, raw + 0, 4);
    memcpy(&index->meaning_off, raw + 4, 4);
    memcpy(&index->example_en_off, raw + 8, 4);
    memcpy(&index->example_zh_off, raw + 12, 4);
    memcpy(&index->root_off, raw + 16, 4);
    memcpy(&index->phonetic_off, raw + 20, 4);
    memcpy(&index->group_off, raw + 24, 4);
    memcpy(&index->pos_off, raw + 28, 4);
    if (want >= 40) {
        memcpy(&index->derived_off, raw + 32, 4);
        memcpy(&index->phrase_off, raw + 36, 4);
    }
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
    if ((version != VOCAB_BANK_VERSION && version != 3u) || count == 0 || count > 20000u)
        return false;
    g_bank.entry_bytes = version == 3u ? 32u : 40u;
    memcpy(&g_bank.index_off, head + 8, 4);
    memcpy(&g_bank.strings_off, head + 12, 4);
    memcpy(&g_bank.bank_hash, head + 16, 4);
    uint32_t stored = 0;
    memcpy(&stored, head + 28, 4);
    if (stored != crc32_bytes(head, 28)) return false;
    g_bank.count = count;
    /* Read the whole index once, in PSRAM (6588 cards = 264 KB). */
    g_index = vocab_alloc(count, sizeof(vocab_index_t));
    if (g_index) {
        const size_t raw_bytes = (size_t)count * g_bank.entry_bytes;
        unsigned char *raw = vocab_alloc(1, raw_bytes);
        if (!raw || fseek(g_bank.file, (long)g_bank.index_off, SEEK_SET) != 0 ||
            fread(raw, 1, raw_bytes, g_bank.file) != raw_bytes) {
            free(raw);
            free(g_index);
            g_index = NULL;
        } else {
            for (uint32_t i = 0; i < count; i++) {
                const unsigned char *row = raw + (size_t)i * g_bank.entry_bytes;
                memcpy(&g_index[i].word_off, row + 0, 4);
                memcpy(&g_index[i].meaning_off, row + 4, 4);
                memcpy(&g_index[i].example_en_off, row + 8, 4);
                memcpy(&g_index[i].example_zh_off, row + 12, 4);
                memcpy(&g_index[i].root_off, row + 16, 4);
                memcpy(&g_index[i].phonetic_off, row + 20, 4);
                memcpy(&g_index[i].group_off, row + 24, 4);
                memcpy(&g_index[i].pos_off, row + 28, 4);
                if (g_bank.entry_bytes >= 40) {
                    memcpy(&g_index[i].derived_off, row + 32, 4);
                    memcpy(&g_index[i].phrase_off, row + 36, 4);
                }
            }
            free(raw);
        }
    }
    return true;
}

static void bank_close(void) {
    if (g_bank.file) fclose(g_bank.file);
    free(g_index);
    g_index = NULL;
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
    g_records = vocab_alloc(g_count, sizeof(vocab_state_record_t));
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
    memcpy(&g_resume_id, head + 24, 4);
    memcpy(&g_resume_day, head + 28, 4);
    if (version != VOCAB_STATE_VERSION || stored_count != (uint16_t)g_count ||
        bank_hash != g_bank_hash) {
        fclose(f);
        return true;
    }
    size_t bytes = (size_t)g_count * sizeof(vocab_state_record_t);
    vocab_state_record_t *tmp = vocab_alloc(1, bytes);
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
        VOCAB_LOG("state loaded: %u cards, resume=%d day=%u", (unsigned)g_count,
                  (int)g_resume_id, (unsigned)g_resume_day);
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
    uint16_t version = VOCAB_STATE_VERSION, count = (uint16_t)g_count;
    memcpy(head + 4, &version, 2);
    memcpy(head + 6, &count, 2);
    memcpy(head + 8, &g_last_day, 4);
    memcpy(head + 12, &g_streak, 2);
    memcpy(head + 14, &g_new_today, 2);
    memcpy(head + 16, &g_reviews_today, 2);
    memcpy(head + 20, &g_bank_hash, 4);
    memcpy(head + 24, &g_resume_id, 4);
    memcpy(head + 28, &g_resume_day, 4);

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
    if (rename(tmp_path, path) != 0) {
        VOCAB_LOG("state save failed: rename %s", path);
        return false;
    }
    VOCAB_LOG("state saved: %u cards, resume=%d day=%u", (unsigned)g_count,
              (int)g_resume_id, (unsigned)g_resume_day);
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
    unsigned replayed = 0;
    while (fread(raw, 1, sizeof(raw), f) == sizeof(raw)) {
        uint32_t id = 0;
        memcpy(&id, raw, 4);
        if (id >= g_count) continue;
        memcpy(&g_records[id], raw + 4, sizeof(vocab_state_record_t));
        replayed++;
    }
    if (replayed) VOCAB_LOG("journal replayed: %u entries", replayed);
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

/* flags bits 0-1: learning stage (0 quiz, 1 example, 2 bare word). */
#define VOCAB_LEVEL_OF(record) ((record)->flags & 0x03u)

/* Local calendar day (minutes since 2020 divided by a local day), derived from
   the system timezone rather than a hard-coded offset. */
static uint32_t vocab_local_day(uint32_t now_min) {
    const time_t t = (time_t)(((uint64_t)now_min + VOCAB_MINUTES_2020) * 60u);
    struct tm lt, gt;
    if (localtime_r(&t, &lt) != NULL && gmtime_r(&t, &gt) != NULL) {
        int offset = (lt.tm_hour * 60 + lt.tm_min) - (gt.tm_hour * 60 + gt.tm_min);
        if (lt.tm_yday != gt.tm_yday) offset += (lt.tm_yday > gt.tm_yday) ? 1440 : -1440;
        const int64_t local = (int64_t)now_min + offset;
        return (uint32_t)((local < 0 ? 0 : local) / VOCAB_MINUTE_PER_DAY);
    }
    return now_min / VOCAB_MINUTE_PER_DAY;
}

static uint32_t review_days(const vocab_config_t *cfg, vocab_state_record_t *record,
                            float stability, vocab_grade_t grade) {
    if (cfg->mode == VOCAB_MODE_CLASSIC) {
        static const uint16_t table[8] = {1, 1, 2, 4, 7, 15, 30, 60};
        unsigned step = record->reps < 7 ? record->reps : 7;
        if (grade == VOCAB_GOOD) step = step + 1 > 7 ? 7 : step + 1;
        if (grade == VOCAB_EASY) step = step + 2 > 7 ? 7 : step + 2;
        return table[step];
    }
    float days = vocab_fsrs_interval(stability, (float)cfg->target_percent / 100.0f);
    if (days < 1.0f) days = 1.0f;
    return (uint32_t)days;
}

/* One answer in either the learning ladder (level 0..2) or review.
 * Returns true when the card graduated into review during this answer. */
static bool schedule_record(vocab_state_record_t *record, vocab_grade_t grade,
                            uint32_t now_min, const vocab_config_t *cfg) {
    const uint32_t day = now_min / VOCAB_MINUTE_PER_DAY;
    uint32_t elapsed_min = record->last_min && now_min > record->last_min
                               ? now_min - record->last_min
                               : 0;
    uint16_t elapsed_days = (uint16_t)(elapsed_min / VOCAB_MINUTE_PER_DAY);
    const bool same_day = record->last_min != 0 && elapsed_min > 0 &&
                          elapsed_min < VOCAB_MINUTE_PER_DAY;
    const bool learning = record->state == VOCAB_STATE_NEW ||
                          record->state == VOCAB_STATE_LEARNING ||
                          record->state == VOCAB_STATE_RELEARNING;
    uint8_t level = VOCAB_LEVEL_OF(record);

    vocab_fsrs_memory_t memory = {record->stability, record->difficulty};

    if (learning) {
        if (grade == VOCAB_AGAIN) {
            level = 0;
            record->state = record->state == VOCAB_STATE_NEW ? VOCAB_STATE_LEARNING
                                                             : VOCAB_STATE_RELEARNING;
            record->due_min = now_min;   /* immediately queued for the next round */
            record->lapses++;
        } else if (grade == VOCAB_HARD) {
            record->due_min = now_min + 5;
            if (record->state == VOCAB_STATE_NEW) record->state = VOCAB_STATE_LEARNING;
        } else {
            if (level < 2) {
                level++;
                record->due_min = now_min + 5;
                if (record->state == VOCAB_STATE_NEW) record->state = VOCAB_STATE_LEARNING;
            } else {
                /* Graduation: one FSRS review with a passing grade. */
                if (record->state == VOCAB_STATE_NEW) {
                    vocab_fsrs_initial((int)VOCAB_GOOD, &memory);
                } else {
                    float elapsed = same_day ? 0.0f
                                             : (elapsed_days ? (float)elapsed_days : 1.0f);
                    vocab_fsrs_review(&memory, elapsed, (int)VOCAB_GOOD, same_day ? 1 : 0);
                }
                const uint32_t days = review_days(cfg, record, memory.stability, VOCAB_GOOD);
                record->state = VOCAB_STATE_REVIEW;
                record->due_min = now_min + days * VOCAB_MINUTE_PER_DAY;
                record->flags = 0;
                record->stability = memory.stability;
                record->difficulty = memory.difficulty;
                record->reps++;
                record->last_min = now_min;
                record->elapsed = elapsed_days;
                return true;
            }
        }
        record->flags = (uint8_t)((record->flags & ~0x03u) | level);
        record->last_min = now_min;
        record->elapsed = elapsed_days;
        (void)day;
        return false;
    }

    /* Review: FSRS (or the classic table) decides the next interval. */
    {
        float elapsed = same_day ? 0.0f : (elapsed_days ? (float)elapsed_days : 1.0f);
        vocab_fsrs_review(&memory, elapsed, (int)grade, same_day ? 1 : 0);
        const uint32_t days = review_days(cfg, record, memory.stability, grade);
        record->stability = memory.stability;
        record->difficulty = memory.difficulty;
        record->due_min = now_min + days * VOCAB_MINUTE_PER_DAY;
        if (grade == VOCAB_AGAIN) {
            /* 忘记: back to the three-stage ladder right away. */
            record->state = VOCAB_STATE_RELEARNING;
            record->flags = 0;
            record->lapses++;
            record->due_min = now_min;   /* immediately queued for the next round */
        } else {
            /* 认识 and 模糊 both stay in review; 模糊 just earned a shorter
               interval from the FSRS Hard grade. */
            record->state = VOCAB_STATE_REVIEW;
            record->flags = 0;
        }
        record->reps++;
        record->last_min = now_min;
        record->elapsed = elapsed_days;
    }
    return false;
}

/* ------------------------------------------------------------------ queue */

/* ------------------------------------------------------- learning groups ---
 * Learning follows the reference app: ten cards form a group, the group is
 * drilled three times (quiz, example recall, bare word), and cards that failed
 * any round are collected into a relearn group that runs right after the
 * current group. The plan lives in plan.dat so a power cut resumes on the same
 * card instead of restarting the group. */
#define PLAN_MAX 96
#define PLAN_MAGIC 0x504C414Eu /* "PLAN" */
#define PLAN_VERSION 1u
#define PLAN_GROUP_SIZE 10u

typedef struct {
    uint32_t id;
    uint8_t stage;
    uint8_t flags; /* bit0 answered, bit1 failed */
    uint16_t pad;
} plan_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t focus;
    uint8_t phase; /* 0 = learn rounds, 1 = relearn rounds */
    uint16_t len;
    uint16_t pos;
    uint16_t group;
    uint16_t pad;
} plan_header_t;

static uint32_t g_today_seconds;
static uint32_t g_total_seconds;
static uint32_t g_time_day;
static void time_load(void);
static bool time_save(void);

static plan_header_t g_plan_hdr;
static plan_entry_t *g_plan;

static void plan_path_of(char *buf, size_t bytes) { path_of(buf, bytes, "plan.dat"); }

static void plan_reset(void) {
    memset(&g_plan_hdr, 0, sizeof(g_plan_hdr));
    g_plan_hdr.magic = PLAN_MAGIC;
    g_plan_hdr.version = PLAN_VERSION;
    if (g_plan) memset(g_plan, 0, sizeof(plan_entry_t) * PLAN_MAX);
}

static bool plan_save(void) {
    if (!g_plan) return false;
    char path[256], tmp[288];
    plan_path_of(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(&g_plan_hdr, 1, sizeof(g_plan_hdr), f) == sizeof(g_plan_hdr) &&
              (g_plan_hdr.len == 0 ||
               fwrite(g_plan, sizeof(plan_entry_t), g_plan_hdr.len, f) == g_plan_hdr.len);
    fflush(f);
    fclose(f);
    if (!ok) {
        remove(tmp);
        return false;
    }
    remove(path);
    return rename(tmp, path) == 0;
}

static bool plan_load(void) {
    if (!g_plan) return false;
    char path[256];
    plan_path_of(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    plan_header_t head;
    bool ok = fread(&head, 1, sizeof(head), f) == sizeof(head) &&
              head.magic == PLAN_MAGIC && head.version == PLAN_VERSION &&
              head.len <= PLAN_MAX && head.pos <= head.len &&
              (head.len == 0 || fread(g_plan, sizeof(plan_entry_t), head.len, f) == head.len);
    fclose(f);
    if (!ok) return false;
    g_plan_hdr = head;
    return true;
}

static void plan_build_cards(const uint32_t *ids, unsigned count, uint8_t phase,
                             unsigned rounds) {
    plan_reset();
    g_plan_hdr.phase = phase;
    unsigned n = 0;
    /* Round-major: every card is drilled in round 1 before round 2 starts. */
    for (unsigned round = 0; round < rounds; round++) {
        for (unsigned card = 0; card < count && n < PLAN_MAX; card++) {
            g_plan[n].id = ids[card];
            g_plan[n].stage = rounds == 1 ? VOCAB_STAGE_WORD
                              : round == 0 ? VOCAB_STAGE_QUIZ
                              : round == 1 ? VOCAB_STAGE_RECALL
                                           : VOCAB_STAGE_WORD;
            g_plan[n].flags = 0;
            n++;
        }
    }
    g_plan_hdr.len = (uint16_t)n;
    g_plan_hdr.pos = 0;
}

/* ---------------------------------------------------------------- order */
/* One fixed shuffle per word book. Only the 32-bit seed is persisted, so the
   same order comes back after reboots and after toggling 正序/乱序. */
#define ORDER_MAGIC 0x44524f56u /* "VORD" */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t seed;
    uint32_t crc;
} order_hdr_t;

static uint32_t g_order_seed;
static bool g_order_ready;

static bool order_seed_load(void) {
    char path[256];
    path_of(path, sizeof(path), "order.dat");
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    order_hdr_t hdr;
    const bool ok = fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    fclose(f);
    if (!ok || hdr.magic != ORDER_MAGIC || hdr.version != VOCAB_VERSION) return false;
    if (hdr.count != g_count || hdr.seed == 0) return false;
    uint32_t check = hdr.crc;
    order_hdr_t copy = hdr;
    copy.crc = 0;
    if (crc32_bytes(&copy, sizeof(copy)) != check) return false;
    g_order_seed = hdr.seed;
    return true;
}

static void order_seed_save(void) {
    char path[256];
    path_of(path, sizeof(path), "order.dat");
    order_hdr_t hdr = {ORDER_MAGIC, VOCAB_VERSION, g_count, g_order_seed, 0};
    hdr.crc = crc32_bytes(&hdr, sizeof(hdr) - sizeof(hdr.crc));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(&hdr, 1, sizeof(hdr), f);
    fflush(f);
    fclose(f);
}

static uint32_t order_seed_get(void) {
    if (g_order_ready) return g_order_seed;
    g_order_ready = true;
    if (!order_seed_load()) {
#ifdef ESP_PLATFORM
        g_order_seed = esp_random();
#else
        g_order_seed = (uint32_t)rand() * 2654435761u;
#endif
        if (g_order_seed == 0) g_order_seed = 0x9e3779b9u;
        order_seed_save();
        VOCAB_LOG("random order generated: seed=%08x", (unsigned)g_order_seed);
    }
    return g_order_seed;
}

/* Positions 0..count-1 in the book's fixed shuffled order. */
static bool order_next_perm(uint32_t *perm, uint32_t count, uint32_t seed) {
    if (!perm || !count) return false;
    for (uint32_t i = 0; i < count; i++) perm[i] = i;
    uint32_t s = seed ? seed : 0x9e3779b9u;
    for (uint32_t i = count; i > 1; i--) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const uint32_t j = s % i;
        const uint32_t tmp = perm[i - 1];
        perm[i - 1] = perm[j];
        perm[j] = tmp;
    }
    return true;
}

/* Cards in the given states, in bank order, that are due now. */
static unsigned plan_collect(uint8_t state_a, uint8_t state_b, uint32_t now_min,
                             uint32_t *out, unsigned max) {
    unsigned n = 0;
    /* Random word selection: walk the book's fixed shuffled order, so new
       words come from all over the book instead of its first pages. */
    if (g_config.shuffle && state_a == VOCAB_STATE_NEW && g_count) {
        uint32_t *perm = vocab_alloc(g_count, sizeof(uint32_t));
        if (perm && order_next_perm(perm, g_count, order_seed_get())) {
            for (uint32_t k = 0; k < g_count && n < max; k++) {
                const uint32_t i = perm[k];
                const vocab_state_record_t *record = &g_records[i];
                if (record->state != state_a && record->state != state_b) continue;
                out[n++] = i;
            }
        }
        free(perm);
        if (n) return n;
    }
    for (uint32_t i = 0; i < g_count && n < max; i++) {
        const vocab_state_record_t *record = &g_records[i];
        if (record->state != state_a && record->state != state_b) continue;
        if (state_a != VOCAB_STATE_NEW && record->due_min && record->due_min > now_min) continue;
        out[n++] = i;
    }
    return n;
}

static bool plan_build_learn(uint32_t now_min) {
    uint32_t ids[32];
    const unsigned per_group = (g_config.group_size >= 5 && g_config.group_size <= 20)
                                   ? g_config.group_size : PLAN_GROUP_SIZE;
    /* Relearning first: words you just forgot come back before new material. */
    unsigned n = plan_collect(VOCAB_STATE_RELEARNING, VOCAB_STATE_LEARNING, now_min,
                              ids, per_group);
    if (n < per_group) {
        uint32_t more[32];
        const unsigned m = plan_collect(VOCAB_STATE_NEW, VOCAB_STATE_NEW, now_min,
                                        more, per_group - n);
        for (unsigned i = 0; i < m && n < per_group; i++) ids[n++] = more[i];
    }
    if (!n) return false;
    VOCAB_LOG("learn plan: %u cards (first id=%u state=%u due=%u now=%u)",
              n, (unsigned)ids[0], (unsigned)g_records[ids[0]].state,
              (unsigned)g_records[ids[0]].due_min, (unsigned)now_min);
    g_plan_hdr.group++;
    plan_build_cards(ids, n, 0, 3);
    return true;
}

static bool plan_build_relearn(void) {
    uint32_t ids[32];
    unsigned n = 0;
    for (uint16_t i = 0; i < g_plan_hdr.len && n < PLAN_GROUP_SIZE; i++) {
        if (!(g_plan[i].flags & 2)) continue;
        bool seen = false;
        for (unsigned k = 0; k < n; k++)
            if (ids[k] == g_plan[i].id) seen = true;
        if (!seen) ids[n++] = g_plan[i].id;
    }
    if (!n) return false;
    plan_build_cards(ids, n, 1, 3);
    return true;
}

static bool plan_build_review(uint32_t now_min) {
    uint32_t ids[32];
    const unsigned per_group = (g_config.group_size >= 5 && g_config.group_size <= 20)
                                   ? g_config.group_size : PLAN_GROUP_SIZE;
    unsigned n = 0;
    for (uint32_t i = 0; i < g_count && n < per_group; i++) {
        const vocab_state_record_t *record = &g_records[i];
        if (record->state != VOCAB_STATE_REVIEW) continue;
        if (!record->due_min || record->due_min > now_min) continue;
        ids[n++] = i;
    }
    if (!n) return false;
    VOCAB_LOG("review plan: %u due (first id=%u state=%u due=%u now=%u)",
              n, (unsigned)ids[0], (unsigned)g_records[ids[0]].state,
              (unsigned)g_records[ids[0]].due_min, (unsigned)now_min);
    plan_build_cards(ids, n, 2, 1);
    return true;
}

bool vocab_review_retry(uint32_t now_min) {
    (void)now_min;
    if (!g_open || !g_plan) return false;
    if (g_plan_hdr.len >= PLAN_MAX) return false;
    g_plan[g_plan_hdr.len].id = g_current;
    g_plan[g_plan_hdr.len].stage = VOCAB_STAGE_WORD;
    g_plan[g_plan_hdr.len].flags = 0;
    g_plan_hdr.len++;
    plan_save();
    return true;
}

/* Move to the next plan when the current one is exhausted. */
static bool plan_advance(uint32_t now_min) {
    if (g_plan_hdr.pos < g_plan_hdr.len) return true;
    if (g_focus == 2) return plan_build_review(now_min);
    if (g_plan_hdr.phase != 1 && plan_build_relearn()) return true;
    return plan_build_learn(now_min);
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
    g_plan = vocab_alloc(PLAN_MAX, sizeof(plan_entry_t));
    time_load();
    vocab_config_load(g_root, &g_config);
    g_open = true;
    g_current_valid = false;
    return true;
}

void vocab_close(uint32_t now_min) {
    if (g_open) {
        g_now = now_min ? now_min : g_now;
        state_save();
    }
    bank_close();
    free(g_records);
    free(g_plan);
    g_plan = NULL;
    g_records = NULL;
    g_count = 0;
    g_undo_count = 0;
    g_open = false;
}

bool vocab_opened(void) { return g_open; }
uint32_t vocab_count(void) { return g_count; }

bool vocab_begin_day(uint32_t now_min, const vocab_config_t *cfg) {
    return vocab_begin_session(now_min, cfg, 0);
}

bool vocab_begin_session(uint32_t now_min, const vocab_config_t *cfg, int focus) {
    if (!g_open) return false;
    if (cfg) g_config = *cfg;
    g_now = now_min;
    const uint32_t day = vocab_local_day(now_min);
    if (day != g_last_day) {
        g_streak = (g_last_day && day == g_last_day + 1) ? (uint16_t)(g_streak + 1) : 1;
        g_last_day = day;
        g_new_today = 0;
        g_reviews_today = 0;
    }
    g_focus = focus;
    g_exhausted = false;
    {
        bool loaded = plan_load();
        const bool type_ok = (focus == 2) ? (g_plan_hdr.phase == 2u)
                                          : (g_plan_hdr.phase != 2u);
        bool usable = loaded && g_plan_hdr.focus == (uint8_t)focus && type_ok &&
                      g_plan_hdr.pos < g_plan_hdr.len;
        /* Review always rebuilds from what is due now: a leftover group of 3
           must not shadow the 11 cards that have become due since. */
        if (focus == 2) usable = false;
        VOCAB_LOG("begin focus=%d loaded=%d plan(focus=%u phase=%u pos=%u len=%u) usable=%d",
                  focus, (int)loaded, (unsigned)g_plan_hdr.focus, (unsigned)g_plan_hdr.phase,
                  (unsigned)g_plan_hdr.pos, (unsigned)g_plan_hdr.len, (int)usable);
        if (!usable) {
            bool built = false;
            if (focus == 3) {
                uint32_t ids[PLAN_GROUP_SIZE];
                unsigned n = plan_collect(VOCAB_STATE_LEARNING, VOCAB_STATE_RELEARNING,
                                          now_min, ids, PLAN_GROUP_SIZE);
                if (n) { plan_build_cards(ids, n, 1, 3); built = true; }
            }
            if (!built && focus == 2) built = plan_build_review(now_min);
            /* Review with nothing due must stay empty: falling back to a learn
               plan is what showed a new word inside 复习. */
            if (!built && focus != 2) built = plan_build_learn(now_min);
            if (!built) {
                g_plan_hdr.len = 0;
                g_plan_hdr.pos = 0;
                g_plan_hdr.phase = (focus == 2) ? 2u : 0u;
            }
            g_plan_hdr.focus = (uint8_t)focus;
            plan_save();
        }
    }
    return true;
}

static bool load_card(uint32_t id, vocab_card_t *out, vocab_card_stage_t *stage) {
    vocab_index_t index;
    if (!read_index(id, &index)) return false;
    memset(out, 0, sizeof(*out));
    out->id = id;
    read_string(index.word_off, out->word, sizeof(out->word));
    read_string(index.phonetic_off, out->phonetic, sizeof(out->phonetic));
    read_string(index.meaning_off, out->meaning, sizeof(out->meaning));
    read_string(index.example_en_off, out->example_en, sizeof(out->example_en));
    read_string(index.example_zh_off, out->example_zh, sizeof(out->example_zh));
    read_string(index.root_off, out->root, sizeof(out->root));
    read_string(index.group_off, out->group, sizeof(out->group));
    read_string(index.pos_off, out->pos, sizeof(out->pos));
    read_string(index.derived_off, out->derived, sizeof(out->derived));
    read_string(index.phrase_off, out->phrase, sizeof(out->phrase));

    const vocab_state_record_t *record = &g_records[id];
    if (record->state == VOCAB_STATE_NEW || record->state == VOCAB_STATE_LEARNING ||
        record->state == VOCAB_STATE_RELEARNING) {
        const uint8_t level = record->flags & 0x03u;
        g_stage = level == 0 ? VOCAB_STAGE_QUIZ
                  : level == 1 ? VOCAB_STAGE_RECALL
                               : VOCAB_STAGE_WORD;
    } else {
        g_stage = VOCAB_STAGE_WORD; /* review always asks for the bare word */
    }
    if (stage) *stage = g_stage;
    return true;
}

bool vocab_next(vocab_card_t *out, vocab_card_stage_t *stage) {
    if (!g_open || !out || !g_plan) return false;
    for (;;) {
        while (g_plan_hdr.pos < g_plan_hdr.len) {
            plan_entry_t *entry = &g_plan[g_plan_hdr.pos];
            if ((entry->flags & 1) || entry->id >= g_count) {
                g_plan_hdr.pos++;
                continue;
            }
            g_current = entry->id;
            g_current_valid = true;
            if (!load_card(entry->id, out, stage)) {
                g_plan_hdr.pos++;
                continue;
            }
            g_stage = (vocab_card_stage_t)entry->stage;
            if (stage) *stage = g_stage;
            VOCAB_LOG("card id=%u state=%u due=%u stage=%u pos=%u/%u",
                      (unsigned)entry->id, (unsigned)g_records[entry->id].state,
                      (unsigned)g_records[entry->id].due_min, (unsigned)entry->stage,
                      (unsigned)g_plan_hdr.pos, (unsigned)g_plan_hdr.len);
            return true;
        }
        if (!plan_advance(g_now)) {
            g_exhausted = true;
            g_current_valid = false;
            return false;
        }
        plan_save();
    }
}

bool vocab_load_by_id(uint32_t id, vocab_card_t *out, vocab_card_stage_t *stage) {
    if (!g_open || !out || id >= g_count) return false;
    return load_card(id, out, stage);
}

void vocab_abandon_plan(void) {
    plan_reset();
    char path[256];
    plan_path_of(path, sizeof(path));
    remove(path);
}

bool vocab_current_card(vocab_card_t *out, vocab_card_stage_t *stage) {
    if (!g_open || !g_current_valid || !out) return false;
    return load_card(g_current, out, stage);
}

bool vocab_answer(uint32_t now_min, vocab_grade_t grade) {
    if (!g_open || !g_current_valid) return false;
    if (grade < VOCAB_AGAIN || grade > VOCAB_EASY) return false;
    g_now = now_min;
    vocab_state_record_t *record = &g_records[g_current];
    const bool was_new = record->state == VOCAB_STATE_NEW;
    const bool was_review = record->state == VOCAB_STATE_REVIEW || record->state == VOCAB_STATE_RELEARNING;

    vocab_undo_t *undo = &g_undo[g_undo_count % VOCAB_UNDO_DEPTH];
    undo->id = g_current;
    undo->record = *record;
    undo->queue_pos = g_plan_hdr.pos;
    undo->new_today = g_new_today;
    undo->reviews_today = g_reviews_today;
    g_undo_count++;

    schedule_record(record, grade, now_min, &g_config);
    journal_append(g_current, record);
    if (was_new && g_new_today < 0xFFFF) g_new_today++;
    /* Only due-card answers count as reviews; learning steps are part of study. */
    if (was_review && g_reviews_today < 0xFFFF) g_reviews_today++;

    {
        if (g_plan_hdr.pos < g_plan_hdr.len) {
            plan_entry_t *entry = &g_plan[g_plan_hdr.pos];
            entry->flags = (uint8_t)(1u | (grade == VOCAB_AGAIN ? 2u : 0u));
            g_plan_hdr.pos++;
            /* 不背单词 style: a wrong card is retried at the end of its round. */
            if (g_config.error_mode == 1 && grade == VOCAB_AGAIN && g_plan_hdr.len < PLAN_MAX) {
                unsigned same = 0;
                for (uint16_t i = 0; i < g_plan_hdr.len; i++)
                    if (g_plan[i].id == entry->id && g_plan[i].stage == entry->stage) same++;
                if (same < 3) {
                    unsigned insert_at = g_plan_hdr.len;
                    for (uint16_t i = 0; i < g_plan_hdr.len; i++)
                        if (g_plan[i].stage == entry->stage) insert_at = (unsigned)i + 1;
                    memmove(&g_plan[insert_at + 1], &g_plan[insert_at],
                            (g_plan_hdr.len - insert_at) * sizeof(plan_entry_t));
                    memset(&g_plan[insert_at], 0, sizeof(plan_entry_t));
                    g_plan[insert_at].id = entry->id;
                    g_plan[insert_at].stage = entry->stage;
                    g_plan_hdr.len++;
                }
            }
            plan_save();
        }
    }
    /* Review + forgot: repeat the card at the end of this review session. */
    if (was_review && grade == VOCAB_AGAIN && g_plan_hdr.len < PLAN_MAX &&
        g_plan_hdr.phase == 2) {
        g_plan[g_plan_hdr.len].id = g_current;
        g_plan[g_plan_hdr.len].stage = VOCAB_STAGE_WORD;
        g_plan[g_plan_hdr.len].flags = 0;
        g_plan_hdr.len++;
    }
    if (g_undo_count) {
        vocab_undo_t *undo = &g_undo[(g_undo_count - 1) % VOCAB_UNDO_DEPTH];
        undo->after = g_records[undo->id];
        undo->after_pos = g_plan_hdr.pos;
        undo->after_new_today = g_new_today;
        undo->after_reviews_today = g_reviews_today;
    }
    g_current_valid = false;
    return true;
}

bool vocab_replan_idle(uint32_t now_min) {
    if (!g_open || !g_plan || g_plan_hdr.pos != 0) return false;
    if (g_focus == 1) return plan_build_learn(now_min);
    if (g_focus == 2) return plan_build_review(now_min);
    return false;
}

bool vocab_undo_cancel(uint32_t now_min) {
    (void)now_min;
    if (!g_open || !g_undo_count) return false;
    vocab_undo_t *undo = &g_undo[(g_undo_count - 1) % VOCAB_UNDO_DEPTH];
    g_records[undo->id] = undo->after;
    if (g_plan) {
        g_plan_hdr.pos = (uint16_t)undo->after_pos;
        plan_save();
    }
    g_new_today = undo->after_new_today;
    g_reviews_today = undo->after_reviews_today;
    g_current = undo->id;
    g_current_valid = true;
    g_undo_count--;
    return true;
}

bool vocab_undo_regrade(uint32_t now_min, vocab_grade_t grade) {
    if (!g_open || !g_undo_count) return false;
    vocab_undo_t *undo = &g_undo[(g_undo_count - 1) % VOCAB_UNDO_DEPTH];
    g_records[undo->id] = undo->record;
    if (g_plan) {
        g_plan_hdr.pos = (uint16_t)undo->queue_pos;
        if (g_plan_hdr.pos < g_plan_hdr.len) g_plan[g_plan_hdr.pos].flags = 0;
    }
    g_new_today = undo->new_today;
    g_reviews_today = undo->reviews_today;
    g_current = undo->id;
    g_current_valid = true;
    g_undo_count--;
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
    return vocab_answer(now_min, grade);
}

bool vocab_regrade_by_id(uint32_t now_min, uint32_t id, vocab_grade_t grade) {
    if (!g_open || id >= g_count) return false;
    int found = -1;
    for (unsigned k = 0; k < g_undo_count && k < VOCAB_UNDO_DEPTH; k++) {
        const unsigned idx = (g_undo_count - 1 - k) % VOCAB_UNDO_DEPTH;
        if (g_undo[idx].id == id) { found = (int)idx; break; }
    }
    if (found < 0) return false;
    vocab_state_record_t pre = g_undo[found].record;
    g_records[id] = pre;
    schedule_record(&g_records[id], grade, now_min, &g_config);
    journal_append(id, &g_records[id]);
    plan_save();
    return true;
}

bool vocab_undo(uint32_t now_min) {
    if (!g_open || g_undo_count == 0) return false;
    g_undo_count--;
    vocab_undo_t *undo = &g_undo[g_undo_count % VOCAB_UNDO_DEPTH];
    if (undo->id >= g_count) return false;
    g_records[undo->id] = undo->record;
    if (g_plan) {
        g_plan_hdr.pos = (uint16_t)undo->queue_pos;
        if (g_plan_hdr.pos < g_plan_hdr.len) g_plan[g_plan_hdr.pos].flags = 0;
        plan_save();
    }
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

static void time_path(char *buf, size_t bytes) { path_of(buf, bytes, "time.dat"); }

static bool time_save(void) {
    char path[256], tmp[288];
    time_path(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    unsigned char raw[20] = {0};
    memcpy(raw, "VOCT", 4);
    memcpy(raw + 4, &g_time_day, 4);
    memcpy(raw + 8, &g_today_seconds, 4);
    memcpy(raw + 12, &g_total_seconds, 4);
    uint32_t crc = crc32_bytes(raw, 16);
    memcpy(raw + 16, &crc, 4);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(raw, 1, sizeof(raw), f) == sizeof(raw);
    fflush(f);
    fclose(f);
    if (!ok) { remove(tmp); return false; }
    remove(path);
    return rename(tmp, path) == 0;
}

static void time_load(void) {
    char path[256];
    time_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    unsigned char raw[20];
    bool ok = fread(raw, 1, sizeof(raw), f) == sizeof(raw) && memcmp(raw, "VOCT", 4) == 0;
    fclose(f);
    if (!ok) return;
    uint32_t crc = 0;
    memcpy(&crc, raw + 16, 4);
    if (crc != crc32_bytes(raw, 16)) return;
    memcpy(&g_time_day, raw + 4, 4);
    memcpy(&g_today_seconds, raw + 8, 4);
    memcpy(&g_total_seconds, raw + 12, 4);
}

bool vocab_add_seconds(uint32_t seconds) {
    if (!seconds) return true;
    const uint32_t today = vocab_local_day(g_now);
    if (g_time_day != today) {
        g_time_day = today;
        g_today_seconds = 0;
    }
    g_today_seconds += seconds;
    g_total_seconds += seconds;
    return time_save();
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
            /* Due today only: review cards whose interval has elapsed. Cards
               learned today (due tomorrow or in a few minutes of learning) are
               not part of today's review queue. */
            /* Review queue = graduated cards whose due time has passed.
               Day-based counting pulled in cards due later today, which made
               a word learned last night reappear hours early. */
            if (record->state == VOCAB_STATE_REVIEW &&
                record->due_min && record->due_min <= g_now)
                out->due_today++;
        }
        out->total++;
    }
    const uint32_t today = vocab_local_day(g_now);
    out->today_seconds = (g_time_day == today) ? g_today_seconds : 0;
    out->total_seconds = g_total_seconds;
    out->finished = g_exhausted;
    return true;
}

bool vocab_reset_progress(void) {
    if (!g_open || !g_records) return false;
    for (uint32_t i = 0; i < g_count; i++) record_default(&g_records[i]);
    g_new_today = 0;
    g_reviews_today = 0;
    g_streak = 0;
    g_last_day = 0;
    g_undo_count = 0;
    g_resume_id = 0xFFFFFFFFu;
    plan_reset();
    char path[256];
    state_path(path, sizeof(path));
    remove(path);
    journal_path(path, sizeof(path));
    remove(path);
    plan_path_of(path, sizeof(path));
    remove(path);
    return state_save();
}

bool vocab_audio_path(uint32_t id, char *buf, size_t bytes) {
    if (!buf || bytes == 0) return false;
    snprintf(buf, bytes, "%s/.thingsos/vocab/audio/%u.mp3", g_root, (unsigned)id);
    return access(buf, R_OK) == 0;
}

bool vocab_example_audio_path(uint32_t id, char *buf, size_t bytes) {
    if (!buf || bytes == 0) return false;
    snprintf(buf, bytes, "%s/.thingsos/vocab/audio/%u_ex.mp3", g_root, (unsigned)id);
    return access(buf, R_OK) == 0;
}

uint32_t vocab_current_id(void) { return g_current_valid ? g_current : 0xFFFFFFFFu; }

uint32_t vocab_new_count(void) {
    uint32_t fresh = 0;
    for (uint32_t i = 0; i < g_count; i++)
        if (g_records[i].state == VOCAB_STATE_NEW) fresh++;
    return fresh;
}

bool vocab_plan_progress(uint16_t *group, uint8_t *round, uint8_t *index, uint8_t *total,
                         uint8_t *phase) {
    if (!g_open || g_plan_hdr.len == 0) return false;
    const unsigned per_round = g_plan_hdr.len / 3u;
    if (!per_round) return false;
    if (group) *group = g_plan_hdr.group;
    if (round) *round = (uint8_t)(g_plan_hdr.pos / per_round + 1);
    if (index) *index = (uint8_t)(g_plan_hdr.pos % per_round + 1);
    if (total) *total = (uint8_t)per_round;
    if (phase) *phase = g_plan_hdr.phase;
    return true;
}

uint8_t vocab_current_state(void) {
    return g_current_valid ? (uint8_t)g_records[g_current].state : 0xFFu;
}

uint32_t vocab_learned_count(void) {
    uint32_t learned = 0;
    for (uint32_t i = 0; i < g_count; i++)
        if (g_records[i].state == VOCAB_STATE_REVIEW) learned++;
    return learned;
}

uint8_t vocab_current_level(void) {
    return g_current_valid ? (uint8_t)(g_records[g_current].flags & 0x03u) : 0;
}

/* Deterministic distractors: same-ish meaning length, different first glyph. */
static bool distractors_impl(uint32_t id, char out[3][VOCAB_MEANING_BYTES],
                             char words[3][VOCAB_WORD_BYTES]) {
    if (!out || !g_count || id >= g_count) return false;
    vocab_index_t self;
    if (!read_index(id, &self)) return false;
    char target[VOCAB_MEANING_BYTES];
    read_string(self.meaning_off, target, sizeof(target));
    const size_t want = strlen(target);
    unsigned found = 0;
    for (uint32_t step = 1; step < g_count && found < 3 && step < 24; step++) {
        const uint32_t candidate = (id + step * 7u) % g_count;
        if (candidate == id) continue;
        vocab_index_t other;
        if (!read_index(candidate, &other)) continue;
        char meaning[VOCAB_MEANING_BYTES];
        read_string(other.meaning_off, meaning, sizeof(meaning));
        const size_t have = strlen(meaning);
        if (have == 0 || (target[0] && meaning[0] == target[0])) continue;
        if (have + 4 < want || have > want + 8) continue;
        bool dup = false;
        for (unsigned k = 0; k < found; k++)
            if (!strcmp(out[k], meaning)) dup = true;
        if (dup) continue;
        snprintf(out[found], VOCAB_MEANING_BYTES, "%s", meaning);
        if (words) read_string(other.word_off, words[found], VOCAB_WORD_BYTES);
        found++;
    }
    for (uint32_t step = 1; step < g_count && found < 3 && step < 24; step++) {
        const uint32_t candidate = (id + step * 13u) % g_count;
        if (candidate == id) continue;
        vocab_index_t other;
        if (!read_index(candidate, &other)) continue;
        char meaning[VOCAB_MEANING_BYTES];
        read_string(other.meaning_off, meaning, sizeof(meaning));
        if (!meaning[0]) continue;
        bool dup = false;
        for (unsigned k = 0; k < found; k++)
            if (!strcmp(out[k], meaning)) dup = true;
        if (dup) continue;
        snprintf(out[found], VOCAB_MEANING_BYTES, "%s", meaning);
        if (words) read_string(other.word_off, words[found], VOCAB_WORD_BYTES);
        found++;
    }
    return found == 3;
}

bool vocab_distractors(uint32_t id, char out[3][VOCAB_MEANING_BYTES]) {
    return distractors_impl(id, out, NULL);
}

bool vocab_distractors_ex(uint32_t id, char means[3][VOCAB_MEANING_BYTES],
                          char words[3][VOCAB_WORD_BYTES]) {
    return distractors_impl(id, means, words);
}

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
