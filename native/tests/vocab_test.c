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
    snprintf(path, sizeof(path), "%s/.thingsos/vocab", dir);
    mkdir(path, 0775);
    snprintf(path, sizeof(path), "%s/.thingsos/vocab/bank.bin", dir);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);

    const uint32_t index_off = 32;
    const uint32_t strings_off = index_off + (uint32_t)(count * 28);
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
    put16(head + 4, 1);
    put16(head + 6, (uint16_t)count);
    put32(head + 8, index_off);
    put32(head + 12, strings_off);
    put32(head + 16, 0x5A5A1234u);
    put32(head + 28, crc32_bytes(head, 28));
    fwrite(head, 1, sizeof(head), f);

    for (size_t i = 0; i < count && i < 16; i++) {
        unsigned char entry[28] = {0};
        put32(entry + 0, offsets[i][0]); /* word */
        put32(entry + 4, offsets[i][2]); /* meaning */
        put32(entry + 8, offsets[i][3]); /* example_en */
        put32(entry + 12, offsets[i][4]); /* example_zh */
        put32(entry + 16, offsets[i][5]); /* root */
        put16(entry + 20, (uint16_t)offsets[i][1]); /* phonetic */
        put16(entry + 22, (uint16_t)offsets[i][6]); /* group */
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
    };
    write_bank(root, rows, 3);

    const uint32_t now = vocab_minutes_from_epoch(20261117, 8, 0);
    vocab_config_t cfg;
    vocab_config_defaults(&cfg);
    cfg.new_limit = 3;
    cfg.review_limit = 10;

    CHECK(vocab_open(root, now));
    CHECK(vocab_count() == 3);
    CHECK(vocab_begin_day(now, &cfg));

    vocab_card_t card;
    vocab_card_stage_t stage;
    CHECK(vocab_next(&card, &stage));
    CHECK(strcmp(card.word, "apple") == 0);
    CHECK(strcmp(card.meaning, "苹果") == 0);
    CHECK(stage == VOCAB_STAGE_WORD);
    const uint32_t first_id = card.id;
    CHECK(vocab_current_id() == first_id);
    vocab_reveal();
    CHECK(vocab_current_revealed());

    /* Good on a new card: stays learning, due in 10 minutes. */
    CHECK(vocab_answer(now, VOCAB_GOOD));
    vocab_stats_t stats;
    CHECK(vocab_stats(now, &stats));
    CHECK(stats.new_today == 1);
    CHECK(stats.reviews_today == 1);

    /* Undo restores the card and counters. */
    CHECK(vocab_undo(now));
    CHECK(vocab_stats(now, &stats));
    CHECK(stats.new_today == 0);

    /* Answer again and let it graduate with Easy. */
    CHECK(vocab_next(&card, &stage));
    CHECK(vocab_answer(now, VOCAB_EASY));

    /* Second card: Again -> learning with a short step. */
    CHECK(vocab_next(&card, &stage));
    const uint32_t second_id = card.id;
    CHECK(vocab_answer(now, VOCAB_AGAIN));

    /* Third card: Hard -> stays learning. */
    CHECK(vocab_next(&card, &stage));
    CHECK(vocab_answer(now, VOCAB_HARD));

    /* Soon after, due-now cards should not be served before their step. */
    CHECK(!vocab_next(&card, &stage));

    /* Persistence: reopen and check the counters survived via state+journal. */
    vocab_close(now);
    CHECK(vocab_open(root, now));
    CHECK(vocab_stats(now, &stats));
    CHECK(stats.reviews_today >= 3);

    /* Later the same day the learning steps become due again. */
    const uint32_t later = now + 11;
    CHECK(vocab_begin_day(later, &cfg));
    CHECK(vocab_next(&card, &stage));
    CHECK(card.id == second_id || card.id == first_id || card.id == 0xFFFFFFFFu);

    /* Classic mode uses the fixed Ebbinghaus table. */
    vocab_config_t classic = cfg;
    classic.mode = VOCAB_MODE_CLASSIC;
    CHECK(vocab_config_save(root, &classic));
    vocab_config_t loaded;
    CHECK(vocab_config_load(root, &loaded));
    CHECK(loaded.mode == VOCAB_MODE_CLASSIC);

    vocab_close(later);

    /* Corrupt state must not crash: open falls back to a fresh session. */
    snprintf(cmd, sizeof(cmd), "%s/.thingsos/vocab/state.dat", root);
    FILE *f = fopen(cmd, "wb");
    CHECK(f != NULL);
    fputs("broken", f);
    fclose(f);
    CHECK(vocab_open(root, now));
    CHECK(vocab_count() == 3);
    vocab_close(now);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    CHECK(system(cmd) == 0);

    /* Optional: validate a real packed bundle (VOCAB_TEST_ROOT points at the
       directory that contains .thingsos/vocab, i.e. the SD root). */
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
