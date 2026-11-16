// SPDX-License-Identifier: Apache-2.0
/* FSRS-6 memory model (21 weights, official defaults).
 * Pure arithmetic so the host test can check it against reference vectors.
 */
#pragma once

typedef struct {
    float stability;  /* days */
    float difficulty; /* 1..10 */
} vocab_fsrs_memory_t;

/* Fresh card, first rating. */
void vocab_fsrs_initial(int grade, vocab_fsrs_memory_t *memory);

/* Apply a review. `elapsed_days` is time since the last review; when
 * `same_day` is true the same-day stability branch is used. */
void vocab_fsrs_review(
    vocab_fsrs_memory_t *memory, float elapsed_days, int grade, int same_day);

/* Recall probability after `elapsed_days`. */
float vocab_fsrs_retrievability(float stability, float elapsed_days);

/* Whole-day interval for the requested retention (0.80..0.95). */
float vocab_fsrs_interval(float stability, float target_retention);
