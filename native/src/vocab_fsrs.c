// SPDX-License-Identifier: Apache-2.0
#include "thingsos/vocab_fsrs.h"

#include <math.h>

#define FSRS_MIN_STABILITY 0.01f
#define FSRS_MAX_STABILITY 36500.0f
#define FSRS_MIN_DIFFICULTY 1.0f
#define FSRS_MAX_DIFFICULTY 10.0f

/* Official FSRS-6 default parameters (open-spaced-repetition). */
static const float FSRS_W[21] = {
    0.212f, 1.2931f, 2.3065f, 8.2956f, 6.4133f, 0.8334f, 3.0194f,
    0.001f, 1.8722f, 0.1666f, 0.796f, 1.4835f, 0.0614f, 0.2629f,
    1.6483f, 0.6014f, 1.8729f, 0.5425f, 0.0912f, 0.0658f, 0.1542f,
};

static float clampf(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

/* F chosen so that R(S, S) = 0.9 for the trainable decay w20. */
static float decay_factor(void) {
    return powf(0.9f, -1.0f / FSRS_W[20]) - 1.0f;
}

static float difficulty_anchor_easy(void) {
    return FSRS_W[4] - expf(FSRS_W[5] * 3.0f) + 1.0f;
}

float vocab_fsrs_retrievability(float stability, float elapsed_days) {
    if (!(stability > 0.0f)) return 0.0f;
    if (elapsed_days <= 0.0f) return 1.0f;
    const float factor = decay_factor();
    return powf(1.0f + factor * elapsed_days / stability, -FSRS_W[20]);
}

float vocab_fsrs_interval(float stability, float target_retention) {
    if (!(stability > 0.0f)) return 1.0f;
    float retention = clampf(target_retention, 0.80f, 0.95f);
    const float factor = decay_factor();
    float days = stability / factor *
                 (powf(retention, -1.0f / FSRS_W[20]) - 1.0f);
    if (!(days >= 1.0f)) days = 1.0f;
    return roundf(days);
}

void vocab_fsrs_initial(int grade, vocab_fsrs_memory_t *memory) {
    int index = grade - 1;
    if (index < 0) index = 0;
    if (index > 3) index = 3;
    float stability = FSRS_W[index];
    float difficulty = FSRS_W[4] - expf(FSRS_W[5] * (float)index) + 1.0f;
    memory->stability = clampf(stability, FSRS_MIN_STABILITY, FSRS_MAX_STABILITY);
    memory->difficulty = clampf(difficulty, FSRS_MIN_DIFFICULTY, FSRS_MAX_DIFFICULTY);
}

void vocab_fsrs_review(
    vocab_fsrs_memory_t *memory, float elapsed_days, int grade, int same_day) {
    if (!memory) return;
    if (!(memory->stability > 0.0f)) {
        vocab_fsrs_initial(grade, memory);
        return;
    }
    if (grade < 1) grade = 1;
    if (grade > 4) grade = 4;

    const float stability = memory->stability;
    const float difficulty = memory->difficulty;
    float next = stability;

    if (same_day) {
        float growth = expf(FSRS_W[17] * ((float)grade - 3.0f + FSRS_W[18])) *
                       powf(stability, -FSRS_W[19]);
        if (grade >= 2 && growth < 1.0f) growth = 1.0f;
        next = stability * growth;
    } else {
        const float recall = vocab_fsrs_retrievability(stability, elapsed_days);
        if (grade == 1) {
            float forgotten = FSRS_W[11] * powf(difficulty, -FSRS_W[12]) *
                              (powf(stability + 1.0f, FSRS_W[13]) - 1.0f) *
                              expf(FSRS_W[14] * (1.0f - recall));
            next = forgotten < stability ? forgotten : stability;
        } else {
            const float hard_penalty = grade == 2 ? FSRS_W[15] : 1.0f;
            const float easy_bonus = grade == 4 ? FSRS_W[16] : 1.0f;
            const float growth = expf(FSRS_W[8]) * (11.0f - difficulty) *
                                 powf(stability, -FSRS_W[9]) *
                                 (expf(FSRS_W[10] * (1.0f - recall)) - 1.0f) *
                                 hard_penalty * easy_bonus;
            next = stability * (1.0f + growth);
        }
    }

    float delta = -FSRS_W[6] * ((float)grade - 3.0f);
    float adjusted = difficulty + delta * (10.0f - difficulty) / 9.0f;
    float next_difficulty = FSRS_W[7] * difficulty_anchor_easy() +
                            (1.0f - FSRS_W[7]) * adjusted;

    memory->stability = clampf(next, FSRS_MIN_STABILITY, FSRS_MAX_STABILITY);
    memory->difficulty =
        clampf(next_difficulty, FSRS_MIN_DIFFICULTY, FSRS_MAX_DIFFICULTY);
}
