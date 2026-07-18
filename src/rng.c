#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const double BALATRO_PI = 3.14159265358979323846;
static uint64_t bits_of_double(double value);

void balatro_key_begin(BalatroKeyBuilder *builder, char *data, size_t capacity) {
    *builder = (BalatroKeyBuilder){.data = data, .capacity = capacity};
    if (capacity) data[0] = '\0';
}

void balatro_key_append(BalatroKeyBuilder *builder, const char *text) {
    if (!builder->capacity) return;
    while (*text && builder->length + 1 < builder->capacity) builder->data[builder->length++] = *text++;
    builder->data[builder->length] = '\0';
}

void balatro_key_append_u64(BalatroKeyBuilder *builder, uint64_t value) {
    char reversed[20];
    size_t count = 0;
    do {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count && builder->length + 1 < builder->capacity) builder->data[builder->length++] = reversed[--count];
    if (builder->capacity) builder->data[builder->length] = '\0';
}

void balatro_key_with_u64(char *data, size_t capacity, const char *prefix, uint64_t value) {
    BalatroKeyBuilder builder;
    balatro_key_begin(&builder, data, capacity);
    balatro_key_append(&builder, prefix);
    balatro_key_append_u64(&builder, value);
}

static double positive_fraction(double value) {
    return value - floor(value);
}

double balatro_round_decimal13(double value) {
#if defined(__SIZEOF_INT128__)
    if (!(value > 0.0)) return 0.0;
    uint64_t bits = bits_of_double(value);
    unsigned exponent = (unsigned)((bits >> 52) & 0x7ffu);
    if (!exponent) return 0.0;
    uint64_t significand = (bits & UINT64_C(0x000fffffffffffff)) | UINT64_C(0x0010000000000000);
    unsigned shift = 1075u - exponent;
    __uint128_t scaled = (__uint128_t)significand * UINT64_C(10000000000000);
    uint64_t rounded;
    if (shift >= 128) {
        rounded = 0;
    } else {
        __uint128_t quotient = scaled >> shift;
        __uint128_t remainder = scaled - (quotient << shift);
        __uint128_t halfway = (__uint128_t)1 << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (quotient & 1))) quotient++;
        rounded = (uint64_t)quotient;
    }
    return (double)rounded / 10000000000000.0;
#else
    char rounded[32];
    (void)snprintf(rounded, sizeof(rounded), "%.13f", value);
    return fabs(strtod(rounded, NULL));
#endif
}

static uint64_t bits_of_double(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static double double_of_bits(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t hash_key(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    while (*text) {
        hash ^= (uint8_t)*text++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t tw223_step(uint64_t state[4]) {
    uint64_t z = state[0];
    z = (((z << 31) ^ z) >> 45) ^ ((z & (UINT64_MAX << 1)) << 18);
    uint64_t result = state[0] = z;
    z = state[1];
    z = (((z << 19) ^ z) >> 30) ^ ((z & (UINT64_MAX << 6)) << 28);
    result ^= state[1] = z;
    z = state[2];
    z = (((z << 24) ^ z) >> 48) ^ ((z & (UINT64_MAX << 9)) << 7);
    result ^= state[2] = z;
    z = state[3];
    z = (((z << 21) ^ z) >> 39) ^ ((z & (UINT64_MAX << 17)) << 8);
    result ^= state[3] = z;
    return result;
}

static void luajit_seed(double seed, uint64_t state[4]) {
    static const uint64_t minimum[4] = {UINT64_C(2), UINT64_C(64), UINT64_C(512), UINT64_C(131072)};
    double value = seed;
    for (size_t i = 0; i < 4; ++i) {
        value = value * 3.14159265358979323846 + 2.7182818284590452354;
        state[i] = bits_of_double(value);
        if (state[i] < minimum[i]) state[i] += minimum[i];
    }
    for (size_t i = 0; i < 10; ++i) tw223_step(state);
}

static double luajit_next(uint64_t state[4]) {
    uint64_t bits = (tw223_step(state) & UINT64_C(0x000fffffffffffff)) | UINT64_C(0x3ff0000000000000);
    return double_of_bits(bits) - 1.0;
}

static double luajit_random(double seed) {
    uint64_t state[4];
    luajit_seed(seed, state);
    return luajit_next(state);
}

double balatro_pseudohash(const char *text) {
    size_t length = strlen(text);
    double number = 1.0;
    for (size_t i = length; i > 0; --i) {
        number = positive_fraction((1.1239285023 / number) * (uint8_t)text[i - 1] * BALATRO_PI + BALATRO_PI * (double)i);
    }
    return number;
}

void balatro_rng_reset(BalatroState *state) {
    state->hashed_seed = balatro_pseudohash(state->seed);
    state->rng_count = 0;
    memset(state->rng, 0, sizeof(state->rng));
}

static double advance_seed(BalatroState *state, const char *stream) {
    uint64_t key = hash_key(stream);
    BalatroRngStream *slot = NULL;
    for (uint16_t i = 0; i < state->rng_count; ++i) {
        if (state->rng[i].key_hash == key) {
            slot = &state->rng[i];
            break;
        }
    }
    if (!slot) {
        if (state->rng_count >= BALATRO_MAX_RNG_STREAMS) return 0.0;
        slot = &state->rng[state->rng_count++];
        slot->key_hash = key;
        char combined[96];
        size_t stream_length = strlen(stream);
        size_t seed_length = strlen(state->seed);
        if (stream_length + seed_length >= sizeof(combined)) return 0.0;
        memcpy(combined, stream, stream_length);
        memcpy(combined + stream_length, state->seed, seed_length + 1);
        slot->value = balatro_pseudohash(combined);
    }
    double raw = positive_fraction(2.134453429141 + slot->value * 1.72431234);
    slot->value = balatro_round_decimal13(raw);
    return (slot->value + state->hashed_seed) / 2.0;
}

double balatro_pseudorandom(BalatroState *state, const char *stream) {
    return luajit_random(advance_seed(state, stream));
}

void balatro_shuffle(BalatroState *state, BalatroCard *cards, size_t count, const char *stream) {
    if (count < 2) return;
    /* Canonicalize CardArea entries by sort_id before applying Fisher–Yates.
       This matters for every per-round and cash-out shuffle because the
       current array order is not canonical. */
    uint8_t order[BALATRO_MAX_DECK];
    order[0] = 0;
    for (size_t i = 1; i < count; ++i) {
        uint8_t index = (uint8_t)i;
        size_t j = i;
        while (j > 0 && cards[order[j - 1]].sort_id > cards[index].sort_id) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = index;
    }
    BalatroCard sorted[BALATRO_MAX_DECK];
    for (size_t i = 0; i < count; ++i) sorted[i] = cards[order[i]];
    memcpy(cards, sorted, count * sizeof(cards[0]));
    uint64_t tw_state[4];
    luajit_seed(advance_seed(state, stream), tw_state);
    for (size_t i = count - 1; i > 0; --i) {
        size_t j = (size_t)floor(luajit_next(tw_state) * (double)(i + 1));
        BalatroCard tmp = cards[i];
        cards[i] = cards[j];
        cards[j] = tmp;
    }
}
