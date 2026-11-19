// SPDX-License-Identifier: Apache-2.0
#include "thingsos/vocab.h"
#include "thingsos/vocab_fsrs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "Failed at %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

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

typedef struct {
    const char *word;
    const char *phonetic;
    const char *meaning;
    const char *example_en;
    const char *example_zh;
    const char *root;
    const char *group;
} bank_row_t;

static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }

static void write_bank(const char *dir, const bank_row_t *rows, size_t count) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.thingsos", dir);
    mkdir(path, 0775);
    snprintf(path, sizeof(path), "%s/vocab", dir);
    mkdir(path, 0775);
    snprintf(path, sizeof(path), "%s/vocab/bank.bin", dir);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);

    const uint32_t index_off = 32;
    const uint32_t strings_off = index_off + (uint32_t)(count * 40);
    unsigned char *strings = calloc(1, 1 << 16);
    size_t at = 1; /* 0 is a valid "empty string" offset */
    uint32_t offsets[16][7];
    for (size_t i = 0; i < count && i < 16; i++) {
        const char *fields[7] = {rows[i].word, rows[i].phonetic, rows[i].meaning,
                                 rows[i].example_en, rows[i].example_zh,
                                 rows[i].root, rows[i].group};
        for (int k = 0; k < 7; k++) {
            const char *s = fields[k] ? fields[k] : "";
            offsets[i][k] = (uint32_t)at;
            size_t n = strlen(s) + 1;
            memcpy(strings + at, s, n);
            at += n;
        }
    }

    unsigned char head[32] = {0};
    memcpy(head, "VOCB", 4);
    put16(head + 4, 4); /* bank format version */
    put16(head + 6, (uint16_t)count);
    put32(head + 8, index_off);
    put32(head + 12, strings_off);
    put32(head + 16, 0x5A5A1234u);
    put32(head + 28, crc32_bytes(head, 28));
    fwrite(head, 1, sizeof(head), f);

    for (size_t i = 0; i < count && i < 16; i++) {
        unsigned char entry[40] = {0};
        put32(entry + 0, offsets[i][0]); /* word */
        put32(entry + 4, offsets[i][2]); /* meaning */
        put32(entry + 8, offsets[i][3]); /* example_en */
        put32(entry + 12, offsets[i][4]); /* example_zh */
        put32(entry + 16, offsets[i][5]); /* root */
        put32(entry + 20, offsets[i][1]); /* phonetic */
        put32(entry + 24, offsets[i][6]); /* group */
        put32(entry + 28, offsets[i][1]); /* pos */
        put32(entry + 32, offsets[i][5]); /* derived (fixture reuses root) */
        put32(entry + 36, offsets[i][3]); /* phrase (fixture reuses example) */
        fwrite(entry, 1, sizeof(entry), f);
    }
    fwrite(strings, 1, at, f);
    fclose(f);
    free(strings);
}

int main(void) {
    /* --- FSRS reference values (official defaults) --- */
    vocab_fsrs_memory_t memory = {0};
    vocab_fsrs_initial(3, &memory);
    CHECK(fabsf(memory.stability - 2.3065f) < 1e-4f);
    CHECK(fabsf(memory.difficulty - (6.4133f - expf(0.8334f * 2.0f) + 1.0f)) < 1e-4f);

    vocab_fsrs_memory_t good = {2.3065f, 2.1185f};
    vocab_fsrs_review(&good, 5.0f, 3, 0);
    CHECK(good.stability > 2.3065f); /* a successful review grows stability */
    vocab_fsrs_memory_t again = {10.0f, 5.0f};
    vocab_fsrs_review(&again, 10.0f, 1, 0);
    CHECK(again.stability < 10.0f); /* forgetting shortens stability */

    /* Interval grows with stability and shrinks with higher retention. */
    CHECK(vocab_fsrs_interval(10.0f, 0.9f) > vocab_fsrs_interval(2.0f, 0.9f));
    CHECK(vocab_fsrs_interval(10.0f, 0.95f) < vocab_fsrs_interval(10.0f, 0.80f));
    CHECK(vocab_fsrs_retrievability(10.0f, 0.0f) > 0.999f);
    CHECK(vocab_fsrs_retrievability(10.0f, 10.0f) > 0.85f &&
          vocab_fsrs_retrievability(10.0f, 10.0f) < 0.95f);

    /* --- time helpers --- */
    const uint32_t day = vocab_date_to_day(20261117);
    CHECK(vocab_day_to_date(day) == 20261117);
    CHECK(vocab_minutes_from_epoch(20261117, 9, 30) == day * 1440 + 9 * 60 + 30);

    /* --- bank + session --- */
    char root[256];
    snprintf(root, sizeof(root), "/tmp/vocab-test-%d", (int)getpid());
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    CHECK(system(cmd) == 0);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", root);
    CHECK(system(cmd) == 0);

    static const bank_row_t rows[] = {
        {"apple", "/ˈæpl/", "苹果", "I ate an apple.", "我吃了个苹果。", "a-pple", "L1"},
        {"abandon", "/əˈbændən/", "放弃", "He abandoned it.", "他放弃了。", "a-bandon", "L1"},
        {"benefit", "/ˈbenɪfɪt/", "好处", "It is a benefit.", "这是个好处。", "bene", "L2"},
        {"comfort", "/ˈkʌmfət/", "舒适", "A quiet comfort.", "一种安静的舒适。", "com-fort", "L2"},
        {"deliver", "/dɪˈlɪvə/", "递送", "Deliver the parcel.", "递送包裹。", "de-liver", "L2"},
        {"evident", "/ˈevɪdənt/", "明显的", "It is evident.", "这很明显。", "e-vid", "L3"},
    };
    write_bank(root, rows, 6);

    const uint32_t now = vocab_minutes_from_epoch(20261117, 8, 0);
    vocab_config_t cfg;
    vocab_config_defaults(&cfg);
    cfg.new_limit = 6;
    cfg.review_limit = 10;

    CHECK(vocab_open(root, now));
    CHECK(vocab_count() == 6);
    CHECK(vocab_begin_day(now, &cfg));

    vocab_card_t card;
    vocab_card_stage_t stage;
    vocab_stats_t stats;

    /* Round 1: every new card is a four-choice quiz first. */
    CHECK(vocab_next(&card, &stage));
    CHECK(strcmp(card.word, "apple") == 0);
    CHECK(strcmp(card.meaning, "苹果") == 0);
    CHECK(stage == VOCAB_STAGE_QUIZ);
    const uint32_t first_id = card.id;
    CHECK(vocab_current_level() == 0);

    char distractors[3][VOCAB_MEANING_BYTES];
    CHECK(vocab_distractors(first_id, distractors));
    CHECK(distractors[0][0] != 0 && strcmp(distractors[0], "苹果") != 0);

    CHECK(vocab_answer(now, VOCAB_GOOD));
    CHECK(vocab_stats(now, &stats));
    CHECK(stats.new_today == 0);   /* counted when all three rounds finish */
    CHECK(vocab_learned_count() == 0);

    /* Undo restores the counters and the plan cursor, then re-answer. */
    CHECK(vocab_undo(now));
    CHECK(vocab_stats(now, &stats));
    CHECK(stats.new_today == 0);
    CHECK(vocab_answer(now, VOCAB_GOOD));

    for (int i = 1; i < 6; i++) {
        CHECK(vocab_next(&card, &stage));
        CHECK(stage == VOCAB_STAGE_QUIZ);
        CHECK(vocab_answer(now, VOCAB_GOOD));
    }
    /* Round 2: example recall for the same six cards. */
    for (int i = 0; i < 6; i++) {
        CHECK(vocab_next(&card, &stage));
        CHECK(stage == VOCAB_STAGE_RECALL);
        CHECK(vocab_answer(now, VOCAB_GOOD));
    }
    /* Round 3: bare word, graduating into review. */
    for (int i = 0; i < 6; i++) {
        CHECK(vocab_next(&card, &stage));
        CHECK(stage == VOCAB_STAGE_WORD);
        CHECK(vocab_answer(now, VOCAB_GOOD));
    }
    CHECK(vocab_learned_count() == 6);

    /* Persistence: the graduated cards survive a reopen. */
    vocab_close(now);
    CHECK(vocab_open(root, now));
    CHECK(vocab_learned_count() == 6);
    CHECK(vocab_stats(now, &stats));
    CHECK(stats.reviews_today == 0);

    /* Classic mode uses the fixed Ebbinghaus table. */
    vocab_config_t classic = cfg;
    classic.mode = VOCAB_MODE_CLASSIC;
    CHECK(vocab_config_save(root, &classic));
    vocab_config_t loaded;
    CHECK(vocab_config_load(root, &loaded));
    CHECK(loaded.mode == VOCAB_MODE_CLASSIC);

    vocab_close(now);

    /* Corrupt state must not crash: open falls back to a fresh session. */
    snprintf(cmd, sizeof(cmd), "%s/vocab/state.dat", root);
    FILE *f = fopen(cmd, "wb");
    CHECK(f != NULL);
    fputs("broken", f);
    fclose(f);
    CHECK(vocab_open(root, now));
    CHECK(vocab_count() == 6);
    vocab_close(now);

    /* Review groups: one bare-word pass of ten, 模糊 stays in review while
       忘记 returns through the three-stage relearn. */
    char root4[256];
    snprintf(root4, sizeof(root4), "/tmp/vocab-review-%d", (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", root4, root4);
    CHECK(system(cmd) == 0);
    write_bank(root4, rows, 6);
    CHECK(vocab_open(root4, now));
    vocab_config_t rc = cfg;
    rc.group_size = 10;
    CHECK(vocab_begin_session(now, &rc, 1));
    for (int i = 0; i < 6 * 3; i++) {
        CHECK(vocab_next(&card, &stage));
        CHECK(vocab_answer(now, VOCAB_GOOD));
    }
    vocab_close(now);
    const uint32_t later = now + 30u * 24u * 60u;
    CHECK(vocab_open(root4, later));
    CHECK(vocab_begin_session(later, &rc, 2));
    int seen = 0;
    uint32_t again_id = 0;
    for (int i = 0; i < 6; i++) {
        CHECK(vocab_next(&card, &stage));
        CHECK(stage == VOCAB_STAGE_WORD);
        seen++;
        if (i == 0) { again_id = card.id; CHECK(vocab_answer(later, VOCAB_AGAIN)); }
        else CHECK(vocab_answer(later, i == 1 ? VOCAB_HARD : VOCAB_GOOD));
    }
    CHECK(seen == 6);
    /* The forgotten card comes back as a relearn group starting from the quiz;
       the fuzzy card does not. */
    CHECK(vocab_next(&card, &stage));
    CHECK(card.id == again_id && stage == VOCAB_STAGE_QUIZ);
    vocab_close(later);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root4);
    CHECK(system(cmd) == 0);

    /* Group model: a group is drilled quiz -> recall -> bare word, and cards
       that failed any round come back as a relearn group. */
    char root3[256];
    snprintf(root3, sizeof(root3), "/tmp/vocab-group-%d", (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", root3, root3);
    CHECK(system(cmd) == 0);
    write_bank(root3, rows, 6);
    CHECK(vocab_open(root3, now));
    vocab_config_t gc = cfg;
    gc.new_limit = 10;
    CHECK(vocab_begin_session(now, &gc, 1));
    uint32_t failed_id = 0;
    for (int i = 0; i < 6; i++) {
        CHECK(vocab_next(&card, &stage));
        CHECK(stage == VOCAB_STAGE_QUIZ);
        if (i == 1) failed_id = card.id;
        CHECK(vocab_answer(now, i == 1 ? VOCAB_AGAIN : VOCAB_GOOD));
    }
    for (int i = 0; i < 6; i++) {
        CHECK(vocab_next(&card, &stage));
        CHECK(stage == VOCAB_STAGE_RECALL);
        CHECK(vocab_answer(now, VOCAB_GOOD));
    }
    for (int i = 0; i < 6; i++) {
        CHECK(vocab_next(&card, &stage));
        CHECK(stage == VOCAB_STAGE_WORD);
        CHECK(vocab_answer(now, VOCAB_GOOD));
    }
    /* The failed card reopens as the relearn group, starting from the quiz. */
    CHECK(vocab_next(&card, &stage));
    CHECK(card.id == failed_id && stage == VOCAB_STAGE_QUIZ);
    vocab_close(now);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root3);
    CHECK(system(cmd) == 0);

    /* Undo and resume across a reopen, on a small dedicated bank. */
    char root2[256];
    snprintf(root2, sizeof(root2), "/tmp/vocab-resume-%d", (int)getpid());
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root2);
    CHECK(system(cmd) == 0);
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", root2);
    CHECK(system(cmd) == 0);
    write_bank(root2, rows, 6);

    CHECK(vocab_open(root2, now));
    vocab_config_t small = cfg;
    small.new_limit = 1;
    CHECK(vocab_begin_session(now, &small, 1));
    CHECK(vocab_next(&card, &stage));
    CHECK(stage == VOCAB_STAGE_QUIZ);
    CHECK(card.derived[0] != 0);   /* bank v4 fields are parsed */
    const uint32_t resume_id = card.id;
    CHECK(vocab_answer(now, VOCAB_GOOD));
    CHECK(vocab_undo(now));
    CHECK(vocab_current_card(&card, &stage));
    CHECK(card.id == resume_id && stage == VOCAB_STAGE_QUIZ);
    CHECK(vocab_answer(now, VOCAB_GOOD));
    vocab_close(now);

    const uint32_t reopen = now + 6;
    CHECK(vocab_open(root2, reopen));
    CHECK(vocab_begin_session(reopen, &small, 1));
    CHECK(vocab_next(&card, &stage));
    /* Resume continues this word's remaining rounds, not a new word. */
    CHECK(stage == VOCAB_STAGE_RECALL && card.id == resume_id);
    vocab_close(reopen);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root2);
    CHECK(system(cmd) == 0);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    CHECK(system(cmd) == 0);

    /* Optional: validate a real packed bundle (VOCAB_TEST_ROOT points at the
       directory that contains vocab, i.e. the SD root). */
    const char *external = getenv("VOCAB_TEST_ROOT");
    if (external && *external) {
        CHECK(vocab_open(external, now));
        CHECK(vocab_count() > 0);
        vocab_config_defaults(&cfg);
        CHECK(vocab_begin_day(now, &cfg));
        CHECK(vocab_next(&card, &stage));
        CHECK(card.word[0] != 0);
        CHECK(card.meaning[0] != 0);
        char audio[256];
        const bool has_audio = vocab_audio_path(card.id, audio, sizeof(audio));
        printf("external bundle: %u cards, first '%s' (%s), stage %d\n",
               (unsigned)vocab_count(), card.word, has_audio ? "audio" : "no audio",
               (int)stage);
        vocab_close(now);
    }
    printf("vocab tests passed\n");
    return 0;
}
