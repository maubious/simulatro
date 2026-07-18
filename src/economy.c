#include "balatro.h"

#include <math.h>

double balatro_blind_amount(uint8_t ante, uint8_t scaling) {
    static const int32_t amounts[3][8] = {
        {300, 800, 2000, 5000, 11000, 20000, 35000, 50000},
        {300, 900, 2600, 8000, 20000, 36000, 60000, 100000},
        {300, 1000, 3200, 9000, 25000, 60000, 110000, 200000},
    };
    if (ante < 1) return 100;
    if (scaling < 1 || scaling > 3) scaling = 1;
    if (ante <= 8) return amounts[scaling - 1][ante - 1];
    double c = (double)ante - 8.0;
    double d = 1.0 + 0.2 * c;
    double raw = floor(amounts[scaling - 1][7] * pow(1.6 + pow(0.75 * c, d), c));
    double rounding = pow(10.0, floor(log10(raw) - 1.0));
    /* This deliberately propagates NaN when raw overflows to infinity,
       matching Lua's `inf - inf % inf` in get_blind_amount. */
    return raw - fmod(raw, rounding);
}

double balatro_blind_target(uint8_t ante, uint8_t blind, uint8_t scaling) {
    double amount = balatro_blind_amount(ante, scaling);
    if (blind == 0) return amount;
    if (blind == 1) return amount * 1.5;
    return amount * 2.0;
}

int32_t balatro_calculate_round_earnings(const BalatroState *state) {
    if (!state) return 0;
    int32_t interest = 0;
    if (state->dollars >= 5 && state->config.deck != BALATRO_CENTER_B_GREEN) {
        int32_t brackets = state->dollars / 5;
        int32_t cap = state->interest_cap / 5;
        if (brackets > cap) brackets = cap;
        interest = brackets * state->interest_amount;
    }
    int bonus = 0;
    int nines = 0;
    for (uint16_t i = 0; i < state->deck_count; ++i)
        if (state->deck[i].rank == 9) nines++;
    for (uint8_t i = 0; i < state->hand_count; ++i)
        if (state->hand[i].rank == 9) nines++;
    for (uint16_t i = 0; i < state->discard_count; ++i)
        if (state->discard[i].rank == 9) nines++;
    for (uint8_t i = 0; i < state->joker_count; ++i) {
        const BalatroCard *joker = &state->jokers[i];
        if (joker->flags & 1u) continue;
        switch (joker->center_id) {
        case BALATRO_CENTER_J_GOLDEN:
            bonus += 4;
            break;
        case BALATRO_CENTER_J_CLOUD_9:
            bonus += nines;
            break;
        case BALATRO_CENTER_J_ROCKET:
            bonus += joker->state[0] > 0 ? joker->state[0] : 1;
            break;
        case BALATRO_CENTER_J_DELAYED_GRAT:
            if (!state->discards_used && state->discards_left) bonus += state->discards_left * 2;
            break;
        case BALATRO_CENTER_J_SATELLITE:
            for (uint16_t mask = state->planet_usage_mask; mask; mask &= (uint16_t)(mask - 1)) bonus++;
            break;
        default:
            break;
        }
    }
    int hand_bonus = state->hands_left * (state->config.deck == BALATRO_CENTER_B_GREEN ? 2 : 1);
    int discard_bonus = state->config.deck == BALATRO_CENTER_B_GREEN ? state->discards_left : 0;
    return state->blind_reward + hand_bonus + discard_bonus + interest + bonus;
}
