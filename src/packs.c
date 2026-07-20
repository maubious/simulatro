#include "internal.h"
#include "content.h"

#include <string.h>

static int add_hallucination_tarot(BalatroState *state) {
    if (state->consumable_count >= state->consumable_slots || state->consumable_count >= BALATRO_MAX_CONSUMABLES) return 0;
    double chance = 0.5;
    for (uint8_t i = 0; i < state->joker_count; ++i)
        if (!(state->jokers[i].flags & 1u) && state->jokers[i].center_id == BALATRO_CENTER_J_OOPS) chance *= 2.0;
    char stream[32];
    balatro_key_with_u64(stream, sizeof(stream), "halu", state->ante);
    if (balatro_pseudorandom(state, stream) >= chance) return 0;
    BalatroCard card = balatro_create_pooled_card(state, SET_TAROT, "hal", 0);
    state->consumables[state->consumable_count++] = card;
    balatro_consumable_added(state, &card);
    return 1;
}

static int resolves_to_hallucination(const BalatroState *state, uint8_t start) {
    uint8_t index = start;
    for (uint8_t depth = 0; depth <= state->joker_count; ++depth) {
        if (index >= state->joker_count) return 0;
        const BalatroCard *joker = &state->jokers[index];
        if (joker->flags & 1u) return 0;
        if (joker->center_id == BALATRO_CENTER_J_BLUEPRINT) {
            index++;
            continue;
        }
        if (joker->center_id == BALATRO_CENTER_J_BRAINSTORM) {
            if (index == 0) return 0;
            index = 0;
            continue;
        }
        return joker->center_id == BALATRO_CENTER_J_HALLUCINATION;
    }
    return 0;
}

static BalatroCard standard_pack_card(BalatroState *state) {
    BalatroCard card = {0};
    char stream[32];
    balatro_key_with_u64(stream, sizeof(stream), "stdset", state->ante);
    int enhanced = balatro_pseudorandom(state, stream) > 0.6;
    balatro_key_with_u64(stream, sizeof(stream), "frontsta", state->ante);
    size_t front = (size_t)(balatro_pseudorandom(state, stream) * 52.0);
    BalatroPlayingCardDefinition definition = balatro_playing_card((uint8_t)front);
    card.suit = definition.suit;
    card.rank = definition.rank;
    card.center_id = BALATRO_CENTER_C_BASE;
    if (enhanced) {
        size_t count = 0;
        const uint16_t *pool = balatro_center_pool(2, &count);
        balatro_key_with_u64(stream, sizeof(stream), "Enhancedsta", state->ante);
        size_t index = (size_t)(balatro_pseudorandom(state, stream) * count);
        card.center_id = pool[index];
        card.enhancement = (uint8_t)(index + 1);
    }
    balatro_key_with_u64(stream, sizeof(stream), "standard_edition", state->ante);
    double edition = balatro_pseudorandom(state, stream);
    if (edition > 1.0 - 0.012 * state->edition_rate)
        card.edition = 3;
    else if (edition > 1.0 - 0.04 * state->edition_rate)
        card.edition = 2;
    else if (edition > 1.0 - 0.08 * state->edition_rate)
        card.edition = 1;
    balatro_key_with_u64(stream, sizeof(stream), "stdseal", state->ante);
    if (balatro_pseudorandom(state, stream) > 0.8) {
        balatro_key_with_u64(stream, sizeof(stream), "stdsealtype", state->ante);
        double seal = balatro_pseudorandom(state, stream);
        card.seal = seal > 0.75 ? 2 : seal > 0.5 ? 3 : seal > 0.25 ? 1 : 4;
    }
    card.sort_id = ++state->next_sort_id;
    card.cost = card.sell_cost = 1;
    return card;
}

static uint16_t telescope_planet(const BalatroState *state) {
    if (!balatro_center_used(state, BALATRO_CENTER_V_TELESCOPE)) return 0;
    uint8_t hand = balatro_most_played_hand(state);
    if (!state->hand_plays[hand]) return 0;
    return balatro_planet_center(hand);
}

static void remove_shop_booster(BalatroState *state, uint8_t index) {
    memmove(&state->shop_boosters[index], &state->shop_boosters[index + 1],
            (state->shop_booster_count - index - 1) * sizeof(BalatroCard));
    state->shop_booster_count--;
}

static int open_pack_center(BalatroState *state, uint16_t center_id) {
    const BalatroCenterDefinition *definition = &balatro_centers[center_id];
    state->pack_count = 0;
    state->pack_choices = definition->pack_choose;
    state->pack_kind = definition->kind;
    /* A duplicated tag can open a second pack after the first one is closed.
       Preserve the original return phase while that queued pack is opened. */
    if (state->phase != BALATRO_PHASE_PACK_OPENING) state->shop_return_phase = state->phase;
    uint8_t set = definition->kind == 1   ? SET_TAROT
                  : definition->kind == 2 ? SET_PLANET
                  : definition->kind == 3 ? SET_SPECTRAL
                  : definition->kind == 5 ? SET_JOKER
                                          : SET_DEFAULT;
    const char *append = definition->kind == 1   ? "ar1"
                         : definition->kind == 2 ? "pl1"
                         : definition->kind == 3 ? "spe"
                         : definition->kind == 5 ? "buf"
                                                 : "sta";
    uint16_t temporary[BALATRO_MAX_PACK_CARDS];
    uint8_t temporary_count = 0;
    for (uint8_t i = 0; i < definition->pack_extra; ++i) {
        if (state->pack_count >= BALATRO_MAX_PACK_CARDS) return BALATRO_ERR_CAPACITY;
        BalatroCard card = {0};
        uint16_t telescope = definition->kind == 2 && i == 0 ? telescope_planet(state) : 0;
        uint8_t card_set = set;
        const char *card_append = append;
        if (definition->kind == 1 && balatro_center_used(state, BALATRO_CENTER_V_OMEN_GLOBE) &&
            balatro_pseudorandom(state, "omen_globe") > 0.8) {
            card_set = SET_SPECTRAL;
            card_append = "ar2";
        }
        if (telescope) {
            card.center_id = telescope;
            card.sort_id = ++state->next_sort_id;
            card.cost = balatro_centers[telescope].cost;
            card.sell_cost = card.cost / 2 > 0 ? card.cost / 2 : 1;
        } else
            card = card_set == SET_DEFAULT ? standard_pack_card(state) : balatro_create_pooled_card(state, card_set, card_append, 1);
        state->pack_cards[state->pack_count++] = card;
        if (card.center_id != BALATRO_CENTER_C_BASE && !balatro_center_used(state, card.center_id)) {
            balatro_mark_center_used(state, card.center_id);
            temporary[temporary_count++] = card.center_id;
        }
    }
    for (uint8_t i = 0; i < temporary_count; ++i) balatro_unmark_center_used(state, temporary[i]);
    for (uint8_t i = 0; i < state->joker_count; ++i)
        if (resolves_to_hallucination(state, i)) (void)add_hallucination_tarot(state);
    /* Arcana and Spectral pack states expose a hand so targeted consumables
       can be used. Their state-update handlers call draw_from_deck_to_hand;
       the other three pack kinds do not. */
    if (definition->kind == 1 || definition->kind == 3) balatro_draw_to_hand(state);
    state->phase = BALATRO_PHASE_PACK_OPENING;
    return BALATRO_OK;
}

int balatro_open_booster(BalatroState *state, uint8_t index) {
    if (!state || index >= state->shop_booster_count) return BALATRO_ERR_ACTION;
    BalatroCard booster = state->shop_boosters[index];
    if (!balatro_can_afford(state, booster.cost)) return BALATRO_ERR_ACTION;
    state->dollars -= booster.cost;
    remove_shop_booster(state, index);
    return open_pack_center(state, booster.center_id);
}

int balatro_open_free_pack(BalatroState *state, uint16_t center_id) {
    if (!state || center_id >= BALATRO_CENTER_COUNT || balatro_centers[center_id].set != SET_BOOSTER) return BALATRO_ERR_ACTION;
    if (state->phase == BALATRO_PHASE_PACK_OPENING) {
        /* Double Tag may produce two pack effects in one blind-select pass.
           Keep the second pack instead of replacing the visible first pack. */
        if (state->pending_free_pack_id) return BALATRO_ERR_CAPACITY;
        state->pending_free_pack_id = center_id;
        return BALATRO_OK;
    }
    return open_pack_center(state, center_id);
}

static void finish_pack(BalatroState *state) {
    uint8_t return_hand = state->pack_kind == 1 || state->pack_kind == 3;
    state->pack_count = 0;
    state->pack_choices = 0;
    state->pack_kind = 0;
    /* end_consumeable calls draw_from_hand_to_deck before restoring the
       interrupted shop/blind-select state. */
    if (return_hand && state->hand_count) {
        /* draw_from_hand_to_deck removes G.hand.cards[1] each time, while a
           deck emplaces every returned card at index 1. The sorted hand is
           therefore prepended in reverse order at the bottom of the deck;
           appending it would put those cards on top and change a second
           Arcana/Spectral pack opened before the next blind. */
        uint8_t returning = state->hand_count;
        if ((size_t)state->deck_count + returning > BALATRO_MAX_DECK) returning = (uint8_t)(BALATRO_MAX_DECK - state->deck_count);
        memmove(&state->deck[returning], &state->deck[0], state->deck_count * sizeof(BalatroCard));
        for (uint8_t i = 0; i < returning; ++i) state->deck[i] = state->hand[state->hand_count - 1 - i];
        state->deck_count += returning;
        state->hand_count = 0;
    }
    if (state->pending_free_pack_id) {
        uint16_t next = state->pending_free_pack_id;
        state->pending_free_pack_id = 0;
        if (open_pack_center(state, next) == BALATRO_OK) return;
    }
    state->phase = state->shop_return_phase;
}

int balatro_pick_pack_card(BalatroState *state, uint8_t index) {
    if (!state || state->phase != BALATRO_PHASE_PACK_OPENING || index >= state->pack_count) return BALATRO_ERR_ACTION;
    BalatroCard card = state->pack_cards[index];
    uint8_t set = balatro_card_set(&card);
    if (set == SET_JOKER) {
        if (state->joker_count >= BALATRO_MAX_JOKERS || (state->joker_count >= state->joker_slots && card.edition != 4))
            return BALATRO_ERR_CAPACITY;
        state->jokers[state->joker_count++] = card;
        balatro_joker_added(state, &state->jokers[state->joker_count - 1]);
        balatro_mark_center_used(state, card.center_id);
    } else if (set == SET_TAROT || set == SET_PLANET || set == SET_SPECTRAL) {
        if (state->consumable_count >= BALATRO_MAX_CONSUMABLES || (state->consumable_count >= state->consumable_slots && card.edition != 4))
            return BALATRO_ERR_CAPACITY;
        state->consumables[state->consumable_count++] = card;
        balatro_consumable_added(state, &card);
    } else if (set == SET_DEFAULT || set == SET_ENHANCED) {
        if (state->deck_count >= BALATRO_MAX_DECK || !balatro_can_add_playing_cards(state, 1)) return BALATRO_ERR_CAPACITY;
        /* CardArea:emplace always inserts at index 1 for a deck. */
        memmove(&state->deck[1], &state->deck[0], state->deck_count * sizeof(BalatroCard));
        state->deck[0] = card;
        state->deck_count++;
        balatro_playing_card_added(state, 1);
    } else
        return BALATRO_ERR_ACTION;
    balatro_complete_pack_pick(state, index);
    return BALATRO_OK;
}

void balatro_complete_pack_pick(BalatroState *state, uint8_t index) {
    memmove(&state->pack_cards[index], &state->pack_cards[index + 1], (state->pack_count - index - 1) * sizeof(BalatroCard));
    state->pack_count--;
    if (--state->pack_choices == 0 || state->pack_count == 0) finish_pack(state);
}

int balatro_skip_pack(BalatroState *state) {
    if (!state || state->phase != BALATRO_PHASE_PACK_OPENING) return BALATRO_ERR_ACTION;
    finish_pack(state);
    return BALATRO_OK;
}
