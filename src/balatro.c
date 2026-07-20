#include "internal.h"
#include "content.h"

#include <math.h>
#include <string.h>

static int joker_active(const BalatroState *state, uint16_t center_id) {
    return balatro_joker_active(state, center_id);
}

void reset_round_rerolls(BalatroState *state) {
    uint8_t chaos = 0;
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (!(state->jokers[j].flags & BALATRO_CARD_DEBUFFED) && state->jokers[j].center_id == BALATRO_CENTER_J_CHAOS && chaos < UINT8_MAX)
            chaos++;
    state->free_rerolls = chaos;
    state->reroll_increase = 0;
    state->reroll_cost = chaos ? 0 : (state->tag_d_six_active ? 0 : state->reroll_base);
}

int8_t blind_reward_for(uint16_t blind_id, uint8_t blind_on_deck) {
    if (blind_id == BALATRO_BLIND_BL_FINAL_ACORN || blind_id == BALATRO_BLIND_BL_FINAL_BELL || blind_id == BALATRO_BLIND_BL_FINAL_HEART ||
        blind_id == BALATRO_BLIND_BL_FINAL_LEAF || blind_id == BALATRO_BLIND_BL_FINAL_VESSEL)
        return 8;
    return blind_on_deck == 0 ? 3 : blind_on_deck == 1 ? 4 : 5;
}

/* Forward declarations for skip-tag helpers defined before the general
   Joker/boss construction routines below. */
int add_joker_rarity(BalatroState *state, uint8_t rarity, const char *append, int legendary);
uint16_t choose_boss(BalatroState *state);
void remove_joker_at(BalatroState *state, uint8_t index);
void sort_hand_desc(BalatroState *state);
int find_discard_card(const BalatroState *state, uint16_t sort_id);

/* LuaJIT 2.1.0-beta3 pairs(G.GAME.hands) order for the hand table.
   To Do List and the blind-select Orbital choices filter this order to hands
   currently visible, then pseudorandom_element preserves the numeric array. */
static const uint8_t lua_hand_order[] = {
    BALATRO_FLUSH_HOUSE, BALATRO_FULL_HOUSE, BALATRO_FLUSH,      BALATRO_PAIR,           BALATRO_HIGH_CARD,       BALATRO_STRAIGHT_FLUSH,
    BALATRO_STRAIGHT,    BALATRO_TWO_PAIR,   BALATRO_FLUSH_FIVE, BALATRO_FIVE_OF_A_KIND, BALATRO_THREE_OF_A_KIND, BALATRO_FOUR_OF_A_KIND,
};

static int hand_visible(const BalatroState *state, uint8_t hand) {
    return hand >= BALATRO_STRAIGHT_FLUSH || state->hand_plays[hand] != 0;
}

uint8_t choose_to_do_hand(BalatroState *state, int excluded) {
    uint8_t candidates[BALATRO_HAND_COUNT];
    size_t count = 0;
    for (size_t i = 0; i < sizeof(lua_hand_order); ++i)
        if (hand_visible(state, lua_hand_order[i]) && (int)lua_hand_order[i] != excluded) candidates[count++] = lua_hand_order[i];
    size_t index = (size_t)floor(balatro_pseudorandom(state, "to_do") * count);
    return candidates[index];
}

void choose_orbital_hands(BalatroState *state) {
    uint8_t visible[BALATRO_HAND_COUNT];
    uint8_t count = 0;
    for (size_t i = 0; i < sizeof(lua_hand_order); ++i)
        if (hand_visible(state, lua_hand_order[i])) visible[count++] = lua_hand_order[i];
    for (uint8_t blind = 0; blind < 3; ++blind) {
        size_t pick = (size_t)floor(balatro_pseudorandom(state, "orbital") * count);
        state->orbital_hands[blind] = visible[pick];
    }
}

void balatro_initialize_joker_card(BalatroState *state, BalatroCard *card) {
    /* Card:set_ability runs in Card:init, before a Joker is bought or emplaced.
       Shop and pack inventory therefore consume stateful construction RNG too.
       state[2] prevents Card:add_to_deck from initializing the card again. */
    if (card->center_id == BALATRO_CENTER_J_TODO_LIST && card->state[2] == 0) {
        card->state[1] = choose_to_do_hand(state, -1);
        card->state[2] = 1;
    }
}

static uint8_t choose_tag(BalatroState *state) {
    static const uint8_t tags[] = {
        BALATRO_TAG_TAG_UNCOMMON,   BALATRO_TAG_TAG_RARE,       BALATRO_TAG_TAG_NEGATIVE, BALATRO_TAG_TAG_FOIL,    BALATRO_TAG_TAG_HOLO,
        BALATRO_TAG_TAG_POLYCHROME, BALATRO_TAG_TAG_INVESTMENT, BALATRO_TAG_TAG_VOUCHER,  BALATRO_TAG_TAG_BOSS,    BALATRO_TAG_TAG_STANDARD,
        BALATRO_TAG_TAG_CHARM,      BALATRO_TAG_TAG_METEOR,     BALATRO_TAG_TAG_BUFFOON,  BALATRO_TAG_TAG_HANDY,   BALATRO_TAG_TAG_GARBAGE,
        BALATRO_TAG_TAG_ETHEREAL,   BALATRO_TAG_TAG_COUPON,     BALATRO_TAG_TAG_DOUBLE,   BALATRO_TAG_TAG_JUGGLE,  BALATRO_TAG_TAG_D_SIX,
        BALATRO_TAG_TAG_TOP_UP,     BALATRO_TAG_TAG_SKIP,       BALATRO_TAG_TAG_ORBITAL,  BALATRO_TAG_TAG_ECONOMY,
    };
    static const uint8_t minimum[] = {
        1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 2, 1, 2, 1,
    };
    /* Preserve UNAVAILABLE entries in the pool and resample with
       Tag{ante}_resample{n}.  Keeping the original 24-entry alignment is
       required for seed parity.  The pinned profile treats discovered
       centers as fully unlocked, so requirements do not remove entries. */
    uint8_t available[sizeof(tags)];
    uint8_t ante = state->ante;
    for (size_t i = 0; i < sizeof(tags); ++i) {
        available[i] = ante >= minimum[i];
    }
    char stream[32];
    for (unsigned attempt = 0; attempt <= 20; ++attempt) {
        BalatroKeyBuilder key;
        balatro_key_begin(&key, stream, sizeof(stream));
        balatro_key_append(&key, "Tag");
        balatro_key_append_u64(&key, state->ante);
        if (attempt) {
            balatro_key_append(&key, "_resample");
            balatro_key_append_u64(&key, attempt + 1);
        }
        size_t index = (size_t)floor(balatro_pseudorandom(state, stream) * sizeof(tags));
        if (available[index]) return tags[index];
    }
    return BALATRO_TAG_TAG_HANDY;
}

void assign_blind_tags(BalatroState *state) {
    state->blind_tags[0] = choose_tag(state);
    state->blind_tags[1] = choose_tag(state);
}

void apply_skip_tag(BalatroState *state, uint8_t tag) {
    state->active_tag = tag;
    switch (tag) {
    case BALATRO_TAG_TAG_UNCOMMON:
    case BALATRO_TAG_TAG_RARE: {
        uint8_t rarity = tag == BALATRO_TAG_TAG_RARE ? 3 : 2;
        if (state->tag_force_rarity == rarity) {
            if (state->tag_force_rarity_count < UINT8_MAX) state->tag_force_rarity_count++;
        } else {
            state->tag_force_rarity = rarity;
            state->tag_force_rarity_count = 1;
        }
        break;
    }
    case BALATRO_TAG_TAG_FOIL:
    case BALATRO_TAG_TAG_HOLO:
    case BALATRO_TAG_TAG_POLYCHROME:
    case BALATRO_TAG_TAG_NEGATIVE: {
        uint8_t edition = tag == BALATRO_TAG_TAG_FOIL ? 1 : tag == BALATRO_TAG_TAG_HOLO ? 2 : tag == BALATRO_TAG_TAG_POLYCHROME ? 3 : 4;
        if (state->tag_force_edition == edition) {
            if (state->tag_force_edition_count < UINT8_MAX) state->tag_force_edition_count++;
        } else {
            state->tag_force_edition = edition;
            state->tag_force_edition_count = 1;
        }
        break;
    }
    case BALATRO_TAG_TAG_INVESTMENT:
        if (state->tag_investment_pending < UINT8_MAX) state->tag_investment_pending++;
        break;
    case BALATRO_TAG_TAG_VOUCHER:
        if (state->tag_voucher_pending < UINT8_MAX) state->tag_voucher_pending++;
        break;
    case BALATRO_TAG_TAG_DOUBLE:
        state->double_tag = 1;
        break;
    case BALATRO_TAG_TAG_ECONOMY: {
        int gain = state->dollars < 40 ? state->dollars : 40;
        state->dollars += gain;
        break;
    }
    case BALATRO_TAG_TAG_GARBAGE:
        state->dollars += state->unused_discards;
        state->unused_discards = 0;
        break;
    case BALATRO_TAG_TAG_HANDY:
        state->dollars += (int32_t)state->run_hands_played;
        break;
    case BALATRO_TAG_TAG_SKIP:
        state->dollars += state->skips * 5;
        break;
    case BALATRO_TAG_TAG_TOP_UP:
        /* Top-up creates two common Jokers with key_append='top'. This uses
           the full pool pipeline:
           Joker1top{ante}, UNAVAILABLE-preserving resamples, and editop{ante}
           for each created card. */
        (void)add_joker_rarity(state, 1, "top", 0);
        (void)add_joker_rarity(state, 1, "top", 0);
        break;
    case BALATRO_TAG_TAG_ORBITAL: {
        uint8_t hand = state->orbital_hands[state->blind_on_deck];
        state->hand_levels[hand] = (uint8_t)(state->hand_levels[hand] > 252 ? 255 : state->hand_levels[hand] + 3);
        break;
    }
    case BALATRO_TAG_TAG_JUGGLE:
        state->tag_hand_bonus = (uint8_t)(state->tag_hand_bonus > UINT8_MAX - 3 ? UINT8_MAX : state->tag_hand_bonus + 3);
        break;
    case BALATRO_TAG_TAG_D_SIX:
        state->tag_d_six_pending = 1;
        break;
    case BALATRO_TAG_TAG_COUPON:
        state->tag_coupon_pending = 1;
        break;
    case BALATRO_TAG_TAG_BOSS:
        /* The free Boss Tag uses the same callback as the voucher
           button and therefore consumes Director's Cut for this ante. */
        state->boss_rerolled = 1;
        state->next_boss_id = choose_boss(state);
        break;
    case BALATRO_TAG_TAG_STANDARD:
        (void)balatro_open_free_pack(state, BALATRO_CENTER_P_STANDARD_MEGA_1);
        break;
    case BALATRO_TAG_TAG_CHARM:
        (void)balatro_open_free_pack(state, BALATRO_CENTER_P_ARCANA_MEGA_1);
        break;
    case BALATRO_TAG_TAG_METEOR:
        (void)balatro_open_free_pack(state, BALATRO_CENTER_P_CELESTIAL_MEGA_1);
        break;
    case BALATRO_TAG_TAG_BUFFOON:
        (void)balatro_open_free_pack(state, BALATRO_CENTER_P_BUFFOON_MEGA_1);
        break;
    case BALATRO_TAG_TAG_ETHEREAL:
        (void)balatro_open_free_pack(state, BALATRO_CENTER_P_SPECTRAL_NORMAL_1);
        break;
    default:
        break;
    }
}

int add_pooled_consumable(BalatroState *state, uint8_t set, const char *append, uint8_t edition) {
    BalatroCard card = balatro_create_pooled_card(state, set, append, 0);
    card.edition = edition;
    if (state->consumable_count >= BALATRO_MAX_CONSUMABLES || (state->consumable_count >= state->consumable_slots && card.edition != 4))
        return 0;
    state->consumables[state->consumable_count++] = card;
    balatro_consumable_added(state, &card);
    return 1;
}

int add_specific_consumable(BalatroState *state, uint16_t center_id) {
    if (state->consumable_count >= state->consumable_slots || state->consumable_count >= BALATRO_MAX_CONSUMABLES) return 0;
    BalatroCard card = {0};
    card.center_id = center_id;
    card.sort_id = ++state->next_sort_id;
    card.cost = balatro_centers[center_id].cost;
    card.sell_cost = card.cost / 2 > 0 ? card.cost / 2 : 1;
    state->consumables[state->consumable_count++] = card;
    balatro_consumable_added(state, &card);
    return 1;
}

void balatro_joker_added(BalatroState *state, const BalatroCard *joker) {
    BalatroCard *mutable_joker = (BalatroCard *)joker;
    /* Negative editions grant a real Joker-slot expansion while the card is
       in the collection (Card:add_to_deck/remove_from_deck). */
    if (joker->edition == 4 && state->joker_slots < UINT8_MAX) state->joker_slots++;
    if (joker->center_id == BALATRO_CENTER_J_TO_THE_MOON) state->interest_amount++;
    balatro_initialize_joker_card(state, mutable_joker);
    if (joker->center_id == BALATRO_CENTER_J_IDOL) {
        int copied_target = 0;
        for (uint8_t i = 0; i + 1 < state->joker_count; ++i)
            if (&state->jokers[i] != joker && !(state->jokers[i].flags & 1u) && state->jokers[i].center_id == BALATRO_CENTER_J_IDOL) {
                mutable_joker->state[0] = state->jokers[i].state[0];
                mutable_joker->state[1] = state->jokers[i].state[1];
                copied_target = 1;
                break;
            }
        if (!copied_target && state->idol_rank) {
            mutable_joker->state[0] = state->idol_rank;
            mutable_joker->state[1] = state->idol_suit;
        }
    }
    if (joker->center_id == BALATRO_CENTER_J_MAIL && state->mail_rank) mutable_joker->state[1] = state->mail_rank;
    if (joker->center_id == BALATRO_CENTER_J_CASTLE && state->castle_suit < 4) mutable_joker->state[1] = state->castle_suit;
    if (joker->center_id == BALATRO_CENTER_J_LOYALTY_CARD && joker->state[2] == 0) {
        mutable_joker->state[1] = (int32_t)state->run_hands_played;
        mutable_joker->state[2] = 1;
    }
    /* Card:add_to_deck changes G.hand's limit in every state, including the
       shop. This matters when an Arcana/Spectral pack is opened before the
       next blind and immediately draws to that enlarged limit. */
    uint8_t old_hand_size = state->hand_size;
    if (joker->center_id == BALATRO_CENTER_J_JUGGLER) {
        state->hand_size++;
    } else if (joker->center_id == BALATRO_CENTER_J_MERRY_ANDY) {
        if (state->hand_size > 1) state->hand_size--;
        state->discards_left = state->discards_left > UINT8_MAX - 3 ? UINT8_MAX : (uint8_t)(state->discards_left + 3);
    } else if (joker->center_id == BALATRO_CENTER_J_DRUNKARD) {
        if (state->discards_left < UINT8_MAX) state->discards_left++;
    } else if (joker->center_id == BALATRO_CENTER_J_TROUBADOUR) {
        state->hand_size = state->hand_size > UINT8_MAX - 2 ? UINT8_MAX : (uint8_t)(state->hand_size + 2);
    } else if (joker->center_id == BALATRO_CENTER_J_TURTLE_BEAN) {
        if (!mutable_joker->state[0]) mutable_joker->state[0] = 5;
        uint8_t bonus = (uint8_t)mutable_joker->state[0];
        state->hand_size = state->hand_size > UINT8_MAX - bonus ? UINT8_MAX : (uint8_t)(state->hand_size + bonus);
    }
    if (state->phase == BALATRO_PHASE_SELECTING_HAND && state->hand_size > old_hand_size) balatro_draw_to_hand(state);
    if (joker->center_id == BALATRO_CENTER_J_STUNTMAN) {
        state->hand_size = state->hand_size > 2 ? state->hand_size - 2 : 1;
        state->base_hand_size = state->base_hand_size > 2 ? state->base_hand_size - 2 : 1;
    }
    if (joker->center_id == BALATRO_CENTER_J_CHAOS) {
        if (state->free_rerolls < UINT8_MAX) state->free_rerolls++;
        state->reroll_cost = 0;
    }
    balatro_refresh_joker_cache(state);
}

void balatro_joker_removed(BalatroState *state, const BalatroCard *joker) {
    uint8_t matching = 0;
    for (uint8_t i = 0; i < state->joker_count; ++i)
        if (state->jokers[i].center_id == joker->center_id) matching++;
    /* Card:remove clears G.GAME.used_jokers once no live copy of this Joker
       remains.  Removal hooks run before the native array is compacted, so
       matching==1 means the card being removed is the final copy. */
    if (matching <= 1) balatro_unmark_center_used(state, joker->center_id);
    if (joker->edition == 4 && state->joker_slots > 0) state->joker_slots--;
    if (joker->center_id == BALATRO_CENTER_J_TO_THE_MOON && state->interest_amount > 0) state->interest_amount--;
    if (joker->center_id == BALATRO_CENTER_J_STUNTMAN) {
        state->hand_size += 2;
        state->base_hand_size += 2;
    }
    if (state->phase == BALATRO_PHASE_SELECTING_HAND) {
        if (joker->center_id == BALATRO_CENTER_J_JUGGLER) {
            if (state->hand_size > 1) state->hand_size--;
        } else if (joker->center_id == BALATRO_CENTER_J_MERRY_ANDY) {
            if (state->hand_size < UINT8_MAX) state->hand_size++;
            state->discards_left = state->discards_left >= 3 ? (uint8_t)(state->discards_left - 3) : 0;
        } else if (joker->center_id == BALATRO_CENTER_J_DRUNKARD) {
            if (state->discards_left) state->discards_left--;
        } else if (joker->center_id == BALATRO_CENTER_J_TROUBADOUR) {
            state->hand_size = state->hand_size > 2 ? state->hand_size - 2 : 1;
        } else if (joker->center_id == BALATRO_CENTER_J_TURTLE_BEAN) {
            uint8_t bonus = joker->state[0] > 0 ? (uint8_t)joker->state[0] : 5;
            state->hand_size = state->hand_size > bonus ? (uint8_t)(state->hand_size - bonus) : 1;
        }
    }
    if (joker->center_id == BALATRO_CENTER_J_CHAOS) {
        if (state->free_rerolls) state->free_rerolls--;
        state->reroll_cost = state->free_rerolls ? 0 : (state->tag_d_six_active ? 0 : state->reroll_base) + state->reroll_increase;
    }
}

void balatro_consumable_added(BalatroState *state, const BalatroCard *card) {
    balatro_mark_center_used(state, card->center_id);
    /* Negative consumables expand the consumable area like Negative Jokers
       expand the Joker area. */
    if (card->edition == 4 && state->consumable_slots < UINT8_MAX) state->consumable_slots++;
}

void balatro_consumable_removed(BalatroState *state, const BalatroCard *card) {
    if (card->edition == 4 && state->consumable_slots > 0) state->consumable_slots--;
    uint8_t matching = 0;
    for (uint8_t i = 0; i < state->consumable_count; ++i)
        if (state->consumables[i].center_id == card->center_id) matching++;
    if (matching <= 1) balatro_unmark_center_used(state, card->center_id);
}

void balatro_playing_card_added(BalatroState *state, uint8_t count) {
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_HOLOGRAM)
            state->jokers[j].state[0] = (state->jokers[j].state[0] > 100 ? state->jokers[j].state[0] : 100) + count * 25;
}

void reset_round_targets(BalatroState *state);

int balatro_debug_add_center(BalatroState *state, uint16_t center_id, uint8_t edition, uint8_t flags) {
    if (!state || !state->config.validation || center_id == 0 || center_id >= BALATRO_CENTER_COUNT) return BALATRO_ERR_ARGUMENT;
    uint8_t set = balatro_centers[center_id].set;
    if (set != 3 && set != SET_TAROT && set != SET_PLANET && set != SET_SPECTRAL) return BALATRO_ERR_ARGUMENT;
    BalatroCard card = {0};
    card.center_id = center_id;
    card.sort_id = ++state->next_sort_id;
    card.flags = flags;
    if (set == 3) balatro_initialize_joker_card(state, &card);
    if (edition == BALATRO_EDITION_POLL && set == 3) {
        char stream[32];
        BalatroKeyBuilder key;
        balatro_key_begin(&key, stream, sizeof(stream));
        balatro_key_append(&key, "edi");
        balatro_key_append_u64(&key, state->ante);
        double roll = balatro_pseudorandom(state, stream);
        if (roll > 0.997)
            card.edition = BALATRO_EDITION_NEGATIVE;
        else if (roll > 1.0 - 0.006 * state->edition_rate)
            card.edition = BALATRO_EDITION_POLYCHROME;
        else if (roll > 1.0 - 0.02 * state->edition_rate)
            card.edition = BALATRO_EDITION_HOLO;
        else if (roll > 1.0 - 0.04 * state->edition_rate)
            card.edition = BALATRO_EDITION_FOIL;
    } else if (edition != BALATRO_EDITION_POLL) {
        if (edition > BALATRO_EDITION_NEGATIVE) return BALATRO_ERR_ARGUMENT;
        card.edition = edition;
    }
    if (card.flags & BALATRO_CARD_PERISHABLE) card.state[3] = 5;
    balatro_price_card(state, &card);
    if (set == 3) {
        if (state->joker_count >= BALATRO_MAX_JOKERS) return BALATRO_ERR_CAPACITY;
        state->jokers[state->joker_count++] = card;
        balatro_joker_added(state, &state->jokers[state->joker_count - 1]);
        balatro_mark_center_used(state, center_id);
    } else {
        if (state->consumable_count >= BALATRO_MAX_CONSUMABLES) return BALATRO_ERR_CAPACITY;
        state->consumables[state->consumable_count++] = card;
        balatro_consumable_added(state, &state->consumables[state->consumable_count - 1]);
    }
    return BALATRO_OK;
}

int balatro_debug_add_playing_card(BalatroState *state, uint8_t suit, uint8_t rank, uint8_t enhancement, uint8_t edition, uint8_t seal) {
    if (!state || !state->config.validation || suit > BALATRO_SPADES || rank < 2 || rank > 14 || enhancement > BALATRO_ENHANCEMENT_LUCKY ||
        edition > BALATRO_EDITION_NEGATIVE || seal > BALATRO_SEAL_PURPLE)
        return BALATRO_ERR_ARGUMENT;
    BalatroCard card = {
        .center_id = BALATRO_CENTER_C_BASE,
        .sort_id = ++state->next_sort_id,
        .suit = suit,
        .rank = rank,
        .enhancement = enhancement,
        .edition = edition,
        .seal = seal,
        .cost = 1,
        .sell_cost = 1,
    };
    if (!balatro_can_add_playing_cards(state, 1)) return BALATRO_ERR_CAPACITY;
    if (state->phase == BALATRO_PHASE_SELECTING_HAND) {
        if (state->hand_count >= BALATRO_MAX_HAND) return BALATRO_ERR_CAPACITY;
        state->hand[state->hand_count++] = card;
    } else {
        if (state->deck_count >= BALATRO_MAX_DECK) return BALATRO_ERR_CAPACITY;
        state->deck[state->deck_count++] = card;
    }
    return BALATRO_OK;
}

int add_joker_rarity(BalatroState *state, uint8_t rarity, const char *append, int legendary) {
    if (state->joker_count >= state->joker_slots || state->joker_count >= BALATRO_MAX_JOKERS) return 0;
    size_t count = 0;
    const uint16_t *pool = balatro_joker_pool(rarity, &count);
    if (!pool || !count) return 0;
    uint16_t center_id = 0;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        char stream[64];
        BalatroKeyBuilder key;
        balatro_key_begin(&key, stream, sizeof(stream));
        balatro_key_append(&key, "Joker");
        balatro_key_append_u64(&key, rarity);
        if (!legendary) {
            balatro_key_append(&key, append);
            balatro_key_append_u64(&key, state->ante);
        }
        if (attempt) {
            balatro_key_append(&key, "_resample");
            balatro_key_append_u64(&key, attempt + 1);
        }
        size_t index = (size_t)floor(balatro_pseudorandom(state, stream) * count);
        uint16_t candidate = pool[index];
        int ring_master = joker_active(state, BALATRO_CENTER_J_RING_MASTER);
        if (balatro_centers[candidate].base_available && (!balatro_center_used(state, candidate) || ring_master)) {
            center_id = candidate;
            break;
        }
    }
    if (!center_id) return 0;
    BalatroCard card = {0};
    card.center_id = center_id;
    card.sort_id = ++state->next_sort_id;
    balatro_initialize_joker_card(state, &card);
    char edition_stream[32];
    BalatroKeyBuilder edition_key;
    balatro_key_begin(&edition_key, edition_stream, sizeof(edition_stream));
    balatro_key_append(&edition_key, "edi");
    balatro_key_append(&edition_key, append);
    balatro_key_append_u64(&edition_key, state->ante);
    double edition = balatro_pseudorandom(state, edition_stream);
    if (edition > 0.997)
        card.edition = BALATRO_EDITION_NEGATIVE;
    else if (edition > 1.0 - 0.006 * state->edition_rate)
        card.edition = BALATRO_EDITION_POLYCHROME;
    else if (edition > 1.0 - 0.02 * state->edition_rate)
        card.edition = BALATRO_EDITION_HOLO;
    else if (edition > 1.0 - 0.04 * state->edition_rate)
        card.edition = BALATRO_EDITION_FOIL;
    balatro_price_card(state, &card);
    state->jokers[state->joker_count++] = card;
    balatro_joker_added(state, &state->jokers[state->joker_count - 1]);
    balatro_mark_center_used(state, card.center_id);
    return 1;
}

void notify_removed_playing_cards(BalatroState *state, const BalatroCard *cards, uint8_t count, int shattered) {
    uint8_t faces = 0, glass = 0;
    for (uint8_t i = 0; i < count; ++i) {
        if (cards[i].rank >= 11 && cards[i].rank <= 13 && !(cards[i].flags & 1u)) faces++;
        /* Glass Joker's remove_playing_cards hook only counts cards marked
           shattered. Hanged Man has a separate using_consumeable hook and
           ordinary destruction such as Immolate must not double-count it. */
        if (shattered && cards[i].enhancement == 4 && !(cards[i].flags & 1u)) glass++;
    }
    if (!faces && !glass) return;
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        BalatroCard *joker = &state->jokers[j];
        if (joker->flags & 1u) continue;
        if (joker->center_id == BALATRO_CENTER_J_CAINO && faces)
            joker->state[0] = (joker->state[0] > 100 ? joker->state[0] : 100) + faces * 100;
        else if (joker->center_id == BALATRO_CENTER_J_GLASS && glass)
            joker->state[0] = (joker->state[0] > 100 ? joker->state[0] : 100) + glass * 75;
    }
}

void remove_hand_index(BalatroState *state, uint8_t index) {
    notify_removed_playing_cards(state, &state->hand[index], 1, 0);
    memmove(&state->hand[index], &state->hand[index + 1], (state->hand_count - index - 1) * sizeof(BalatroCard));
    state->hand_count--;
}

BalatroCard random_playing_card(BalatroState *state, const char *stream) {
    BalatroCard card = {0};
    size_t front = (size_t)floor(balatro_pseudorandom(state, stream) * 52.0);
    card.center_id = BALATRO_CENTER_C_BASE;
    card.sort_id = ++state->next_sort_id;
    BalatroPlayingCardDefinition definition = balatro_playing_card((uint8_t)front);
    card.suit = definition.suit;
    card.rank = definition.rank;
    card.cost = card.sell_cost = 1;
    return card;
}

/* Select from a CardArea using pseudorandom_element's sort_id ordering. */
uint8_t random_sorted_hand_index(BalatroState *state, const char *stream) {
    uint8_t order[BALATRO_MAX_HAND];
    for (uint8_t i = 0; i < state->hand_count; ++i) order[i] = i;
    for (uint8_t i = 1; i < state->hand_count; ++i) {
        uint8_t value = order[i], j = i;
        while (j && state->hand[order[j - 1]].sort_id > state->hand[value].sort_id) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = value;
    }
    if (!state->hand_count) return 0;
    size_t index = (size_t)floor(balatro_pseudorandom(state, stream) * state->hand_count);
    return order[index];
}

static BalatroCard spectral_playing_card(BalatroState *state, const uint8_t *ranks, size_t rank_count, const char *create_stream) {
    static const uint8_t suits[] = {
        BALATRO_SPADES,
        BALATRO_HEARTS,
        BALATRO_DIAMONDS,
        BALATRO_CLUBS,
    }; /* Suit-code insertion order: S, H, D, C. */
    static const uint8_t enhancements[] = {1, 2, 3, 4, 5, 7, 8};
    BalatroCard card = {0};
    /* Grim fixes rank='A' and does not call pseudorandom_element for rank;
       Familiar and Incantation roll both rank and suit. */
    size_t rank = rank_count > 1 ? (size_t)floor(balatro_pseudorandom(state, create_stream) * rank_count) : 0;
    size_t suit = (size_t)floor(balatro_pseudorandom(state, create_stream) * 4.0);
    size_t enhancement = (size_t)floor(balatro_pseudorandom(state, "spe_card") * (sizeof(enhancements) / sizeof(enhancements[0])));
    if (rank >= rank_count) rank = rank_count - 1;
    if (suit >= sizeof(suits)) suit = sizeof(suits) - 1;
    if (enhancement >= sizeof(enhancements)) enhancement = sizeof(enhancements) - 1;
    card.center_id = BALATRO_CENTER_C_BASE;
    card.sort_id = ++state->next_sort_id;
    card.rank = ranks[rank];
    card.suit = suits[suit];
    card.enhancement = enhancements[enhancement];
    card.cost = card.sell_cost = 1;
    return card;
}

typedef struct TargetCard {
    uint16_t sort_id;
    uint8_t rank;
    uint8_t suit;
} TargetCard;

static uint16_t collect_nonstone_cards(const BalatroState *state, TargetCard *candidates) {
    uint16_t count = 0;
    for (uint16_t i = 0; i < state->deck_count; ++i)
        if (state->deck[i].enhancement != BALATRO_ENHANCEMENT_STONE)
            candidates[count++] = (TargetCard){state->deck[i].sort_id, state->deck[i].rank, state->deck[i].suit};
    for (uint8_t i = 0; i < state->hand_count; ++i)
        if (state->hand[i].enhancement != BALATRO_ENHANCEMENT_STONE)
            candidates[count++] = (TargetCard){state->hand[i].sort_id, state->hand[i].rank, state->hand[i].suit};
    for (uint16_t i = 0; i < state->discard_count; ++i)
        if (state->discard[i].enhancement != BALATRO_ENHANCEMENT_STONE)
            candidates[count++] = (TargetCard){state->discard[i].sort_id, state->discard[i].rank, state->discard[i].suit};
    return count;
}

static void sort_target_cards(TargetCard *candidates, uint16_t count) {
    for (uint16_t i = 1; i < count; ++i) {
        TargetCard value = candidates[i];
        uint16_t j = i;
        while (j && candidates[j - 1].sort_id > value.sort_id) {
            candidates[j] = candidates[j - 1];
            --j;
        }
        candidates[j] = value;
    }
}

static size_t pick_target_index(BalatroState *state, uint16_t count, const char *prefix) {
    char stream[32];
    BalatroKeyBuilder key;
    balatro_key_begin(&key, stream, sizeof(stream));
    balatro_key_append(&key, prefix);
    balatro_key_append_u64(&key, state->ante);
    size_t pick = (size_t)floor(balatro_pseudorandom(state, stream) * count);
    return pick < count ? pick : (size_t)(count - 1);
}

void reset_round_targets(BalatroState *state) {
    TargetCard candidates[BALATRO_MAX_DECK * 2 + BALATRO_MAX_HAND];
    uint16_t count = collect_nonstone_cards(state, candidates);
    sort_target_cards(candidates, count);
    uint8_t idol_rank = 14, idol_suit = BALATRO_SPADES;
    if (count) {
        size_t pick = pick_target_index(state, count, "idol");
        idol_rank = candidates[pick].rank;
        idol_suit = candidates[pick].suit;
    }
    uint8_t mail_rank = 14, castle_suit = BALATRO_SPADES;
    if (count) {
        size_t mail = pick_target_index(state, count, "mail");
        size_t castle = pick_target_index(state, count, "cas");
        mail_rank = candidates[mail].rank;
        castle_suit = candidates[castle].suit;
    }
    state->idol_rank = idol_rank;
    state->idol_suit = idol_suit;
    state->mail_rank = mail_rank;
    state->castle_suit = castle_suit;
    for (uint8_t i = 0; i < state->joker_count; ++i) {
        if (state->jokers[i].flags & 1u) continue;
        if (state->jokers[i].center_id == BALATRO_CENTER_J_IDOL) {
            state->jokers[i].state[0] = idol_rank;
            state->jokers[i].state[1] = idol_suit;
        } else if (state->jokers[i].center_id == BALATRO_CENTER_J_MAIL)
            state->jokers[i].state[1] = mail_rank;
        else if (state->jokers[i].center_id == BALATRO_CENTER_J_CASTLE)
            state->jokers[i].state[1] = castle_suit;
    }
    uint8_t suits[4] = {BALATRO_SPADES, BALATRO_HEARTS, BALATRO_CLUBS, BALATRO_DIAMONDS};
    uint8_t choices[4], choice_count = 0;
    for (uint8_t i = 0; i < 4; ++i)
        if (suits[i] != state->ancient_suit) choices[choice_count++] = suits[i];
    size_t pick = pick_target_index(state, choice_count, "anc");
    state->ancient_suit = choices[pick];
}

void add_spectral_cards(BalatroState *state, const uint8_t *ranks, size_t rank_count, uint8_t count, const char *stream) {
    while (count--) {
        if (!balatro_can_add_playing_cards(state, 1)) break;
        BalatroCard card = spectral_playing_card(state, ranks, rank_count, stream);
        /* Balatro creates Familiar/Grim/Incantation cards directly in G.hand
           during a live round; they are not silently inserted into the deck. */
        if ((state->phase == BALATRO_PHASE_SELECTING_HAND || state->phase == BALATRO_PHASE_PACK_OPENING) &&
            state->hand_count < BALATRO_MAX_HAND)
            state->hand[state->hand_count++] = card;
        else if (state->deck_count < BALATRO_MAX_DECK)
            state->deck[state->deck_count++] = card;
        else
            continue;
        balatro_playing_card_added(state, 1);
    }
    /* Pack overlays append creations to the temporary hand. finish_pack
       reverses that hand onto the deck, placing creations before survivors. */
    if (state->phase == BALATRO_PHASE_SELECTING_HAND) sort_hand_desc(state);
}

static void make_red_deck(BalatroState *state) {
    state->deck_count = 0;
    /* The playing-card pool is sorted lexicographically by key:
       Clubs, Diamonds, Hearts, Spades.  Preserve that order before the
       deterministic shuffle so the same seed produces the same deck. */
    static const uint8_t suits[] = {BALATRO_CLUBS, BALATRO_DIAMONDS, BALATRO_HEARTS, BALATRO_SPADES};
    static const uint8_t ranks[] = {2, 3, 4, 5, 6, 7, 8, 9, 14, 11, 13, 12, 10};
    for (size_t suit_index = 0; suit_index < sizeof(suits); ++suit_index) {
        uint8_t suit = suits[suit_index];
        for (size_t rank_index = 0; rank_index < sizeof(ranks); ++rank_index) {
            uint8_t rank = ranks[rank_index];
            if (state->config.deck == BALATRO_CENTER_B_ABANDONED && (rank == 11 || rank == 12 || rank == 13)) continue;
            BalatroCard card = {0};
            card.center_id = BALATRO_CENTER_C_BASE;
            card.sort_id = ++state->next_sort_id;
            card.suit = suit;
            card.rank = rank;
            if (state->config.deck == BALATRO_CENTER_B_CHECKERED) {
                if (card.suit == BALATRO_CLUBS)
                    card.suit = BALATRO_SPADES;
                else if (card.suit == BALATRO_DIAMONDS)
                    card.suit = BALATRO_HEARTS;
            }
            if (state->config.deck == BALATRO_CENTER_B_ERRATIC) {
                size_t front = (size_t)floor(balatro_pseudorandom(state, "erratic") * 52.0);
                BalatroPlayingCardDefinition definition = balatro_playing_card((uint8_t)front);
                card.suit = definition.suit;
                card.rank = definition.rank;
            }
            state->deck[state->deck_count++] = card;
        }
    }
}

void draw_after_play(BalatroState *state) {
    uint8_t target = state->hand_size;
    if (!state->blind_disabled && state->blind_id == BALATRO_BLIND_BL_SERPENT) {
        target = state->hand_count + 3;
    }
    while (state->hand_count < target && state->deck_count > 0 && state->hand_count < BALATRO_MAX_HAND)
        state->hand[state->hand_count++] = state->deck[--state->deck_count];
    sort_hand_desc(state);
}

void apply_discard_effects(BalatroState *state, const BalatroCard *cards, uint8_t count, int first_discard, int hook) {
    /* discard_cards_from_highlighted dispatches the same seal/Joker contexts
       for The Hook's forced discard. `hook` only suppresses Burnt Joker and
       the actual discard counter/cost mutation. */
    if (!hook && first_discard) {
        for (uint8_t j = 0; j < state->joker_count; ++j)
            if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_BURNT) {
                uint8_t mask = 0;
                BalatroHandType hand = balatro_classify_hand(cards, count, &mask);
                if (state->hand_levels[hand] < 255) state->hand_levels[hand]++;
            }
    }
    int pareidolia = joker_active(state, BALATRO_CENTER_J_PAREIDOLIA);
    int face_count = 0;
    for (uint8_t k = 0; k < count; ++k)
        if (!(cards[k].flags & 1u) && (pareidolia || (cards[k].rank >= 11 && cards[k].rank <= 13))) face_count++;
    BalatroCard removed[BALATRO_MAX_SELECTION] = {0};
    uint8_t removed_count = 0;
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        BalatroCard *joker = &state->jokers[j];
        if (joker->flags & 1u) continue;
        if (joker->center_id == BALATRO_CENTER_J_FACELESS && face_count >= 3) {
            state->dollars += 5;
        } else if (joker->center_id == BALATRO_CENTER_J_MAIL) {
            for (uint8_t k = 0; k < count; ++k)
                if (!(cards[k].flags & 1u) && cards[k].rank == (uint8_t)joker->state[1]) state->dollars += 5;
        } else if (joker->center_id == BALATRO_CENTER_J_TRADING && first_discard && count == 1) {
            state->dollars += 3;
            int discard_index = find_discard_card(state, cards[0].sort_id);
            if (discard_index >= 0) {
                removed[removed_count++] = cards[0];
                memmove(&state->discard[discard_index], &state->discard[discard_index + 1],
                        (state->discard_count - (uint16_t)discard_index - 1) * sizeof(BalatroCard));
                state->discard_count--;
            }
        } else if (joker->center_id == BALATRO_CENTER_J_CASTLE) {
            for (uint8_t k = 0; k < count; ++k)
                if (!(cards[k].flags & 1u) && (cards[k].enhancement == 3 || cards[k].suit == (uint8_t)joker->state[1]))
                    joker->state[0] += 3;
        }
    }
    for (uint8_t k = 0; k < count; ++k)
        if (!(cards[k].flags & 1u) && cards[k].seal == 4 && state->consumable_count < state->consumable_slots)
            (void)add_pooled_consumable(state, SET_TAROT, "8ba", 0);
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        BalatroCard *joker = &state->jokers[j];
        if (joker->flags & 1u) continue;
        if (joker->center_id == BALATRO_CENTER_J_GREEN_JOKER && joker->state[0] > 0) joker->state[0]--;
        if (joker->center_id == BALATRO_CENTER_J_HIT_THE_ROAD) {
            int jacks = 0;
            for (uint8_t k = 0; k < count; ++k)
                if (!(cards[k].flags & 1u) && cards[k].rank == 11) jacks++;
            joker->state[0] += jacks * 50;
        }
        if (joker->center_id == BALATRO_CENTER_J_RAMEN) {
            int x = joker->state[0] > 100 ? joker->state[0] : 200;
            x -= count;
            if (x <= 100)
                joker->state[1] = 1;
            else
                joker->state[0] = x;
        }
        if (joker->center_id == BALATRO_CENTER_J_YORICK) {
            int remaining = joker->state[1] > 0 ? joker->state[1] : 23;
            for (uint8_t k = 0; k < count; ++k) {
                if (--remaining <= 0) {
                    remaining = 23;
                    joker->state[0] = (joker->state[0] > 100 ? joker->state[0] : 100) + 100;
                }
            }
            joker->state[1] = remaining;
        }
    }
    for (uint8_t j = state->joker_count; j-- > 0;)
        if (state->jokers[j].center_id == BALATRO_CENTER_J_RAMEN && state->jokers[j].state[1]) remove_joker_at(state, j);
    if (removed_count) notify_removed_playing_cards(state, removed, removed_count, 0);
}

void apply_hook_discard(BalatroState *state) {
    if (state->blind_disabled || state->blind_id != BALATRO_BLIND_BL_HOOK) return;
    BalatroCard cards[2];
    uint8_t count = 0;
    for (uint8_t draw = 0; draw < 2 && state->hand_count; ++draw) {
        /* pseudorandom_element sorts Card objects by creation sort_id.  The
           hand array is visual rank order, so indexing it directly selects a
           different card even though the named-stream value is identical. */
        uint8_t index = random_sorted_hand_index(state, "hook");
        cards[count++] = state->hand[index];
        if (state->discard_count < BALATRO_MAX_DECK) state->discard[state->discard_count++] = state->hand[index];
        memmove(&state->hand[index], &state->hand[index + 1], (state->hand_count - index - 1) * sizeof(BalatroCard));
        state->hand_count--;
    }
    if (count) apply_discard_effects(state, cards, count, state->discards_used == 0, 1);
}

int find_discard_card(const BalatroState *state, uint16_t sort_id) {
    for (uint16_t i = 0; i < state->discard_count; ++i)
        if (state->discard[i].sort_id == sort_id) return (int)i;
    return -1;
}

void apply_drawn_to_hand_boss(BalatroState *state, int crimson_prepped) {
    if (state->blind_disabled) return;
    if (state->blind_id == BALATRO_BLIND_BL_FINAL_BELL && state->hand_count) {
        for (uint8_t i = 0; i < state->hand_count; ++i)
            if (state->hand[i].flags & (1u << 4)) return;
        uint8_t order[BALATRO_MAX_HAND];
        for (uint8_t i = 0; i < state->hand_count; ++i) order[i] = i;
        for (uint8_t i = 1; i < state->hand_count; ++i) {
            uint8_t value = order[i], j = i;
            while (j && state->hand[order[j - 1]].sort_id > state->hand[value].sort_id) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = value;
        }
        size_t sorted_index = (size_t)floor(balatro_pseudorandom(state, "cerulean_bell") * state->hand_count);
        state->hand[order[sorted_index]].flags |= (uint8_t)(1u << 4);
    } else if (crimson_prepped && state->blind_id == BALATRO_BLIND_BL_FINAL_HEART && state->joker_count) {
        uint8_t eligible[BALATRO_MAX_JOKERS], count = 0;
        /* The previously debuffed Joker is excluded when at least two cards
           exist, then all debuffs are cleared before the next target is set. */
        for (uint8_t i = 0; i < state->joker_count; ++i) {
            if (!(state->jokers[i].flags & 1u) || state->joker_count < 2) eligible[count++] = i;
            state->jokers[i].flags &= (uint8_t)~1u;
        }
        for (uint8_t i = 1; i < count; ++i) {
            uint8_t value = eligible[i], j = i;
            while (j && state->jokers[eligible[j - 1]].sort_id > state->jokers[value].sort_id) {
                eligible[j] = eligible[j - 1];
                --j;
            }
            eligible[j] = value;
        }
        if (count) {
            size_t sorted_index = (size_t)floor(balatro_pseudorandom(state, "crimson_heart") * count);
            state->jokers[eligible[sorted_index]].flags |= 1u;
        }
    }
}

double balatro_probability_normal(const BalatroState *state, double base) {
    for (uint8_t i = 0; i < state->joker_count; ++i)
        if (!(state->jokers[i].flags & 1u) && state->jokers[i].center_id == BALATRO_CENTER_J_OOPS) base *= 2.0;
    return base;
}

void balatro_draw_to_hand(BalatroState *state) {
    while (state->hand_count < state->hand_size && state->deck_count > 0 && state->hand_count < BALATRO_MAX_HAND)
        state->hand[state->hand_count++] = state->deck[--state->deck_count];
    sort_hand_desc(state);
}

static int card_nominal(const BalatroCard *card, int by_suit) {
    int suit_value = card->suit == BALATRO_SPADES ? 4 : card->suit == BALATRO_HEARTS ? 3 : card->suit == BALATRO_CLUBS ? 2 : 1;
    int nominal = card->rank == 14 ? 11 : card->rank >= 11 ? 10 : card->rank;
    int face = card->rank >= 11 ? (card->rank == 14 ? 4 : card->rank - 10) : 0;
    int base = nominal * 100 + face * 10;
    /* Card:get_nominal('suit') multiplies suit_nominal by 1000. Stone
       overrides that multiplier with -1000 for both sort modes. */
    if (card->enhancement == BALATRO_ENHANCEMENT_STONE) return base - suit_value * 10000;
    return by_suit ? suit_value * 10000 + base : base * 10 + suit_value;
}

void sort_hand_mode(BalatroState *state, int by_suit) {
    for (uint8_t i = 1; i < state->hand_count; ++i) {
        BalatroCard card = state->hand[i];
        int key = card_nominal(&card, by_suit);
        uint8_t j = i;
        while (j > 0) {
            const BalatroCard *prev = &state->hand[j - 1];
            int prev_key = card_nominal(prev, by_suit);
            if (prev_key >= key) break;
            state->hand[j] = state->hand[j - 1];
            --j;
        }
        state->hand[j] = card;
    }
}

void sort_hand_desc(BalatroState *state) {
    sort_hand_mode(state, state->hand_sort_suit != 0);
}

uint16_t choose_boss(BalatroState *state) {
    if (state->config.win_ante && state->ante >= 2 && state->ante % state->config.win_ante == 0) {
        static const uint16_t final_ids[] = {
            BALATRO_BLIND_BL_FINAL_ACORN, BALATRO_BLIND_BL_FINAL_BELL,   BALATRO_BLIND_BL_FINAL_HEART,
            BALATRO_BLIND_BL_FINAL_LEAF,  BALATRO_BLIND_BL_FINAL_VESSEL,
        };
        uint8_t min_use = UINT8_MAX;
        for (size_t i = 0; i < sizeof(final_ids) / sizeof(final_ids[0]); ++i)
            if (state->boss_usage[final_ids[i]] < min_use) min_use = state->boss_usage[final_ids[i]];
        uint16_t eligible[sizeof(final_ids) / sizeof(final_ids[0])];
        size_t count = 0;
        for (size_t i = 0; i < sizeof(final_ids) / sizeof(final_ids[0]); ++i)
            if (state->boss_usage[final_ids[i]] == min_use) eligible[count++] = final_ids[i];
        size_t index = (size_t)floor(balatro_pseudorandom(state, "boss") * count);
        uint16_t chosen = eligible[index < count ? index : count - 1];
        if (state->boss_usage[chosen] < UINT8_MAX) state->boss_usage[chosen]++;
        return chosen;
    }
    static const uint16_t ids[] = {
        BALATRO_BLIND_BL_ARM,   BALATRO_BLIND_BL_CLUB,    BALATRO_BLIND_BL_EYE,     BALATRO_BLIND_BL_FISH,  BALATRO_BLIND_BL_FLINT,
        BALATRO_BLIND_BL_GOAD,  BALATRO_BLIND_BL_HEAD,    BALATRO_BLIND_BL_HOOK,    BALATRO_BLIND_BL_HOUSE, BALATRO_BLIND_BL_MANACLE,
        BALATRO_BLIND_BL_MARK,  BALATRO_BLIND_BL_MOUTH,   BALATRO_BLIND_BL_NEEDLE,  BALATRO_BLIND_BL_OX,    BALATRO_BLIND_BL_PILLAR,
        BALATRO_BLIND_BL_PLANT, BALATRO_BLIND_BL_PSYCHIC, BALATRO_BLIND_BL_SERPENT, BALATRO_BLIND_BL_TOOTH, BALATRO_BLIND_BL_WALL,
        BALATRO_BLIND_BL_WATER, BALATRO_BLIND_BL_WHEEL,   BALATRO_BLIND_BL_WINDOW,
    };
    static const uint8_t minimum[] = {2, 1, 3, 2, 2, 1, 1, 1, 2, 1, 2, 2, 2, 6, 1, 4, 1, 5, 3, 2, 2, 2, 1};
    uint16_t eligible[sizeof(ids) / sizeof(ids[0])];
    size_t count = 0;
    uint8_t min_use = UINT8_MAX;
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
        if (state->ante >= minimum[i] && state->boss_usage[ids[i]] <= min_use) {
            if (state->boss_usage[ids[i]] < min_use) count = 0, min_use = state->boss_usage[ids[i]];
            eligible[count++] = ids[i];
        }
    if (!count) return BALATRO_BLIND_BL_ARM;
    size_t index = (size_t)floor(balatro_pseudorandom(state, "boss") * count);
    uint16_t chosen = eligible[index < count ? index : count - 1];
    if (state->boss_usage[chosen] < UINT8_MAX) state->boss_usage[chosen]++;
    return chosen;
}

void balatro_clear_card_debuffs(BalatroState *state) {
    for (uint16_t i = 0; i < state->deck_count; ++i) state->deck[i].flags &= (uint8_t)~BALATRO_CARD_DEBUFFED;
    for (uint8_t i = 0; i < state->hand_count; ++i) state->hand[i].flags &= (uint8_t)~BALATRO_CARD_DEBUFFED;
    for (uint16_t i = 0; i < state->discard_count; ++i) state->discard[i].flags &= (uint8_t)~BALATRO_CARD_DEBUFFED;
    for (uint8_t i = 0; i < state->joker_count; ++i) state->jokers[i].flags &= (uint8_t)~BALATRO_CARD_DEBUFFED;
    balatro_refresh_joker_cache(state);
}

int blind_debuffs_card(const BalatroState *state, const BalatroCard *card) {
    if (state->blind_disabled) return 0;
    int stone = card->enhancement == BALATRO_ENHANCEMENT_STONE;
    int wild = card->enhancement == BALATRO_ENHANCEMENT_WILD;
    switch (state->blind_id) {
    /* Blind:debuff_card calls Card:is_suit(..., true): Stone is suitless,
       while an otherwise non-debuffed Wild Card matches every suit. */
    case BALATRO_BLIND_BL_CLUB:
        return !stone && (wild || card->suit == BALATRO_CLUBS);
    case BALATRO_BLIND_BL_GOAD:
        return !stone && (wild || card->suit == BALATRO_SPADES);
    case BALATRO_BLIND_BL_HEAD:
        return !stone && (wild || card->suit == BALATRO_HEARTS);
    case BALATRO_BLIND_BL_WINDOW:
        return !stone && (wild || card->suit == BALATRO_DIAMONDS);
    case BALATRO_BLIND_BL_PLANT:
        return card->rank >= 11 && card->rank <= 13;
    case BALATRO_BLIND_BL_PILLAR:
        return card->state[3] != 0;
    case BALATRO_BLIND_BL_FINAL_LEAF:
        return 1;
    default:
        return 0;
    }
}

void refresh_card_debuff(const BalatroState *state, BalatroCard *card) {
    card->flags &= (uint8_t)~1u;
    if (blind_debuffs_card(state, card)) card->flags |= 1u;
}

/* Cashing out a Boss updates the most-played poker hand. Hand order is the
   tie-break: a lower order
   (Flush Five ... High Card) wins ties, while an all-zero run keeps High
   Card as the initialized default. */
uint8_t balatro_most_played_hand(const BalatroState *state) {
    uint8_t best = BALATRO_HIGH_CARD;
    uint16_t best_count = 0;
    for (uint8_t hand = 0; hand < BALATRO_HAND_COUNT; ++hand) {
        uint16_t count = state->hand_plays[hand];
        if (count > best_count || (count && count == best_count && hand < best)) {
            best = hand;
            best_count = count;
        }
    }
    return best;
}

void apply_card_debuffs(BalatroState *state) {
    for (uint16_t i = 0; i < state->deck_count; ++i)
        if (blind_debuffs_card(state, &state->deck[i])) state->deck[i].flags |= 1u;
    for (uint8_t i = 0; i < state->hand_count; ++i)
        if (blind_debuffs_card(state, &state->hand[i])) state->hand[i].flags |= 1u;
}

void swap_jokers(BalatroState *state, uint8_t left, uint8_t right) {
    BalatroCard card = state->jokers[left];
    state->jokers[left] = state->jokers[right];
    state->jokers[right] = card;
}


void balatro_default_config(BalatroConfig *config) {
    if (!config) return;
    *config = (BalatroConfig){.deck = 0, .stake = 1, .win_ante = 8};
    balatro_default_observation_profile(&config->observation);
}

void balatro_default_observation_profile(BalatroObservationProfile *profile) {
    if (!profile) return;
    *profile = (BalatroObservationProfile){
        .playing_cards = BALATRO_OBS_MAX_PLAYING_CARDS,
        .playing_variants = BALATRO_OBS_MAX_PLAYING_VARIANTS,
        .hand = BALATRO_OBS_MAX_HAND,
        .jokers = BALATRO_OBS_MAX_JOKERS,
        .consumables = BALATRO_OBS_MAX_CONSUMABLES,
        .tags = BALATRO_OBS_MAX_TAGS,
        .shop_vouchers = BALATRO_OBS_MAX_SHOP_VOUCHERS,
    };
}

static int observation_profile_valid(const BalatroObservationProfile *profile) {
    return profile && profile->playing_cards && profile->playing_cards <= BALATRO_OBS_MAX_PLAYING_CARDS && profile->playing_variants &&
           profile->playing_variants <= BALATRO_OBS_MAX_PLAYING_VARIANTS && profile->hand && profile->hand <= BALATRO_OBS_MAX_HAND &&
           profile->jokers && profile->jokers <= BALATRO_OBS_MAX_JOKERS && profile->consumables &&
           profile->consumables <= BALATRO_OBS_MAX_CONSUMABLES && profile->tags && profile->tags <= BALATRO_OBS_MAX_TAGS &&
           profile->shop_vouchers && profile->shop_vouchers <= BALATRO_OBS_MAX_SHOP_VOUCHERS;
}

int balatro_init(BalatroState *state, const BalatroConfig *config, uint64_t seed) {
    if (!state || !config) return BALATRO_ERR_ARGUMENT;
    if (!observation_profile_valid(&config->observation)) return BALATRO_ERR_ARGUMENT;
    memset(state, 0, sizeof(*state));
    state->config = *config;
    return balatro_reset(state, seed);
}

int balatro_init_seed_string(BalatroState *state, const BalatroConfig *config, const char *seed) {
    if (!state || !config || !seed || !seed[0]) return BALATRO_ERR_ARGUMENT;
    if (!observation_profile_valid(&config->observation)) return BALATRO_ERR_ARGUMENT;
    memset(state, 0, sizeof(*state));
    state->config = *config;
    return balatro_reset_seed_string(state, seed);
}

int balatro_reset(BalatroState *state, uint64_t seed) {
    if (!state) return BALATRO_ERR_ARGUMENT;
    char text[32];
    BalatroKeyBuilder key;
    balatro_key_begin(&key, text, sizeof(text));
    balatro_key_append_u64(&key, seed);
    state->numeric_seed = seed;
    return balatro_reset_seed_string(state, text);
}

int balatro_reset_seed_string(BalatroState *state, const char *seed) {
    if (!state || !seed || !seed[0] || strlen(seed) >= sizeof(state->seed)) return BALATRO_ERR_ARGUMENT;
    BalatroConfig config = state->config;
    uint64_t numeric_seed = state->numeric_seed;
    memset(state, 0, sizeof(*state));
    state->config = config;
    state->numeric_seed = numeric_seed;
    memcpy(state->seed, seed, strlen(seed) + 1);
    balatro_rng_reset(state);
    state->phase = BALATRO_PHASE_BLIND_SELECT;
    state->ante = 1;
    state->blind_on_deck = 0;
    state->dollars = 4;
    state->hand_size = 8;
    state->joker_slots = 5;
    state->consumable_slots = 2;
    state->reroll_base = 5;
    state->reroll_cost = 5;
    state->shop_joker_max = 2;
    state->hands_per_round = 4;
    state->discards_per_round = 3;
    state->joker_rate = 20.0f;
    state->tarot_rate = 4.0f;
    state->planet_rate = 4.0f;
    state->edition_rate = 1.0f;
    state->ancient_suit = UINT8_MAX;
    state->interest_cap = 25;
    state->interest_amount = 1;
    state->rental_rate = 3;
    state->stake_scaling = config.stake >= 6 ? 3 : config.stake >= 3 ? 2 : 1;
    switch (config.deck) {
    case 0:
    case BALATRO_CENTER_B_RED:
        state->discards_per_round++;
        break;
    case BALATRO_CENTER_B_BLUE:
        state->hands_per_round++;
        break;
    case BALATRO_CENTER_B_BLACK:
        if (state->hands_per_round > 1) state->hands_per_round--;
        state->joker_slots++;
        break;
    case BALATRO_CENTER_B_GREEN:
        state->discards_per_round++;
        break;
    case BALATRO_CENTER_B_YELLOW:
        state->dollars += 10;
        break;
    case BALATRO_CENTER_B_GHOST:
        state->spectral_rate = 2.0f;
        break;
    case BALATRO_CENTER_B_NEBULA:
        if (state->consumable_slots) state->consumable_slots--;
        break;
    case BALATRO_CENTER_B_PAINTED:
        state->hand_size += 2;
        if (state->joker_slots) state->joker_slots--;
        break;
    default:
        break;
    }
    /* Orange stake starts with one fewer discard. */
    if (config.stake >= 5 && state->discards_per_round) state->discards_per_round--;
    state->base_hand_size = state->hand_size;
    /* Run-start order is Boss, voucher, then two tags. */
    state->next_boss_id = choose_boss(state);
    state->next_voucher_id = balatro_pick_voucher(state);
    assign_blind_tags(state);
    choose_orbital_hands(state);
    for (size_t i = 0; i < BALATRO_HAND_COUNT; ++i) state->hand_levels[i] = 1;
    make_red_deck(state);
    balatro_shuffle(state, state->deck, state->deck_count, "shuffle");
    if (config.deck == BALATRO_CENTER_B_GHOST) {
        state->consumables[state->consumable_count++] = (BalatroCard){.center_id = BALATRO_CENTER_C_HEX, .cost = 4, .sell_cost = 2};
    } else if (config.deck == BALATRO_CENTER_B_MAGIC) {
        state->consumables[state->consumable_count++] = (BalatroCard){.center_id = BALATRO_CENTER_C_FOOL};
        state->consumables[state->consumable_count++] = (BalatroCard){.center_id = BALATRO_CENTER_C_FOOL};
        state->consumable_slots++;
        balatro_mark_center_used(state, BALATRO_CENTER_V_CRYSTAL_BALL);
    } else if (config.deck == BALATRO_CENTER_B_NEBULA) {
        balatro_mark_center_used(state, BALATRO_CENTER_V_TELESCOPE);
    } else if (config.deck == BALATRO_CENTER_B_ZODIAC) {
        state->shop_joker_max++;
        state->tarot_rate = 9.6f;
        state->planet_rate = 9.6f;
        balatro_mark_center_used(state, BALATRO_CENTER_V_TAROT_MERCHANT);
        balatro_mark_center_used(state, BALATRO_CENTER_V_PLANET_MERCHANT);
        balatro_mark_center_used(state, BALATRO_CENTER_V_OVERSTOCK_NORM);
    }
    return BALATRO_OK;
}
