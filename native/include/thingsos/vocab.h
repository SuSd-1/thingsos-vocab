// SPDX-License-Identifier: Apache-2.0
/* Offline word-card learning: FSRS-6 scheduling on top of a small on-SD bank.
 *
 * The core is plain C with stdio only, so the same code runs on the device and
 * in host tests. All persistent files live under <root>/.thingsos/vocab/.
 * Time is "minutes since 2020-01-01 00:00 local": minute precision is needed
 * for the 1/10 minute learning steps, day precision for review intervals.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VOCAB_WORD_BYTES 64
#define VOCAB_PHONETIC_BYTES 64
#define VOCAB_MEANING_BYTES 256
#define VOCAB_EXAMPLE_BYTES 384
#define VOCAB_ROOT_BYTES 96
#define VOCAB_GROUP_BYTES 24
#define VOCAB_POS_BYTES 12
#define VOCAB_DERIVED_BYTES 160
#define VOCAB_PHRASE_BYTES 200

#define VOCAB_MAX_CARDS 100000u
#define VOCAB_MINUTE_PER_DAY 1440u

typedef enum {
    VOCAB_STATE_NEW = 0,
    VOCAB_STATE_LEARNING = 1,
    VOCAB_STATE_REVIEW = 2,
    VOCAB_STATE_RELEARNING = 3,
} vocab_state_t;

/* Grades map onto FSRS 1..4. */
typedef enum {
    VOCAB_AGAIN = 1,
    VOCAB_HARD = 2,
    VOCAB_GOOD = 3,
    VOCAB_EASY = 4,
} vocab_grade_t;

typedef enum {
    VOCAB_MODE_FSRS = 0,
    VOCAB_MODE_CLASSIC = 1,
} vocab_mode_t;

typedef enum {
    VOCAB_STAGE_WORD = 0,   /* word -> meaning */
    VOCAB_STAGE_RECALL = 1, /* word -> recall meaning */
    VOCAB_STAGE_AUDIO = 2,  /* listen -> meaning */
    VOCAB_STAGE_QUIZ = 3,   /* four meanings, pick the right one */
} vocab_card_stage_t;

typedef struct {
    uint32_t id;
    char word[VOCAB_WORD_BYTES];
    char phonetic[VOCAB_PHONETIC_BYTES];
    char meaning[VOCAB_MEANING_BYTES];
    char example_en[VOCAB_EXAMPLE_BYTES];
    char example_zh[VOCAB_EXAMPLE_BYTES];
    char root[VOCAB_ROOT_BYTES];
    char group[VOCAB_GROUP_BYTES];
    char pos[VOCAB_POS_BYTES];
    char derived[VOCAB_DERIVED_BYTES];
    char phrase[VOCAB_PHRASE_BYTES];
} vocab_card_t;

typedef struct {
    uint16_t new_today;
    uint16_t due_today;
    uint16_t reviews_today;
    uint16_t streak;
    uint32_t total;
    uint32_t learned;
    uint32_t today_seconds;
    uint32_t total_seconds;
    bool finished;
} vocab_stats_t;

typedef struct {
    vocab_mode_t mode;
    uint8_t target_percent; /* 80..95 */
    uint16_t new_limit;
    uint16_t review_limit;
    bool auto_audio;
    uint8_t group_size;   /* cards per group, 5..20 */
    uint8_t error_mode;   /* 0 = relearn after the group, 1 = retry at round end */
    bool side_keys;       /* remap the four side keys while learning/reviewing */
    bool shuffle;         /* pick each group in random order */
    bool auto_example;    /* read the example sentence after the word */
} vocab_config_t;

/* 24 bytes, persisted as-is. */
typedef struct {
    uint16_t reps;
    uint16_t lapses;
    uint32_t due_min;  /* 0 = new / not scheduled */
    uint32_t last_min;
    uint16_t elapsed;  /* days since last review, cached */
    float stability;
    float difficulty;
    uint8_t state;
    uint8_t flags;
} vocab_state_record_t;

/* Lifecycle. root is the SD mount point ("/sdcard"). */
bool vocab_open(const char *root, uint32_t now_min);
void vocab_close(uint32_t now_min);
bool vocab_opened(void);
uint32_t vocab_count(void);

/* Daily session. */
bool vocab_begin_day(uint32_t now_min, const vocab_config_t *cfg);
/* focus: 0 mixed, 1 learning ladder (new/learning/relearning), 2 due reviews,
 * 3 relearning booster only. */
bool vocab_begin_session(uint32_t now_min, const vocab_config_t *cfg, int focus);
bool vocab_next(vocab_card_t *out, vocab_card_stage_t *stage);
bool vocab_answer(uint32_t now_min, vocab_grade_t grade);
bool vocab_undo(uint32_t now_min);
/* Re-queue the card that was just answered at the end of the current review. */
bool vocab_review_retry(uint32_t now_min);
/* UI undo helpers: keep the original answer (cancel) or replace it. */
bool vocab_undo_cancel(uint32_t now_min);
/* Rebuild the current group when it has not started yet (settings change). */
bool vocab_replan_idle(uint32_t now_min);
/* Replace the grade of an already answered card (undo -> 改判). */
bool vocab_regrade_by_id(uint32_t now_min, uint32_t id, vocab_grade_t grade);
bool vocab_undo_regrade(uint32_t now_min, vocab_grade_t grade);
bool vocab_stats(uint32_t now_min, vocab_stats_t *out);
bool vocab_audio_path(uint32_t id, char *buf, size_t bytes);
bool vocab_example_audio_path(uint32_t id, char *buf, size_t bytes);
uint32_t vocab_current_id(void);
/* Load one card by id without touching the queue (peek at a previous word). */
bool vocab_load_by_id(uint32_t id, vocab_card_t *out, vocab_card_stage_t *stage);
/* Drop the saved learning plan so the next session rebuilds it. */
void vocab_abandon_plan(void);
/* Re-read the current card without advancing the queue (used after undo). */
bool vocab_current_card(vocab_card_t *out, vocab_card_stage_t *stage);
uint32_t vocab_learned_count(void);
uint32_t vocab_new_count(void);
uint8_t vocab_current_level(void);
/* State of the card on screen (0 NEW, 1 LEARNING, 2 REVIEW, 3 RELEARNING). */
uint8_t vocab_current_state(void);
/* Group/round cursor of the active learning plan (false when none). */
bool vocab_plan_progress(uint16_t *group, uint8_t *round, uint8_t *index, uint8_t *total,
                         uint8_t *phase);
/* Three meaning strings that are plausibly confusable with `id`. */
bool vocab_distractors(uint32_t id, char out[3][VOCAB_MEANING_BYTES]);
/* Distractors plus the word each meaning belongs to (for the quiz reveal). */
bool vocab_distractors_ex(uint32_t id, char means[3][VOCAB_MEANING_BYTES],
                          char words[3][VOCAB_WORD_BYTES]);

/* Settings. */
void vocab_config_defaults(vocab_config_t *cfg);
bool vocab_config_load(const char *root, vocab_config_t *cfg);
bool vocab_config_save(const char *root, const vocab_config_t *cfg);
/* Study-time accounting (seconds). */
bool vocab_add_seconds(uint32_t seconds);
/* Wipe learning progress (state, journal and plan) but keep the bank/audio. */
bool vocab_reset_progress(void);

/* Log export for offline weight optimisation (best effort). */
bool vocab_export_log(const char *path);

/* Time helpers (YYYYMMDD <-> minutes since 2020-01-01). */
uint32_t vocab_date_to_day(uint32_t yyyymmdd);
uint32_t vocab_day_to_date(uint32_t day);
uint32_t vocab_minutes_from_epoch(uint32_t yyyymmdd, unsigned hour, unsigned minute);
