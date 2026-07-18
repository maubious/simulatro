#include "internal.h"
#include "content.h"

#include <math.h>
#include <string.h>

static int state_layout_valid(const BalatroState *state) {
    return state && state->rng_count <= BALATRO_MAX_RNG_STREAMS && state->deck_count <= BALATRO_MAX_DECK &&
           state->hand_count <= BALATRO_MAX_HAND && state->discard_count <= BALATRO_MAX_DECK && state->joker_count <= BALATRO_MAX_JOKERS &&
           state->consumable_count <= BALATRO_MAX_CONSUMABLES && state->shop_count <= BALATRO_MAX_SHOP_CARDS &&
           state->pack_count <= BALATRO_MAX_PACK_CARDS && state->phase <= BALATRO_PHASE_GAME_OVER &&
           memchr(state->seed, 0, sizeof(state->seed));
}

static int joker_active(const BalatroState *state, uint16_t center_id) {
    for (uint8_t i = 0; i < state->joker_count; ++i)
        if (!(state->jokers[i].flags & 1u) && state->jokers[i].center_id == center_id) return 1;
    return 0;
}

static void reset_round_rerolls(BalatroState *state) {
    uint8_t chaos = 0;
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (!(state->jokers[j].flags & BALATRO_CARD_DEBUFFED) && state->jokers[j].center_id == BALATRO_CENTER_J_CHAOS && chaos < UINT8_MAX)
            chaos++;
    state->free_rerolls = chaos;
    state->reroll_increase = 0;
    state->reroll_cost = chaos ? 0 : (state->tag_d_six_active ? 0 : state->reroll_base);
}

static int8_t blind_reward_for(uint16_t blind_id, uint8_t blind_on_deck) {
    if (blind_id == BALATRO_BLIND_BL_FINAL_ACORN || blind_id == BALATRO_BLIND_BL_FINAL_BELL || blind_id == BALATRO_BLIND_BL_FINAL_HEART ||
        blind_id == BALATRO_BLIND_BL_FINAL_LEAF || blind_id == BALATRO_BLIND_BL_FINAL_VESSEL)
        return 8;
    return blind_on_deck == 0 ? 3 : blind_on_deck == 1 ? 4 : 5;
}

/* Forward declarations for skip-tag helpers defined before the general
   Joker/boss construction routines below. */
static int add_joker_rarity(BalatroState *state, uint8_t rarity, const char *append, int legendary);
static uint16_t choose_boss(BalatroState *state);
static void remove_joker_at(BalatroState *state, uint8_t index);
static void sort_hand_desc(BalatroState *state);
static void end_round_jokers(BalatroState *state);
static int find_discard_card(const BalatroState *state, uint16_t sort_id);
static int consumable_target_limit(uint16_t center_id);
static int consumable_target_minimum(uint16_t center_id);

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

static uint8_t choose_to_do_hand(BalatroState *state, int excluded) {
    uint8_t candidates[BALATRO_HAND_COUNT];
    size_t count = 0;
    for (size_t i = 0; i < sizeof(lua_hand_order); ++i)
        if (hand_visible(state, lua_hand_order[i]) && (int)lua_hand_order[i] != excluded) candidates[count++] = lua_hand_order[i];
    size_t index = (size_t)floor(balatro_pseudorandom(state, "to_do") * count);
    return candidates[index];
}

static void choose_orbital_hands(BalatroState *state) {
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

static void assign_blind_tags(BalatroState *state) {
    state->blind_tags[0] = choose_tag(state);
    state->blind_tags[1] = choose_tag(state);
}

static void apply_skip_tag(BalatroState *state, uint8_t tag) {
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

static int add_pooled_consumable(BalatroState *state, uint8_t set, const char *append, uint8_t edition) {
    BalatroCard card = balatro_create_pooled_card(state, set, append, 0);
    card.edition = edition;
    if (state->consumable_count >= BALATRO_MAX_CONSUMABLES || (state->consumable_count >= state->consumable_slots && card.edition != 4))
        return 0;
    state->consumables[state->consumable_count++] = card;
    balatro_consumable_added(state, &card);
    return 1;
}

static int add_specific_consumable(BalatroState *state, uint16_t center_id) {
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

static void reset_round_targets(BalatroState *state);

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
    if (state->phase == BALATRO_PHASE_SELECTING_HAND) {
        if (state->hand_count >= BALATRO_MAX_HAND) return BALATRO_ERR_CAPACITY;
        state->hand[state->hand_count++] = card;
    } else {
        if (state->deck_count >= BALATRO_MAX_DECK) return BALATRO_ERR_CAPACITY;
        state->deck[state->deck_count++] = card;
    }
    return BALATRO_OK;
}

static int add_joker_rarity(BalatroState *state, uint8_t rarity, const char *append, int legendary) {
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

static void notify_removed_playing_cards(BalatroState *state, const BalatroCard *cards, uint8_t count, int shattered) {
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

static void remove_hand_index(BalatroState *state, uint8_t index) {
    notify_removed_playing_cards(state, &state->hand[index], 1, 0);
    memmove(&state->hand[index], &state->hand[index + 1], (state->hand_count - index - 1) * sizeof(BalatroCard));
    state->hand_count--;
}

static BalatroCard random_playing_card(BalatroState *state, const char *stream) {
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
static uint8_t random_sorted_hand_index(BalatroState *state, const char *stream) {
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

static void reset_round_targets(BalatroState *state) {
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

static void add_spectral_cards(BalatroState *state, const uint8_t *ranks, size_t rank_count, uint8_t count, const char *stream) {
    while (count--) {
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

static void draw_after_play(BalatroState *state) {
    uint8_t target = state->hand_size;
    if (!state->blind_disabled && state->blind_id == BALATRO_BLIND_BL_SERPENT) {
        target = state->hand_count + 3;
    }
    while (state->hand_count < target && state->deck_count > 0 && state->hand_count < BALATRO_MAX_HAND)
        state->hand[state->hand_count++] = state->deck[--state->deck_count];
    sort_hand_desc(state);
}

static void apply_discard_effects(BalatroState *state, const BalatroCard *cards, uint8_t count, int first_discard, int hook) {
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

static void apply_hook_discard(BalatroState *state) {
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

static int find_discard_card(const BalatroState *state, uint16_t sort_id) {
    for (uint16_t i = 0; i < state->discard_count; ++i)
        if (state->discard[i].sort_id == sort_id) return (int)i;
    return -1;
}

static void apply_drawn_to_hand_boss(BalatroState *state, int crimson_prepped) {
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

static double probability_normal(const BalatroState *state, double base) {
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

static void sort_hand_mode(BalatroState *state, int by_suit) {
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

static void sort_hand_desc(BalatroState *state) {
    sort_hand_mode(state, state->hand_sort_suit != 0);
}

static uint16_t choose_boss(BalatroState *state) {
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
}

static int blind_debuffs_card(const BalatroState *state, const BalatroCard *card) {
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

static void refresh_card_debuff(const BalatroState *state, BalatroCard *card) {
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

static void apply_card_debuffs(BalatroState *state) {
    for (uint16_t i = 0; i < state->deck_count; ++i)
        if (blind_debuffs_card(state, &state->deck[i])) state->deck[i].flags |= 1u;
    for (uint8_t i = 0; i < state->hand_count; ++i)
        if (blind_debuffs_card(state, &state->hand[i])) state->hand[i].flags |= 1u;
}

static void swap_jokers(BalatroState *state, uint8_t left, uint8_t right) {
    BalatroCard card = state->jokers[left];
    state->jokers[left] = state->jokers[right];
    state->jokers[right] = card;
}

static uint8_t sorted_editionless_jokers(const BalatroState *state, uint8_t indices[BALATRO_MAX_JOKERS]) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < state->joker_count; ++i)
        if (!(state->jokers[i].flags & BALATRO_CARD_DEBUFFED) && state->jokers[i].edition == BALATRO_EDITION_NONE) indices[count++] = i;
    for (uint8_t i = 1; i < count; ++i) {
        uint8_t value = indices[i], j = i;
        while (j && state->jokers[indices[j - 1]].sort_id > state->jokers[value].sort_id) {
            indices[j] = indices[j - 1];
            --j;
        }
        indices[j] = value;
    }
    return count;
}

static void apply_consumable(BalatroState *state, const BalatroAction *action, BalatroCard card) {
    uint8_t set = balatro_card_set(&card);
    BalatroHandType planet = balatro_planet_hand(card.center_id);
    if (planet < BALATRO_HAND_COUNT) {
        state->planet_usage_mask |= (uint16_t)(1u << planet);
        if (state->hand_levels[planet] < 255) state->hand_levels[planet]++;
        for (uint8_t j = 0; j < state->joker_count; ++j)
            if (state->jokers[j].center_id == BALATRO_CENTER_J_CONSTELLATION)
                state->jokers[j].state[0] = (state->jokers[j].state[0] > 100 ? state->jokers[j].state[0] : 100) + 10;
    } else if (card.center_id == BALATRO_CENTER_C_HERMIT) {
        int gain = state->dollars > 0 ? (state->dollars < 20 ? state->dollars : 20) : 0;
        state->dollars += gain;
    } else if (card.center_id == BALATRO_CENTER_C_TEMPERANCE) {
        int total = 0;
        for (uint8_t i = 0; i < state->joker_count; ++i) total += state->jokers[i].sell_cost;
        state->dollars += total > 50 ? 50 : total;
    } else if (card.center_id == BALATRO_CENTER_C_BLACK_HOLE) {
        for (uint8_t i = 0; i < BALATRO_HAND_COUNT; ++i)
            if (state->hand_levels[i] < 255) state->hand_levels[i]++;
    } else if (card.center_id == BALATRO_CENTER_C_SIGIL) {
        static const uint8_t suits[] = {BALATRO_SPADES, BALATRO_HEARTS, BALATRO_DIAMONDS, BALATRO_CLUBS};
        uint8_t suit = suits[(size_t)floor(balatro_pseudorandom(state, "sigil") * 4.0) % 4];
        for (uint8_t i = 0; i < state->hand_count; ++i) state->hand[i].suit = suit;
    } else if (card.center_id == BALATRO_CENTER_C_OUIJA) {
        uint8_t rank = (uint8_t)(2 + floor(balatro_pseudorandom(state, "ouija") * 13.0));
        for (uint8_t i = 0; i < state->hand_count; ++i) state->hand[i].rank = rank;
        if (state->hand_size > 1) state->hand_size--;
        if (state->base_hand_size > 1) state->base_hand_size--;
    } else if (card.center_id == BALATRO_CENTER_C_IMMOLATE) {
        /* Shuffle a temporary sort_id-canonicalized copy to choose
           the destroyed Card objects. It never rearranges G.hand itself, so
           the surviving temporary pack hand retains its visual order before
           finish_pack returns it to the deck. */
        BalatroCard shuffled[BALATRO_MAX_HAND];
        memcpy(shuffled, state->hand, (size_t)state->hand_count * sizeof(BalatroCard));
        balatro_shuffle(state, shuffled, state->hand_count, "immolate");
        uint8_t remove = state->hand_count < 5 ? state->hand_count : 5;
        uint16_t destroyed_ids[5];
        for (uint8_t i = 0; i < remove; ++i) destroyed_ids[i] = shuffled[i].sort_id;
        /* Destroyed cards dissolve in reverse order. Removing the
           same identities from the unshuffled hand preserves survivor order. */
        for (uint8_t i = remove; i-- > 0;) {
            for (uint8_t h = 0; h < state->hand_count; ++h) {
                if (state->hand[h].sort_id == destroyed_ids[i]) {
                    remove_hand_index(state, h);
                    break;
                }
            }
        }
        state->dollars += 20;
    } else if (card.center_id == BALATRO_CENTER_C_FAMILIAR || card.center_id == BALATRO_CENTER_C_GRIM ||
               card.center_id == BALATRO_CENTER_C_INCANTATION) {
        if (state->hand_count) remove_hand_index(state, random_sorted_hand_index(state, "random_destroy"));
        static const uint8_t face_ranks[] = {11, 12, 13};
        static const uint8_t ace_ranks[] = {14};
        static const uint8_t number_ranks[] = {2, 3, 4, 5, 6, 7, 8, 9, 10};
        const uint8_t *ranks = card.center_id == BALATRO_CENTER_C_FAMILIAR ? face_ranks
                               : card.center_id == BALATRO_CENTER_C_GRIM   ? ace_ranks
                                                                           : number_ranks;
        size_t rank_count = card.center_id == BALATRO_CENTER_C_FAMILIAR ? sizeof(face_ranks)
                            : card.center_id == BALATRO_CENTER_C_GRIM   ? sizeof(ace_ranks)
                                                                        : sizeof(number_ranks);
        uint8_t create_count = card.center_id == BALATRO_CENTER_C_FAMILIAR ? 3 : card.center_id == BALATRO_CENTER_C_GRIM ? 2 : 4;
        const char *stream = card.center_id == BALATRO_CENTER_C_FAMILIAR ? "familiar_create"
                             : card.center_id == BALATRO_CENTER_C_GRIM   ? "grim_create"
                                                                         : "incantation_create";
        add_spectral_cards(state, ranks, rank_count, create_count, stream);
    } else if (card.center_id == BALATRO_CENTER_C_CRYPTID && action->selection_count) {
        BalatroCard source = state->hand[action->selection[0]];
        for (uint8_t i = 0; i < 2; ++i) {
            BalatroCard copy = source;
            copy.sort_id = ++state->next_sort_id;
            int added = 0;
            /* Cryptid always emplaces its copies in G.hand.  That includes
               Spectral-pack overlays; finish_pack subsequently returns the
               enlarged temporary hand to the bottom of G.deck. */
            if ((state->phase == BALATRO_PHASE_SELECTING_HAND || state->phase == BALATRO_PHASE_PACK_OPENING) &&
                state->hand_count < BALATRO_MAX_HAND)
                state->hand[state->hand_count++] = copy, added = 1;
            else if (state->deck_count < BALATRO_MAX_DECK)
                state->deck[state->deck_count++] = copy, added = 1;
            if (added) balatro_playing_card_added(state, 1);
        }
        if (state->phase == BALATRO_PHASE_SELECTING_HAND) sort_hand_desc(state);
    } else if (card.center_id == BALATRO_CENTER_C_AURA && action->selection_count) {
        /* Guaranteed Aura edition: Poly > .85, Holo > .50, otherwise Foil. */
        double roll = balatro_pseudorandom(state, "aura");
        uint8_t edition = roll > 0.85 ? 3 : roll > 0.50 ? 2 : 1;
        state->hand[action->selection[0]].edition = edition;
    } else if (card.center_id == BALATRO_CENTER_C_ECTOPLASM) {
        uint8_t eligible[BALATRO_MAX_JOKERS];
        uint8_t count = sorted_editionless_jokers(state, eligible);
        if (count) {
            size_t pick = (size_t)floor(balatro_pseudorandom(state, "ectoplasm") * count);
            state->jokers[eligible[pick]].edition = 4;
            balatro_price_card(state, &state->jokers[eligible[pick]]);
            if (state->joker_slots < UINT8_MAX) state->joker_slots++;
            uint8_t penalty = state->ecto_penalty ? state->ecto_penalty : 1;
            if (state->hand_size > penalty)
                state->hand_size -= penalty;
            else
                state->hand_size = 1;
            if (state->base_hand_size > penalty)
                state->base_hand_size -= penalty;
            else
                state->base_hand_size = 1;
            /* G.GAME.ecto_minus stores the penalty for the next Ectoplasm.
               A missing value initializes to 1, applies, then
               increments to 2. Incrementing the zero sentinel directly
               incorrectly made the first two uses both cost one hand slot. */
            state->ecto_penalty = penalty < UINT8_MAX ? (uint8_t)(penalty + 1) : UINT8_MAX;
        }
    } else if (card.center_id == BALATRO_CENTER_C_HEX) {
        uint8_t eligible[BALATRO_MAX_JOKERS];
        uint8_t count = sorted_editionless_jokers(state, eligible);
        if (count) {
            size_t pick = (size_t)floor(balatro_pseudorandom(state, "hex") * count);
            uint8_t target = eligible[pick];
            state->jokers[target].edition = 3;
            balatro_price_card(state, &state->jokers[target]);
            for (uint8_t i = state->joker_count; i-- > 0;) {
                if (i != target && !(state->jokers[i].flags & (1u << 1))) remove_joker_at(state, i);
            }
        }
    } else if (card.center_id == BALATRO_CENTER_C_WHEEL_OF_FORTUNE) {
        uint8_t eligible[BALATRO_MAX_JOKERS];
        uint8_t count = sorted_editionless_jokers(state, eligible);
        if (count && balatro_pseudorandom(state, "wheel_of_fortune") < probability_normal(state, 0.25)) {
            size_t pick = (size_t)floor(balatro_pseudorandom(state, "wheel_of_fortune") * count);
            double edition = balatro_pseudorandom(state, "wheel_of_fortune");
            state->jokers[eligible[pick]].edition = edition > 0.85 ? 3 : edition > 0.50 ? 2 : 1;
            balatro_price_card(state, &state->jokers[eligible[pick]]);
        }
    } else if (card.center_id == BALATRO_CENTER_C_FOOL) {
        if (state->last_tarot_planet && state->last_tarot_planet != BALATRO_CENTER_C_FOOL) {
            uint8_t saved_slots = state->consumable_slots;
            if (action->type == BALATRO_ACTION_USE_CONSUMABLE && state->consumable_slots < UINT8_MAX) state->consumable_slots++;
            (void)add_specific_consumable(state, state->last_tarot_planet);
            state->consumable_slots = saved_slots;
        }
    } else if (card.center_id == BALATRO_CENTER_C_EMPEROR) {
        uint8_t saved_slots = state->consumable_slots;
        if (action->type == BALATRO_ACTION_USE_CONSUMABLE && state->consumable_slots < UINT8_MAX) state->consumable_slots++;
        uint8_t room = state->consumable_slots - state->consumable_count;
        /* Both queued create_card calls use append='emp'; pseudoseed advances
           Tarotemp{ante} between calls rather than using numbered streams. */
        if (room) (void)add_pooled_consumable(state, SET_TAROT, "emp", 0);
        if (room > 1) (void)add_pooled_consumable(state, SET_TAROT, "emp", 0);
        state->consumable_slots = saved_slots;
    } else if (card.center_id == BALATRO_CENTER_C_HIGH_PRIESTESS) {
        uint8_t saved_slots = state->consumable_slots;
        if (action->type == BALATRO_ACTION_USE_CONSUMABLE && state->consumable_slots < UINT8_MAX) state->consumable_slots++;
        uint8_t room = state->consumable_slots - state->consumable_count;
        if (room) (void)add_pooled_consumable(state, SET_PLANET, "pri", 0);
        if (room > 1) (void)add_pooled_consumable(state, SET_PLANET, "pri", 0);
        state->consumable_slots = saved_slots;
    } else if (card.center_id == BALATRO_CENTER_C_JUDGEMENT) {
        /* Judgement calls create_card('Joker', ..., append='jud') with
           soulable=nil, followed by rarity, pool, sticker, and edition
           streams. A direct common-pool draw from "jud" is not equivalent. */
        if (state->joker_count < state->joker_slots && state->joker_count < BALATRO_MAX_JOKERS) {
            BalatroCard joker = balatro_create_pooled_card(state, 3, "jud", 0);
            state->jokers[state->joker_count++] = joker;
            balatro_joker_added(state, &state->jokers[state->joker_count - 1]);
            balatro_mark_center_used(state, joker.center_id);
        }
    } else if (card.center_id == BALATRO_CENTER_C_DEATH && action->selection_count >= 2) {
        uint8_t left = action->selection[0], right = action->selection[1];
        if (left < state->hand_count && right < state->hand_count && left != right) {
            /* Copy the visually rightmost highlighted card into
               the other. Legal actions enumerate hand positions left-to-right,
               so the latter selected index is the source; sort_id is unrelated
               after a player manually rearranges the hand. */
            BalatroCard source = state->hand[right];
            BalatroCard *target = &state->hand[left];
            target->center_id = source.center_id;
            target->suit = source.suit;
            target->rank = source.rank;
            target->enhancement = source.enhancement;
            target->edition = source.edition;
            target->seal = source.seal;
            target->perma_bonus = source.perma_bonus;
            refresh_card_debuff(state, target);
            target->cost = source.cost;
            target->sell_cost = source.sell_cost;
        }
    } else if (card.center_id == BALATRO_CENTER_C_HANGED_MAN && action->selection_count) {
        /* Glass Joker's using_consumeable hook sees highlighted Glass cards
           before Hanged Man removes them; remove_hand_index below dispatches
           the subsequent remove_playing_cards hook. */
        uint8_t glass = 0;
        for (uint8_t i = 0; i < action->selection_count; ++i)
            if (action->selection[i] < state->hand_count && state->hand[action->selection[i]].enhancement == 4) glass++;
        if (glass)
            for (uint8_t j = 0; j < state->joker_count; ++j)
                if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_GLASS)
                    state->jokers[j].state[0] = (state->jokers[j].state[0] > 100 ? state->jokers[j].state[0] : 100) + glass * 75;
        for (uint8_t i = action->selection_count; i-- > 0;) {
            if (action->selection[i] < state->hand_count) remove_hand_index(state, action->selection[i]);
        }
    } else if (card.center_id == BALATRO_CENTER_C_WRAITH) {
        (void)add_joker_rarity(state, 3, "wra", 0);
        state->dollars = 0;
    } else if (card.center_id == BALATRO_CENTER_C_SOUL) {
        (void)add_joker_rarity(state, 4, "sou", 1);
    } else if (card.center_id == BALATRO_CENTER_C_ANKH && state->joker_count) {
        uint8_t order[BALATRO_MAX_JOKERS];
        for (uint8_t i = 0; i < state->joker_count; ++i) order[i] = i;
        for (uint8_t i = 1; i < state->joker_count; ++i) {
            uint8_t value = order[i], j = i;
            while (j && state->jokers[order[j - 1]].sort_id > state->jokers[value].sort_id) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = value;
        }
        size_t pick = (size_t)floor(balatro_pseudorandom(state, "ankh_choice") * state->joker_count);
        BalatroCard copy = state->jokers[order[pick]];
        for (uint8_t i = state->joker_count; i-- > 0;) {
            if (i != order[pick] && !(state->jokers[i].flags & (1u << 1))) remove_joker_at(state, i);
        }
        if (state->joker_count < state->joker_slots && state->joker_count < BALATRO_MAX_JOKERS) {
            copy.sort_id = ++state->next_sort_id;
            /* Balatro's Ankh strips Negative from the duplicated copy. */
            if (copy.edition == 4) copy.edition = 0;
            balatro_price_card(state, &copy);
            state->jokers[state->joker_count++] = copy;
            balatro_joker_added(state, &state->jokers[state->joker_count - 1]);
        }
    } else if (action->selection_count) {
        const BalatroCenterDefinition *definition = &balatro_centers[card.center_id];
        for (uint8_t i = 0; i < action->selection_count; ++i) {
            uint8_t index = action->selection[i];
            BalatroCard *target = &state->hand[index];
            if (definition->target_effect == TARGET_ENHANCEMENT)
                target->enhancement = definition->target_value;
            else if (definition->target_effect == TARGET_SUIT)
                target->suit = definition->target_value;
            else if (definition->target_effect == TARGET_SEAL)
                target->seal = definition->target_value;
            else if (definition->target_effect == TARGET_RANK_UP)
                target->rank = target->rank == 14 ? 2 : target->rank < 14 ? target->rank + 1 : target->rank;
            refresh_card_debuff(state, target);
        }
    }
    if (set == 4) {
        state->tarots_used++;
        state->last_tarot_planet = card.center_id;
    } else if (set == 5)
        state->last_tarot_planet = card.center_id;
}

static void use_consumable(BalatroState *state, const BalatroAction *action) {
    BalatroCard card = state->consumables[action->primary];
    apply_consumable(state, action, card);
    balatro_consumable_removed(state, &card);
    memmove(&state->consumables[action->primary], &state->consumables[action->primary + 1],
            (state->consumable_count - action->primary - 1) * sizeof(BalatroCard));
    state->consumable_count--;
}

static void sell_consumable(BalatroState *state, uint8_t index) {
    BalatroCard card = state->consumables[index];
    state->dollars += card.sell_cost;
    balatro_consumable_removed(state, &card);
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_CAMPFIRE)
            state->jokers[j].state[0] = (state->jokers[j].state[0] > 100 ? state->jokers[j].state[0] : 100) + 25;
    memmove(&state->consumables[index], &state->consumables[index + 1], (state->consumable_count - index - 1) * sizeof(BalatroCard));
    state->consumable_count--;
}

size_t balatro_state_size(void) {
    return sizeof(BalatroState);
}
size_t balatro_observation_size(void) {
    return sizeof(BalatroObservation);
}

void balatro_clone_state(BalatroState *destination, const BalatroState *source) {
    if (!destination || !source || destination == source) return;
    uint16_t rng_count = source->rng_count < BALATRO_MAX_RNG_STREAMS ? source->rng_count : BALATRO_MAX_RNG_STREAMS;
    memcpy(destination, source, offsetof(BalatroState, rng));
    memcpy(destination->rng, source->rng, (size_t)rng_count * sizeof(source->rng[0]));
    memcpy(&destination->rng_count, &source->rng_count, offsetof(BalatroState, deck) - offsetof(BalatroState, rng_count));
    memcpy(destination->deck, source->deck, (size_t)source->deck_count * sizeof(source->deck[0]));
    memcpy(destination->hand, source->hand, (size_t)source->hand_count * sizeof(source->hand[0]));
    memcpy(destination->discard, source->discard, (size_t)source->discard_count * sizeof(source->discard[0]));
    memcpy(destination->jokers, source->jokers, (size_t)source->joker_count * sizeof(source->jokers[0]));
    memcpy(destination->consumables, source->consumables, (size_t)source->consumable_count * sizeof(source->consumables[0]));
    memcpy(destination->shop_cards, source->shop_cards, (size_t)source->shop_count * sizeof(source->shop_cards[0]));
    memcpy(destination->pack_cards, source->pack_cards, (size_t)source->pack_count * sizeof(source->pack_cards[0]));
    memcpy(&destination->deck_count, &source->deck_count, sizeof(*source) - offsetof(BalatroState, deck_count));
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

static int action_is_legal(const BalatroState *state, const BalatroAction *action);
static int legal_view_impl(const BalatroState *state, BalatroLegalView *out, int count_actions);

static int selected(const BalatroAction *action, uint8_t index) {
    for (uint8_t i = 0; i < action->selection_count; ++i)
        if (action->selection[i] == index) return 1;
    return 0;
}

static int remove_selected(BalatroState *state, const BalatroAction *action, BalatroCard played[BALATRO_MAX_SELECTION]) {
    uint8_t out_count = 0, keep_count = 0;
    BalatroCard keep[BALATRO_MAX_HAND];
    for (uint8_t i = 0; i < state->hand_count; ++i) {
        if (selected(action, i))
            played[out_count++] = state->hand[i];
        else
            keep[keep_count++] = state->hand[i];
    }
    memcpy(state->hand, keep, sizeof(BalatroCard) * keep_count);
    state->hand_count = keep_count;
    return out_count;
}

static uint16_t resolved_joker_center(const BalatroState *state, uint8_t start) {
    uint8_t index = start;
    for (uint8_t depth = 0; depth <= state->joker_count; ++depth) {
        if (index >= state->joker_count) return BALATRO_CENTER_NONE;
        const BalatroCard *joker = &state->jokers[index];
        if (joker->flags & BALATRO_CARD_DEBUFFED) return BALATRO_CENTER_NONE;
        if (joker->center_id == BALATRO_CENTER_J_BLUEPRINT) {
            index = (uint8_t)(index + 1);
            continue;
        }
        if (joker->center_id == BALATRO_CENTER_J_BRAINSTORM) {
            if (index == 0) return BALATRO_CENTER_NONE;
            index = 0;
            continue;
        }
        return joker->center_id;
    }
    return BALATRO_CENTER_NONE;
}

static void finish_blind(BalatroState *state) {
    state->phase = BALATRO_PHASE_ROUND_EVAL;
    /* Blind:defeat restores Manacle's temporary CardArea limit before the
       round-evaluation/shop boundary. Packs opened in that shop therefore
       draw to the normal hand size even before next_round(). */
    if (!state->blind_disabled && state->blind_id == BALATRO_BLIND_BL_MANACLE && state->hand_size < UINT8_MAX) state->hand_size++;
    /* Card:calculate_rental() charges every rental Joker immediately at the
       end-round boundary, even while that Joker is debuffed.  The resulting
       balance is what evaluate_round() uses for interest; rental is not a
       round-evaluation row and must not be delayed until cash-out. */
    for (uint8_t i = 0; i < state->joker_count; ++i)
        if (state->jokers[i].flags & (1u << 3)) state->dollars -= state->rental_rate;
    state->unused_discards += state->discards_left;
    if (state->blind_on_deck == 2) state->most_played_hand = balatro_most_played_hand(state);
    int32_t tag_eval_bonus = 0;
    int32_t gold_hand_bonus = 0;
    for (uint8_t i = 0; i < state->hand_count; ++i) {
        if (state->hand[i].flags & 1u) continue;
        uint8_t repetitions = state->hand[i].seal == 2 ? 2 : 1;
        for (uint8_t j = 0; j < state->joker_count; ++j)
            if (resolved_joker_center(state, j) == BALATRO_CENTER_J_MIME) repetitions++;
        /* Gold Card pays h_dollars during round evaluation.  A Gold Seal
           pays its $3 only when the card is played (get_p_dollars), not while merely held here. */
        if (state->hand[i].enhancement == 7) gold_hand_bonus += 3 * repetitions;
        if (state->hands_played && state->hand[i].seal == 3)
            for (uint8_t repetition = 0; repetition < repetitions; ++repetition)
                if (state->consumable_count < state->consumable_slots)
                    (void)add_specific_consumable(state, balatro_planet_center(state->last_hand_type));
    }
    if (state->tag_investment_pending && state->blind_on_deck == 2) {
        /* Tag:apply_to_run({type='eval'}) contributes to round-eval dollars
           after Joker bonuses but before the displayed bottom row.  Keep it
           out of the balance until cash-out so interest is computed from the
           pre-tag balance. */
        tag_eval_bonus = 25 * state->tag_investment_pending;
        state->tag_investment_pending = 0;
    }
    while (state->discard_count && state->deck_count < BALATRO_MAX_DECK)
        state->deck[state->deck_count++] = state->discard[--state->discard_count];
    while (state->hand_count && state->deck_count < BALATRO_MAX_DECK) state->deck[state->deck_count++] = state->hand[--state->hand_count];
    end_round_jokers(state);
    /* Blind state is transient. Blind:defeat clears playing-card debuffs,
       Crimson Heart's Joker debuff, and any Cerulean Bell forced selection
       before packs or consumables can be used in the following shop. */
    for (uint16_t i = 0; i < state->deck_count; ++i) state->deck[i].flags &= (uint8_t)~(BALATRO_CARD_DEBUFFED | BALATRO_CARD_FORCED);
    for (uint8_t i = 0; i < state->joker_count; ++i) state->jokers[i].flags &= (uint8_t)~BALATRO_CARD_DEBUFFED;
    if (state->blind_on_deck == 2) {
        if (state->config.deck == BALATRO_CENTER_B_ANAGLYPH) state->double_tag = 1;
        /* Boss defeat clears played_this_ante on every playing
           card immediately after a Boss is defeated. */
        for (uint16_t i = 0; i < state->deck_count; ++i) state->deck[i].state[3] = 0;
        state->ante++;
        state->blind_on_deck = 0;
        state->blind_skipped_mask = 0;
        /* At Boss defeat, draw the next voucher first; cash-out
           then draws tags and reset_blinds selects the next boss. */
        state->next_voucher_id = balatro_pick_voucher(state);
        assign_blind_tags(state);
        choose_orbital_hands(state);
        state->boss_rerolled = 0;
        state->next_boss_id = choose_boss(state);
    } else
        state->blind_on_deck++;
    /* D6's temporary reroll base survives the shop and accepted blind, then
       it clears at this end-of-round boundary. */
    if (state->tag_d_six_active) {
        state->tag_d_six_active = 0;
        state->reroll_cost = state->free_rerolls ? 0 : state->reroll_base + state->reroll_increase;
    }
    if (state->blind_id != BALATRO_BLIND_BL_SMALL && state->blind_id != BALATRO_BLIND_BL_BIG) {
        for (uint8_t i = 0; i < state->joker_count; ++i)
            if (!(state->jokers[i].flags & 1u) && state->jokers[i].center_id == BALATRO_CENTER_J_ROCKET)
                state->jokers[i].state[0] = (state->jokers[i].state[0] > 0 ? state->jokers[i].state[0] : 1) + 2;
    }
    for (uint8_t i = 0; i < state->joker_count; ++i) {
        BalatroCard *joker = &state->jokers[i];
        if (joker->flags & 1u) continue;
        if (joker->center_id == BALATRO_CENTER_J_EGG)
            joker->sell_cost += 3;
        else if (joker->center_id == BALATRO_CENTER_J_GIFT) {
            for (uint8_t j = 0; j < state->joker_count; ++j) state->jokers[j].sell_cost++;
            for (uint8_t j = 0; j < state->consumable_count; ++j) state->consumables[j].sell_cost++;
        } else if (joker->center_id == BALATRO_CENTER_J_INVISIBLE)
            joker->state[0]++;
        if ((joker->flags & (1u << 2)) && !(joker->flags & 1u)) {
            if (joker->state[3] <= 1) {
                joker->state[3] = 0;
                joker->flags |= 1u;
            } else
                joker->state[3]--;
        }
    }
    /* Held Gold Cards pay through ease_dollars during end-of-round card
       evaluation.  This precedes evaluate_round(), so the balance and its
       interest row must already include the payout before cash-out. */
    state->dollars += gold_hand_bonus;
    state->round_earnings = balatro_calculate_round_earnings(state) + tag_eval_bonus;
    /* There is no active Blind during round evaluation or the shop. This
       also prevents suit/face/Leaf debuffs from being reapplied to a pack's
       temporary hand before the next blind is selected. */
    state->blind_disabled = 1;
}

/* A training-only potential that tracks actual run progress.  The old shaped
   reward used the raw chip delta, which becomes negative when a defeated
   blind is cleared and chips are reset for the next blind.  It also rewarded
   skipping a blind because blind_on_deck advances for both skips and wins.
   Keep this potential independent of card identity/economy so it cannot alter
   game rules or the sparse evaluation metric. */
static double progress_potential(const BalatroState *state) {
    double completed = state->ante > 0 ? (double)(state->ante - 1) * 3.0 : 0.0;
    if (state->blind_on_deck <= 2) completed += (double)state->blind_on_deck;
    double fraction = 0.0;
    if (state->phase == BALATRO_PHASE_SELECTING_HAND && state->blind_chips > 0) {
        fraction = state->chips / state->blind_chips;
        if (isnan(fraction)) fraction = 0.0;
        if (fraction < 0.0) fraction = 0.0;
        if (fraction > 1.0) fraction = 1.0;
    }
    /* Leave a small, observable increment for crossing the blind boundary. */
    double potential = completed + 0.8 * fraction;
    if (state->won)
        potential += 1.0;
    else if (state->terminal)
        potential -= 1.0;
    return potential;
}

static void remove_joker_at(BalatroState *state, uint8_t index) {
    BalatroCard removed = state->jokers[index];
    balatro_joker_removed(state, &removed);
    memmove(&state->jokers[index], &state->jokers[index + 1], (state->joker_count - index - 1) * sizeof(BalatroCard));
    state->joker_count--;
}

static void end_round_jokers(BalatroState *state) {
    for (uint8_t i = 0; i < state->joker_count; ++i) {
        BalatroCard *joker = &state->jokers[i];
        /* Card:calculate_joker returns immediately for debuffed Jokers, so
           end_of_round hooks must not age or mutate them either. */
        if (joker->flags & 1u) continue;
        if (joker->center_id == BALATRO_CENTER_J_POPCORN) {
            int mult = joker->state[0] > 0 ? joker->state[0] : 20;
            joker->state[0] = mult - 4 > 0 ? mult - 4 : 0;
            if (!joker->state[0]) {
                remove_joker_at(state, i--);
                continue;
            }
        } else if (joker->center_id == BALATRO_CENTER_J_CAMPFIRE && state->blind_on_deck == 2) {
            joker->state[0] = 100;
        } else if (joker->center_id == BALATRO_CENTER_J_HIT_THE_ROAD) {
            joker->state[0] = 100;
        } else if (joker->center_id == BALATRO_CENTER_J_TURTLE_BEAN && joker->state[0] > 0) {
            joker->state[0]--;
            if (!joker->state[0]) {
                remove_joker_at(state, i--);
                continue;
            }
        } else if (joker->center_id == BALATRO_CENTER_J_TODO_LIST) {
            joker->state[1] = choose_to_do_hand(state, joker->state[1]);
        } else if (joker->center_id == BALATRO_CENTER_J_GROS_MICHEL || joker->center_id == BALATRO_CENTER_J_CAVENDISH) {
            const double odds = joker->center_id == BALATRO_CENTER_J_GROS_MICHEL ? 6.0 : 1000.0;
            const char *stream = joker->center_id == BALATRO_CENTER_J_GROS_MICHEL ? "gros_michel" : "cavendish";
            if (balatro_pseudorandom(state, stream) < probability_normal(state, 1.0 / odds)) {
                if (joker->center_id == BALATRO_CENTER_J_GROS_MICHEL) state->gros_michel_extinct = 1;
                remove_joker_at(state, i--);
                continue;
            }
        }
    }
}

static void select_blind_transition(BalatroState *state) {
    state->phase = BALATRO_PHASE_SELECTING_HAND;
    /* Rebuild persistent Card:add_to_deck modifiers below from a clean
       base. This also prevents a Joker obtained from a pre-blind pack from being counted twice. */
    state->hand_size = state->base_hand_size;
    /* select_blind calls ease_round(1) before new_round(). */
    state->round++;
    /* new_round() owns the per-round reroll reset. The previous shop's
       displayed cost remains observable while choosing a blind. */
    reset_round_rerolls(state);
    balatro_clear_card_debuffs(state);
    state->blind_disabled = 0;
    state->blind_only_hand = UINT8_MAX;
    state->blind_hands_mask = 0;
    if (state->blind_on_deck == 2 && state->next_boss_id == 0) state->next_boss_id = choose_boss(state);
    state->blind_id = state->blind_on_deck == 0   ? BALATRO_BLIND_BL_SMALL
                      : state->blind_on_deck == 1 ? BALATRO_BLIND_BL_BIG
                                                  : state->next_boss_id;
    reset_round_targets(state);
    state->chips = 0;
    state->blind_chips = balatro_blind_target(state->ante, state->blind_on_deck, state->stake_scaling);
    if (state->config.deck == BALATRO_CENTER_B_PLASMA) state->blind_chips *= 2;
    if (state->blind_id == BALATRO_BLIND_BL_WALL)
        state->blind_chips *= 2;
    else if (state->blind_id == BALATRO_BLIND_BL_FINAL_VESSEL)
        state->blind_chips *= 3;
    state->blind_reward = blind_reward_for(state->blind_id, state->blind_on_deck);
    if (state->config.stake >= 2 && state->blind_on_deck == 0) state->blind_reward = 0;
    state->hands_left = state->hands_per_round;
    state->discards_left = state->discards_per_round;
    /* Juggle Tag is consumed by the next accepted blind (each blind is a
       round), including Big/Boss after a skip. */
    if (state->tag_hand_bonus) {
        state->hand_size += state->tag_hand_bonus;
        state->tag_hand_bonus = 0;
    }
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        BalatroCard *joker = &state->jokers[j];
        if (joker->flags & 1u) continue;
        if (joker->center_id == BALATRO_CENTER_J_JUGGLER)
            state->hand_size++;
        else if (joker->center_id == BALATRO_CENTER_J_TROUBADOUR) {
            state->hand_size += 2;
            if (state->hands_left > 1) state->hands_left--;
        } else if (joker->center_id == BALATRO_CENTER_J_MERRY_ANDY) {
            if (state->hand_size > 1) state->hand_size--;
            state->discards_left += 3;
        } else if (joker->center_id == BALATRO_CENTER_J_DRUNKARD) {
            state->discards_left++;
        } else if (joker->center_id == BALATRO_CENTER_J_TURTLE_BEAN) {
            if (!joker->state[0]) joker->state[0] = 5;
            state->hand_size += joker->state[0];
        }
    }
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        BalatroCard *joker = &state->jokers[j];
        if (joker->flags & 1u) continue;
        if (joker->center_id == BALATRO_CENTER_J_BURGLAR) {
            state->hands_left += 3;
            state->discards_left = 0;
        } else if (joker->center_id == BALATRO_CENTER_J_CHICOT && state->blind_on_deck == 2) {
            state->blind_disabled = 1;
        } else if (joker->center_id == BALATRO_CENTER_J_CEREMONIAL && j + 1 < state->joker_count) {
            BalatroCard *target = &state->jokers[j + 1];
            if (!(target->flags & (1u << 1))) {
                joker->state[0] += target->sell_cost * 2;
                remove_joker_at(state, j + 1);
            }
        }
    }
    /* Chicot runs in setting_blind after the target was installed.
       Blind:disable explicitly reverses target-only boss modifiers. */
    if (state->blind_disabled) {
        if (state->blind_id == BALATRO_BLIND_BL_WALL)
            state->blind_chips /= 2;
        else if (state->blind_id == BALATRO_BLIND_BL_FINAL_VESSEL)
            state->blind_chips /= 3;
    }
    /* Madness fires during setting_blind on non-Boss blinds: grow its
       xMult and destroy one non-eternal fellow Joker. */
    if (state->blind_on_deck != 2) {
        int madness = -1;
        for (uint8_t j = 0; j < state->joker_count; ++j)
            if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_MADNESS) {
                madness = j;
                state->jokers[j].state[0] = (state->jokers[j].state[0] > 100 ? state->jokers[j].state[0] : 100) + 50;
                break;
            }
        if (madness >= 0) {
            uint8_t eligible[BALATRO_MAX_JOKERS];
            uint8_t count = 0;
            for (uint8_t j = 0; j < state->joker_count; ++j)
                if (j != (uint8_t)madness && !(state->jokers[j].flags & (1u << 1)) && !(state->jokers[j].flags & 1u)) eligible[count++] = j;
            if (count) {
                /* pseudorandom_element canonicalizes Card tables by
                   creation sort_id, independent of their current area
                   order. Joker dragging must therefore not change which
                   fellow Madness destroys for a given RNG value. */
                for (uint8_t i = 1; i < count; ++i) {
                    uint8_t candidate = eligible[i];
                    uint8_t j = i;
                    while (j > 0 && state->jokers[eligible[j - 1]].sort_id > state->jokers[candidate].sort_id) {
                        eligible[j] = eligible[j - 1];
                        --j;
                    }
                    eligible[j] = candidate;
                }
                size_t pick = (size_t)floor(balatro_pseudorandom(state, "madness") * count);
                remove_joker_at(state, eligible[pick]);
            }
        }
    }
    if (!state->blind_disabled && state->blind_id == BALATRO_BLIND_BL_NEEDLE) state->hands_left = 1;
    if (!state->blind_disabled && state->blind_id == BALATRO_BLIND_BL_WATER) state->discards_left = 0;
    if (!state->blind_disabled && state->blind_id == BALATRO_BLIND_BL_MANACLE && state->hand_size > 1) state->hand_size--;
    /* Blind-selection effects run before the round shuffle/draw. */
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        BalatroCard *joker = &state->jokers[j];
        if (joker->flags & 1u) continue;
        if (joker->center_id == BALATRO_CENTER_J_MARBLE && state->deck_count < BALATRO_MAX_DECK) {
            BalatroCard stone = random_playing_card(state, "marb_fr");
            stone.enhancement = 6;
            state->deck[state->deck_count++] = stone;
            balatro_playing_card_added(state, 1);
        } else if (joker->center_id == BALATRO_CENTER_J_CARTOMANCER) {
            add_pooled_consumable(state, SET_TAROT, "car", 0);
        } else if (joker->center_id == BALATRO_CENTER_J_RIFF_RAFF) {
            /* Both create_card calls use Joker1rif{ante}. The first
               result becomes unavailable through used_jokers, so the
               second call follows Joker1rif{ante}_resample2. */
            (void)add_joker_rarity(state, 1, "rif", 0);
            (void)add_joker_rarity(state, 1, "rif", 0);
        }
    }
    state->hands_played = state->discards_used = 0;
    memset(state->hand_plays_round, 0, sizeof(state->hand_plays_round));
    char round_shuffle[32];
    BalatroKeyBuilder round_key;
    balatro_key_begin(&round_key, round_shuffle, sizeof(round_shuffle));
    balatro_key_append(&round_key, "nr");
    balatro_key_append_u64(&round_key, state->ante);
    balatro_shuffle(state, state->deck, state->deck_count, round_shuffle);
    balatro_draw_to_hand(state);
    sort_hand_desc(state);
    apply_card_debuffs(state);
    apply_drawn_to_hand_boss(state, 1);
    /* Certificate is a first-hand-drawn hook: create it directly
       in G.hand after the initial deal, then re-sorts and debuffs it. */
    if (joker_active(state, BALATRO_CENTER_J_CERTIFICATE) && state->hand_count < BALATRO_MAX_HAND) {
        BalatroCard certificate = random_playing_card(state, "cert_fr");
        double seal = balatro_pseudorandom(state, "certsl");
        certificate.seal = seal > 0.75 ? 2 : seal > 0.5 ? 3 : seal > 0.25 ? 1 : 4;
        if (blind_debuffs_card(state, &certificate)) certificate.flags |= 1u;
        state->hand[state->hand_count++] = certificate;
        balatro_playing_card_added(state, 1);
        sort_hand_desc(state);
    }
}

static void resolve_hand_transition(BalatroState *state, const BalatroAction *action) {
    BalatroCard cards[BALATRO_MAX_SELECTION] = {0};
    int count = remove_selected(state, action, cards);
    memcpy(&state->discard[state->discard_count], cards, sizeof(BalatroCard) * (size_t)count);
    state->discard_count += (uint16_t)count;
    if (action->type == BALATRO_ACTION_PLAY_HAND) {
        int first_hand = state->hands_played == 0;
        /* DNA's before hook creates the copy in G.hand before scoring.
           Keeping it in the hand matters for held-card effects and lets
           Hologram observe playing_card_added in gameplay order. */
        if (first_hand && count == 1 && joker_active(state, BALATRO_CENTER_J_DNA) && state->hand_count < BALATRO_MAX_HAND) {
            BalatroCard copy = cards[0];
            copy.sort_id = ++state->next_sort_id;
            copy.flags &= (uint8_t)~1u;
            state->hand[state->hand_count++] = copy;
            sort_hand_desc(state);
            balatro_playing_card_added(state, 1);
        }
        int ox_trigger = 0;
        if (!state->blind_disabled && state->blind_id == BALATRO_BLIND_BL_OX) {
            uint8_t mask = 0;
            BalatroHandType candidate = balatro_classify_hand(cards, (size_t)count, &mask);
            ox_trigger = candidate == (BalatroHandType)state->most_played_hand;
        }
        BalatroScoreResult score;
        apply_hook_discard(state);
        for (uint8_t i = 0; i < state->hand_count; ++i) state->hand[i].flags &= (uint8_t)~(1u << 4);
        state->hands_left--;
        balatro_score_hand(state, cards, (size_t)count, state->hand, state->hand_count, &score);
        /* context.after runs once after every played hand, including a
           hand that neither clears nor ends the blind. Ice Cream marks
           itself during scoring; Seltzer consumes one of its ten hands
           here after contributing its retriggers to that hand. */
        for (uint8_t j = state->joker_count; j-- > 0;) {
            BalatroCard *joker = &state->jokers[j];
            if (joker->center_id == BALATRO_CENTER_J_ICE_CREAM && joker->state[1]) {
                remove_joker_at(state, j);
            } else if (!(joker->flags & BALATRO_CARD_DEBUFFED) && joker->center_id == BALATRO_CENTER_J_SELZER) {
                int remaining = joker->state[0] > 0 ? joker->state[0] : 10;
                if (remaining <= 1)
                    remove_joker_at(state, j);
                else
                    joker->state[0] = remaining - 1;
            }
        }
        state->last_hand_score = score.total;
        state->last_hand_type = (uint8_t)score.hand_type;
        /* First dispatch remove_playing_cards for all destroyed
           cards, then cards_destroyed for the shattered subset. */
        uint8_t shattered = 0, shattered_faces = 0;
        BalatroCard destroyed[BALATRO_MAX_SELECTION] = {0};
        uint8_t destroyed_count = 0;
        for (uint8_t i = 0; i < count; ++i)
            if (score.destroyed_mask & (1u << i)) {
                destroyed[destroyed_count++] = cards[i];
                shattered++;
                if (cards[i].rank >= 11 && cards[i].rank <= 13) shattered_faces++;
            }
        notify_removed_playing_cards(state, destroyed, destroyed_count, 1);
        if (shattered)
            for (uint8_t j = 0; j < state->joker_count; ++j) {
                BalatroCard *joker = &state->jokers[j];
                if (joker->flags & 1u) continue;
                if (joker->center_id == BALATRO_CENTER_J_CAINO && shattered_faces)
                    joker->state[0] = (joker->state[0] > 100 ? joker->state[0] : 100) + shattered_faces * 100;
                else if (joker->center_id == BALATRO_CENTER_J_GLASS)
                    joker->state[0] = (joker->state[0] > 100 ? joker->state[0] : 100) + shattered * 75;
            }
        for (uint8_t i = 0; i < count; ++i)
            if (score.destroyed_mask & (1u << i)) {
                int discard_index = find_discard_card(state, cards[i].sort_id);
                if (discard_index >= 0) {
                    memmove(&state->discard[discard_index], &state->discard[discard_index + 1],
                            (state->discard_count - (uint16_t)discard_index - 1) * sizeof(BalatroCard));
                    state->discard_count--;
                }
            }
        for (uint8_t i = 0; i < count; ++i) {
            cards[i].state[3] = 1;
            int discard_index = find_discard_card(state, cards[i].sort_id);
            if (discard_index >= 0) {
                state->discard[discard_index].state[3] = 1;
                state->discard[discard_index].enhancement = cards[i].enhancement;
                state->discard[discard_index].perma_bonus = cards[i].perma_bonus;
            }
        }
        int has_ace = 0;
        for (uint8_t i = 0; i < count; ++i)
            if (cards[i].rank == 14) has_ace = 1;
        if (first_hand && count == 1 && cards[0].rank == 6 && joker_active(state, BALATRO_CENTER_J_SIXTH_SENSE)) {
            int discard_index = find_discard_card(state, cards[0].sort_id);
            if (discard_index >= 0) {
                memmove(&state->discard[discard_index], &state->discard[discard_index + 1],
                        (state->discard_count - (uint16_t)discard_index - 1) * sizeof(BalatroCard));
                state->discard_count--;
            }
            (void)add_pooled_consumable(state, SET_SPECTRAL, "sixth", 0);
        }
        if (score.hand_type == BALATRO_STRAIGHT_FLUSH && joker_active(state, BALATRO_CENTER_J_SEANCE))
            (void)add_pooled_consumable(state, SET_SPECTRAL, "sea", 0);
        if ((score.hand_type == BALATRO_STRAIGHT || score.hand_type == BALATRO_STRAIGHT_FLUSH) && has_ace &&
            joker_active(state, BALATRO_CENTER_J_SUPERPOSITION))
            (void)add_pooled_consumable(state, SET_TAROT, "sup", 0);
        if (state->dollars <= 4 && joker_active(state, BALATRO_CENTER_J_VAGABOND)) (void)add_pooled_consumable(state, SET_TAROT, "vag", 0);
        state->dollars += score.dollars;
        if (ox_trigger) state->dollars = 0;
        state->chips += state->last_hand_score;
        state->hands_played++;
        state->run_hands_played++;
        /* The clear test is `chips - blind.chips >= 0`, which intentionally does
           not clear an infinity-vs-infinity or any NaN blind. */
        if (state->chips - state->blind_chips >= 0.0)
            finish_blind(state);
        else if (state->hands_left == 0) {
            int saved = 0;
            for (uint8_t j = 0; j < state->joker_count; ++j)
                if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_MR_BONES &&
                    state->chips / state->blind_chips >= 0.25) {
                    remove_joker_at(state, j);
                    saved = 1;
                    break;
                }
            if (saved) {
                /* Mr. Bones marks the blind defeated without fabricating
                   score; source/live retain the player's sub-target chip
                   total on the round-evaluation screen and show a saved blind row worth $0. */
                state->blind_reward = 0;
                finish_blind(state);
            } else {
                state->terminal = 1;
                state->phase = BALATRO_PHASE_GAME_OVER;
            }
        }
    } else {
        int first_discard = state->discards_used == 0;
        state->discards_left--;
        state->discards_used++;
        if (first_discard) {
            for (uint8_t j = 0; j < state->joker_count; ++j)
                if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_BURNT) {
                    uint8_t mask = 0;
                    BalatroHandType discarded_hand = balatro_classify_hand(cards, (size_t)count, &mask);
                    if (state->hand_levels[discarded_hand] < 255) state->hand_levels[discarded_hand]++;
                }
        }
        int pareidolia = joker_active(state, BALATRO_CENTER_J_PAREIDOLIA);
        int face_count = 0;
        for (uint8_t k = 0; k < count; ++k)
            if (!(cards[k].flags & 1u) && (pareidolia || (cards[k].rank >= 11 && cards[k].rank <= 13))) face_count++;
        for (uint8_t j = 0; j < state->joker_count; ++j) {
            BalatroCard *joker = &state->jokers[j];
            if (joker->flags & 1u) continue;
            if (joker->center_id == BALATRO_CENTER_J_FACELESS && face_count >= 3)
                state->dollars += 5;
            else if (joker->center_id == BALATRO_CENTER_J_MAIL) {
                for (uint8_t k = 0; k < count; ++k)
                    if (!(cards[k].flags & 1u) && cards[k].rank == (uint8_t)joker->state[1]) state->dollars += 5;
            } else if (joker->center_id == BALATRO_CENTER_J_TRADING && first_discard && count == 1) {
                state->dollars += 3;
                if (state->discard_count) state->discard_count--;
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
    }
    if (!state->terminal && state->phase == BALATRO_PHASE_SELECTING_HAND) {
        draw_after_play(state);
        apply_card_debuffs(state);
        apply_drawn_to_hand_boss(state, action->type == BALATRO_ACTION_PLAY_HAND);
    }
}

static void skip_blind_transition(BalatroState *state) {
    uint8_t tag = state->blind_on_deck < 2 ? state->blind_tags[state->blind_on_deck] : BALATRO_TAG_NONE;
    state->skips++;
    state->blind_skipped_mask |= (uint8_t)(1u << state->blind_on_deck);
    if (tag != BALATRO_TAG_NONE) {
        apply_skip_tag(state, tag);
        if (tag != BALATRO_TAG_TAG_DOUBLE && state->double_tag) {
            state->double_tag = 0;
            apply_skip_tag(state, tag);
        }
    }
    state->blind_on_deck++;
}

static void cash_out_transition(BalatroState *state) {
    char cashout_shuffle[32];
    BalatroKeyBuilder cashout_key;
    balatro_key_begin(&cashout_key, cashout_shuffle, sizeof(cashout_shuffle));
    balatro_key_append(&cashout_key, "cashout");
    balatro_key_append_u64(&cashout_key, state->ante);
    balatro_shuffle(state, state->deck, state->deck_count, cashout_shuffle);
    state->dollars += state->round_earnings;
    state->round_earnings = 0;
    if (state->ante > state->config.win_ante) {
        state->won = state->terminal = 1;
        state->phase = BALATRO_PHASE_GAME_OVER;
    } else {
        state->phase = BALATRO_PHASE_SHOP;
        balatro_populate_shop(state);
    }
}

static void buy_and_use_transition(BalatroState *state, const BalatroAction *action) {
    BalatroCard card = state->shop_cards[action->primary];
    /* Remove the shop card and pay before use, but never emplace
       Buy & Use consumables in G.consumeables. Card:add_to_deck/remove_from_deck
       hooks still bracket the effect (notably for Negative editions). */
    state->dollars -= card.cost;
    memmove(&state->shop_cards[action->primary], &state->shop_cards[action->primary + 1],
            (state->shop_count - action->primary - 1) * sizeof(BalatroCard));
    state->shop_count--;
    balatro_consumable_added(state, &card);
    apply_consumable(state, action, card);
    balatro_consumable_removed(state, &card);
}

static void reroll_shop_transition(BalatroState *state) {
    balatro_reroll_shop(state);
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_FLASH) state->jokers[j].state[0] += 2;
}

static void pick_pack_card_transition(BalatroState *state, const BalatroAction *action) {
    BalatroCard pack_card = state->pack_cards[action->primary];
    uint8_t set = balatro_card_set(&pack_card);
    /* Consumables selected from Arcana/Celestial/Spectral packs are used
       directly from G.pack_cards. They do not occupy a consumable slot,
       and their effect runs before end_consumeable returns the temporary pack hand to the deck. */
    if (set == SET_TAROT || set == SET_PLANET || set == SET_SPECTRAL) {
        apply_consumable(state, action, pack_card);
        balatro_complete_pack_pick(state, action->primary);
        return;
    }
    balatro_pick_pack_card(state, action->primary);
}

static void skip_pack_transition(BalatroState *state) {
    balatro_skip_pack(state);
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_RED_CARD) state->jokers[j].state[0] += 3;
}

static void next_round_transition(BalatroState *state) {
    if (joker_active(state, BALATRO_CENTER_J_PERKEO) && state->consumable_count && state->consumable_count < state->consumable_slots &&
        state->consumable_count < BALATRO_MAX_CONSUMABLES) {
        size_t index = (size_t)floor(balatro_pseudorandom(state, "perkeo") * state->consumable_count);
        BalatroCard copy = state->consumables[index];
        copy.sort_id = ++state->next_sort_id;
        copy.edition = 4;
        balatro_price_card(state, &copy);
        state->consumables[state->consumable_count++] = copy;
        balatro_consumable_added(state, &copy);
    }
    state->hand_size = state->base_hand_size;
    /* CardAreas disappear with the shop.  Keeping their cards in the
       native state made non-shop observations expose stale inventory. */
    state->shop_count = 0;
    state->phase = BALATRO_PHASE_BLIND_SELECT;
}

static void apply_action_transition(BalatroState *state, const BalatroAction *action) {
    switch (action->type) {
    case BALATRO_ACTION_REROLL_BOSS:
        /* Mark the ante as rerolled before charging and drawing through the
           shared `boss` RNG stream. */
        state->boss_rerolled = 1;
        state->dollars -= 10;
        state->next_boss_id = choose_boss(state);
        break;
    case BALATRO_ACTION_SELECT_BLIND:
        select_blind_transition(state);
        break;
    case BALATRO_ACTION_SKIP_BLIND:
        skip_blind_transition(state);
        break;
    case BALATRO_ACTION_PLAY_HAND:
    case BALATRO_ACTION_DISCARD:
        resolve_hand_transition(state, action);
        break;
    case BALATRO_ACTION_SORT_HAND_RANK:
        state->hand_sort_suit = 0;
        sort_hand_mode(state, 0);
        break;
    case BALATRO_ACTION_SORT_HAND_SUIT:
        state->hand_sort_suit = 1;
        sort_hand_mode(state, 1);
        break;
    case BALATRO_ACTION_SWAP_HAND_LEFT:
    case BALATRO_ACTION_SWAP_HAND_RIGHT: {
        uint8_t other = action->type == BALATRO_ACTION_SWAP_HAND_LEFT ? action->primary - 1 : action->primary + 1;
        BalatroCard card = state->hand[action->primary];
        state->hand[action->primary] = state->hand[other];
        state->hand[other] = card;
        break;
    }
    case BALATRO_ACTION_SWAP_JOKERS_LEFT:
    case BALATRO_ACTION_SWAP_JOKERS_RIGHT: {
        uint8_t other = action->type == BALATRO_ACTION_SWAP_JOKERS_LEFT ? action->primary - 1 : action->primary + 1;
        swap_jokers(state, action->primary, other);
        break;
    }
    case BALATRO_ACTION_CASH_OUT:
        cash_out_transition(state);
        break;
    case BALATRO_ACTION_BUY_CARD:
        balatro_buy_shop_card(state, action->primary);
        break;
    case BALATRO_ACTION_BUY_AND_USE:
        buy_and_use_transition(state, action);
        break;
    case BALATRO_ACTION_SELL_JOKER:
        balatro_sell_joker(state, action->primary);
        break;
    case BALATRO_ACTION_SELL_CONSUMABLE:
        sell_consumable(state, action->primary);
        break;
    case BALATRO_ACTION_REROLL:
        reroll_shop_transition(state);
        break;
    case BALATRO_ACTION_REDEEM_VOUCHER:
        balatro_redeem_voucher(state, action->primary);
        break;
    case BALATRO_ACTION_OPEN_BOOSTER:
        balatro_open_booster(state, action->primary);
        break;
    case BALATRO_ACTION_PICK_PACK_CARD:
        pick_pack_card_transition(state, action);
        break;
    case BALATRO_ACTION_SKIP_PACK:
        skip_pack_transition(state);
        break;
    case BALATRO_ACTION_USE_CONSUMABLE:
        use_consumable(state, action);
        break;
    case BALATRO_ACTION_NEXT_ROUND:
        next_round_transition(state);
        break;
    default:
        break;
    }
}

static int balatro_step_impl(BalatroState *state, const BalatroAction *action, BalatroStepResult *out, int trusted) {
    if (!state || !action || !out) return BALATRO_ERR_ARGUMENT;
    *out = (BalatroStepResult){0};
    if (!trusted && !state_layout_valid(state)) return BALATRO_ERR_INVARIANT;
    if (!trusted && !action_is_legal(state, action)) return BALATRO_ERR_ACTION;
    double previous_progress = progress_potential(state);
    int32_t previous_dollars = state->dollars;
    state->actions_taken++;
    apply_action_transition(state, action);
    out->terminal = state->terminal;
    out->won = state->won;
    out->ante = state->ante;
    out->sparse_reward = state->won ? 1.0f : 0.0f;
    out->reward = out->sparse_reward;
    if (state->config.shaped_reward) {
        double progress_delta = progress_potential(state) - previous_progress;
        /* Skipping is a valid strategic action, but it is not blind
           completion.  Do not leak a positive progress reward for it. */
        if (action->type == BALATRO_ACTION_SKIP_BLIND && progress_delta > 0.0) progress_delta = 0.0;
        double money_delta = (double)(state->dollars - previous_dollars) / 100.0;
        if (progress_delta > 1.0) progress_delta = 1.0;
        if (progress_delta < -1.0) progress_delta = -1.0;
        if (money_delta > 1.0) money_delta = 1.0;
        if (money_delta < -1.0) money_delta = -1.0;
        out->reward += (float)(progress_delta + 0.02 * money_delta);
    }
    return BALATRO_OK;
}

int balatro_step(BalatroState *state, const BalatroAction *action, BalatroStepResult *out) {
    return balatro_step_impl(state, action, out, 0);
}

int balatro_step_trusted(BalatroState *state, const BalatroAction *action, BalatroStepResult *out) {
    return balatro_step_impl(state, action, out, 1);
}

int balatro_score_play_actions_trusted(const BalatroState *state, const BalatroAction *actions, size_t count, double *scores) {
    if (!state || (!actions && count) || (!scores && count)) return BALATRO_ERR_ARGUMENT;
    for (size_t i = 0; i < count; ++i) {
        if (actions[i].type != BALATRO_ACTION_PLAY_HAND) return BALATRO_ERR_ACTION;
        BalatroState candidate;
        balatro_clone_state(&candidate, state);
        BalatroStepResult result;
        balatro_step_impl(&candidate, &actions[i], &result, 1);
        scores[i] = candidate.last_hand_score;
    }
    return BALATRO_OK;
}

static int consumable_target_limit(uint16_t center_id) {
    return center_id < BALATRO_CENTER_COUNT ? balatro_centers[center_id].target_max : 0;
}

static int consumable_target_minimum(uint16_t center_id) {
    return center_id < BALATRO_CENTER_COUNT ? balatro_centers[center_id].target_min : 0;
}

static int consumable_no_target_legal(const BalatroState *state, uint16_t center_id) {
    switch (center_id) {
    case BALATRO_CENTER_C_FAMILIAR:
    case BALATRO_CENTER_C_GRIM:
    case BALATRO_CENTER_C_INCANTATION:
    case BALATRO_CENTER_C_IMMOLATE:
    case BALATRO_CENTER_C_SIGIL:
    case BALATRO_CENTER_C_OUIJA:
        return state->hand_count > 1;
    case BALATRO_CENTER_C_ANKH:
        return state->joker_count > 0 && state->joker_count < state->joker_slots;
    case BALATRO_CENTER_C_WRAITH:
    case BALATRO_CENTER_C_SOUL:
        return state->joker_count < state->joker_slots;
    case BALATRO_CENTER_C_ECTOPLASM:
    case BALATRO_CENTER_C_HEX:
        for (uint8_t i = 0; i < state->joker_count; ++i)
            if (state->jokers[i].edition == 0) return 1;
        return 0;
    case BALATRO_CENTER_C_WHEEL_OF_FORTUNE:
        for (uint8_t i = 0; i < state->joker_count; ++i)
            if (!(state->jokers[i].flags & 1u) && state->jokers[i].edition == 0) return 1;
        return 0;
    case BALATRO_CENTER_C_EMPEROR:
    case BALATRO_CENTER_C_HIGH_PRIESTESS:
        /* Consumable use removes the original card before creating the
           generated cards, so a full area still has one transient slot. */
        return 1;
    case BALATRO_CENTER_C_JUDGEMENT:
        return state->joker_count < state->joker_slots;
    case BALATRO_CENTER_C_FOOL:
        return state->last_tarot_planet != 0 && state->last_tarot_planet != BALATRO_CENTER_C_FOOL;
    default:
        return 1;
    }
}

static int action_is_legal(const BalatroState *state, const BalatroAction *action) {
    if (!state || !action || action->selection_count > BALATRO_MAX_SELECTION) return 0;
    BalatroLegalView view;
    if (legal_view_impl(state, &view, 0)) return 0;
    for (uint16_t i = 0; i < view.group_count; ++i) {
        const BalatroLegalGroup *group = &view.groups[i];
        if (group->kind == BALATRO_LEGAL_DISCRETE) {
            if (!action->selection_count && action->type == group->action.type && action->primary == group->action.primary) return 1;
            continue;
        }
        const BalatroSelectionFamily *family = &group->selection;
        if (action->type != family->type || action->primary != family->primary || action->selection_count < family->minimum ||
            action->selection_count > family->maximum)
            continue;
        uint16_t selected = 0;
        for (uint8_t j = 0; j < action->selection_count; ++j) {
            uint8_t index = action->selection[j];
            if (index >= BALATRO_MAX_HAND || (j && action->selection[j - 1] >= index)) {
                selected = 0;
                break;
            }
            selected |= (uint16_t)(1u << index);
        }
        if (selected && !(selected & (uint16_t)~family->allowed_mask) && (selected & family->required_mask) == family->required_mask)
            return 1;
    }
    return 0;
}

static uint8_t bit_count16(uint16_t value) {
    uint8_t count = 0;
    while (value) {
        value &= (uint16_t)(value - 1);
        count++;
    }
    return count;
}

static uint32_t choose_count(uint8_t n, uint8_t k) {
    if (k > n) return 0;
    if (k > n - k) k = (uint8_t)(n - k);
    uint32_t result = 1;
    for (uint8_t i = 1; i <= k; ++i) result = result * (uint32_t)(n - k + i) / i;
    return result;
}

static uint32_t selection_family_size(const BalatroSelectionFamily *family, uint8_t selected_count) {
    uint8_t allowed = bit_count16(family->allowed_mask);
    uint8_t required = bit_count16(family->required_mask);
    if ((family->required_mask & family->allowed_mask) != family->required_mask || selected_count < required || selected_count > allowed)
        return 0;
    return choose_count((uint8_t)(allowed - required), (uint8_t)(selected_count - required));
}

uint32_t balatro_legal_group_count(const BalatroLegalGroup *group) {
    if (!group) return 0;
    if (group->kind == BALATRO_LEGAL_DISCRETE) return 1;
    if (group->kind != BALATRO_LEGAL_SELECTION) return 0;
    uint32_t count = 0;
    for (uint8_t selected = group->selection.minimum; selected <= group->selection.maximum; ++selected)
        count += selection_family_size(&group->selection, selected);
    return count;
}

int balatro_legal_group_action(const BalatroLegalGroup *group, uint32_t ordinal, BalatroAction *out) {
    if (!group || !out) return BALATRO_ERR_ARGUMENT;
    if (group->kind == BALATRO_LEGAL_DISCRETE) {
        if (ordinal) return BALATRO_ERR_ACTION;
        *out = group->action;
        return BALATRO_OK;
    }
    if (group->kind != BALATRO_LEGAL_SELECTION) return BALATRO_ERR_ACTION;
    const BalatroSelectionFamily *family = &group->selection;
    uint8_t selected_count = family->minimum;
    for (; selected_count <= family->maximum; ++selected_count) {
        uint32_t count = selection_family_size(family, selected_count);
        if (ordinal < count) break;
        ordinal -= count;
    }
    if (selected_count > family->maximum) return BALATRO_ERR_ACTION;
    BalatroAction action = {
        .type = family->type,
        .primary = family->primary,
        .selection_count = selected_count,
    };
    uint16_t chosen = 0;
    int previous = -1;
    for (uint8_t position = 0; position < selected_count; ++position) {
        uint8_t remaining = (uint8_t)(selected_count - position - 1);
        int found = 0;
        for (int candidate = previous + 1; candidate < BALATRO_MAX_HAND; ++candidate) {
            uint16_t bit = (uint16_t)(1u << candidate);
            if (!(family->allowed_mask & bit)) continue;
            uint16_t required_left = (uint16_t)(family->required_mask & ~(chosen | bit));
            uint16_t lower = candidate == 15 ? UINT16_MAX : (uint16_t)((1u << (candidate + 1)) - 1u);
            if (required_left & lower) continue;
            uint16_t allowed_after = (uint16_t)(family->allowed_mask & ~lower);
            uint8_t required_after = bit_count16(required_left);
            uint8_t optional_after = (uint8_t)(bit_count16(allowed_after) - required_after);
            if (required_after > remaining) continue;
            uint32_t completions = choose_count(optional_after, (uint8_t)(remaining - required_after));
            if (ordinal >= completions) {
                ordinal -= completions;
                continue;
            }
            action.selection[position] = (uint8_t)candidate;
            chosen |= bit;
            previous = candidate;
            found = 1;
            break;
        }
        if (!found) return BALATRO_ERR_INVARIANT;
    }
    *out = action;
    return BALATRO_OK;
}

int balatro_legal_group_actions(const BalatroLegalGroup *group, BalatroAction *out, size_t capacity) {
    if (!group || (capacity && !out)) return BALATRO_ERR_ARGUMENT;
    uint32_t count = balatro_legal_group_count(group);
    uint32_t written = count < capacity ? count : (uint32_t)capacity;
    for (uint32_t ordinal = 0; ordinal < written; ++ordinal) {
        int error = balatro_legal_group_action(group, ordinal, &out[ordinal]);
        if (error) return error;
    }
    return (int)count;
}

static void view_add_discrete(BalatroLegalView *view, BalatroAction action, int count_actions) {
    BalatroLegalGroup *group = &view->groups[view->group_count++];
    *group = (BalatroLegalGroup){
        .kind = BALATRO_LEGAL_DISCRETE,
        .action = action,
    };
    if (count_actions) view->action_count++;
}

static uint16_t hand_mask(const BalatroState *state) {
    return state->hand_count == BALATRO_MAX_HAND ? UINT16_MAX : (uint16_t)((1u << state->hand_count) - 1u);
}

static void view_add_selection(BalatroLegalView *view, uint8_t type, uint8_t primary, uint8_t minimum, uint8_t maximum, uint16_t allowed,
                               uint16_t required, int count_actions) {
    if (maximum > bit_count16(allowed)) maximum = bit_count16(allowed);
    if (minimum > maximum || bit_count16(required) > maximum || (required & allowed) != required) return;
    BalatroLegalGroup *group = &view->groups[view->group_count];
    *group = (BalatroLegalGroup){
        .kind = BALATRO_LEGAL_SELECTION,
        .selection =
            {
                .type = type,
                .primary = primary,
                .minimum = minimum,
                .maximum = maximum,
                .allowed_mask = allowed,
                .required_mask = required,
            },
    };
    view->group_count++;
    if (count_actions) view->action_count += balatro_legal_group_count(group);
}

static uint16_t consumable_allowed_mask(const BalatroState *state, uint16_t center_id) {
    uint16_t allowed = hand_mask(state);
    if (center_id == BALATRO_CENTER_C_AURA)
        for (uint8_t i = 0; i < state->hand_count; ++i)
            if (state->hand[i].edition != BALATRO_EDITION_NONE) allowed &= (uint16_t)~(1u << i);
    return allowed;
}

static void view_add_consumables(const BalatroState *state, BalatroLegalView *view, int count_actions) {
    for (uint8_t i = 0; i < state->consumable_count; ++i) {
        const BalatroCard *card = &state->consumables[i];
        if (!(card->flags & BALATRO_CARD_ETERNAL))
            view_add_discrete(view, (BalatroAction){.type = BALATRO_ACTION_SELL_CONSUMABLE, .primary = i}, count_actions);
        int maximum = consumable_target_limit(card->center_id);
        if (!maximum) {
            if (consumable_no_target_legal(state, card->center_id))
                view_add_discrete(view, (BalatroAction){.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = i}, count_actions);
        } else {
            view_add_selection(view, BALATRO_ACTION_USE_CONSUMABLE, i, (uint8_t)consumable_target_minimum(card->center_id),
                               (uint8_t)maximum, consumable_allowed_mask(state, card->center_id), 0, count_actions);
        }
    }
}

static int legal_view_impl(const BalatroState *state, BalatroLegalView *out, int count_actions) {
    if (!state || !out) return BALATRO_ERR_ARGUMENT;
    _Static_assert(BALATRO_MAX_LEGAL_GROUPS >= 84, "legal view cannot hold the fixed-layout maximum");
    *out = (BalatroLegalView){0};
#define ADD(action_value) view_add_discrete(out, (action_value), count_actions)
    if (state->phase == BALATRO_PHASE_BLIND_SELECT) {
        ADD(((BalatroAction){.type = BALATRO_ACTION_SELECT_BLIND}));
        if (state->blind_on_deck < 2) ADD(((BalatroAction){.type = BALATRO_ACTION_SKIP_BLIND}));
        int can_reroll = balatro_center_used(state, BALATRO_CENTER_V_RETCON) ||
                         (balatro_center_used(state, BALATRO_CENTER_V_DIRECTORS_CUT) && !state->boss_rerolled);
        if (can_reroll && balatro_can_afford(state, 10)) ADD(((BalatroAction){.type = BALATRO_ACTION_REROLL_BOSS}));
    } else if (state->phase == BALATRO_PHASE_SELECTING_HAND) {
        uint16_t allowed = hand_mask(state), forced = 0;
        uint8_t maximum = (uint8_t)(BALATRO_MAX_DECK - state->discard_count);
        if (maximum > BALATRO_MAX_SELECTION) maximum = BALATRO_MAX_SELECTION;
        for (uint8_t i = 0; i < state->hand_count; ++i)
            if (state->hand[i].flags & BALATRO_CARD_FORCED) {
                forced = (uint16_t)(1u << i);
                break;
            }
        if (state->hands_left) {
            view_add_selection(out, BALATRO_ACTION_PLAY_HAND, 0, 1, maximum, allowed, forced, count_actions);
        }
        if (state->discards_left) {
            view_add_selection(out, BALATRO_ACTION_DISCARD, 0, 1, maximum, allowed, 0, count_actions);
        }
        for (uint8_t i = 0; i < state->hand_count; ++i) {
            if (i) ADD(((BalatroAction){.type = BALATRO_ACTION_SWAP_HAND_LEFT, .primary = i}));
            if (i + 1 < state->hand_count) ADD(((BalatroAction){.type = BALATRO_ACTION_SWAP_HAND_RIGHT, .primary = i}));
        }
        ADD(((BalatroAction){.type = BALATRO_ACTION_SORT_HAND_RANK}));
        ADD(((BalatroAction){.type = BALATRO_ACTION_SORT_HAND_SUIT}));
    } else if (state->phase == BALATRO_PHASE_ROUND_EVAL) {
        ADD(((BalatroAction){.type = BALATRO_ACTION_CASH_OUT}));
    } else if (state->phase == BALATRO_PHASE_SHOP) {
        for (uint8_t i = 0; i < state->shop_count; ++i) {
            const BalatroCard *card = &state->shop_cards[i];
            uint8_t set = balatro_card_set(card);
            int affordable = balatro_can_afford(state, card->cost);
            int has_space = set == SET_JOKER ? state->joker_count < BALATRO_MAX_JOKERS &&
                                                   (state->joker_count < state->joker_slots || card->edition == BALATRO_EDITION_NEGATIVE)
                            : (set >= SET_TAROT && set <= SET_SPECTRAL)
                                ? state->consumable_count < BALATRO_MAX_CONSUMABLES &&
                                      (state->consumable_count < state->consumable_slots || card->edition == BALATRO_EDITION_NEGATIVE)
                            : (set == SET_DEFAULT || set == SET_ENHANCED) ? state->deck_count < BALATRO_MAX_DECK
                                                                        : 0;
            if ((set == SET_DEFAULT || (set >= SET_ENHANCED && set <= SET_SPECTRAL)) && affordable && has_space)
                ADD(((BalatroAction){.type = BALATRO_ACTION_BUY_CARD, .primary = i}));
            if (set >= SET_TAROT && set <= SET_SPECTRAL && affordable) {
                int maximum = consumable_target_limit(card->center_id);
                if (!maximum) {
                    if (consumable_no_target_legal(state, card->center_id))
                        ADD(((BalatroAction){.type = BALATRO_ACTION_BUY_AND_USE, .primary = i}));
                } else {
                    view_add_selection(out, BALATRO_ACTION_BUY_AND_USE, i, (uint8_t)consumable_target_minimum(card->center_id),
                                       (uint8_t)maximum, consumable_allowed_mask(state, card->center_id), 0, count_actions);
                }
            }
            if (set == SET_VOUCHER && affordable) ADD(((BalatroAction){.type = BALATRO_ACTION_REDEEM_VOUCHER, .primary = i}));
            if (set == SET_BOOSTER && affordable) ADD(((BalatroAction){.type = BALATRO_ACTION_OPEN_BOOSTER, .primary = i}));
        }
        if (state->free_rerolls || balatro_can_afford(state, state->reroll_cost)) ADD(((BalatroAction){.type = BALATRO_ACTION_REROLL}));
        ADD(((BalatroAction){.type = BALATRO_ACTION_NEXT_ROUND}));
    } else if (state->phase == BALATRO_PHASE_PACK_OPENING) {
        for (uint8_t i = 0; i < state->pack_count; ++i) {
            const BalatroCard *card = &state->pack_cards[i];
            uint8_t set = balatro_card_set(card);
            int maximum = consumable_target_limit(card->center_id);
            if (set >= SET_TAROT && set <= SET_SPECTRAL && maximum) {
                view_add_selection(out, BALATRO_ACTION_PICK_PACK_CARD, i, (uint8_t)consumable_target_minimum(card->center_id),
                                   (uint8_t)maximum, consumable_allowed_mask(state, card->center_id), 0, count_actions);
            } else if (!(set >= SET_TAROT && set <= SET_SPECTRAL) || consumable_no_target_legal(state, card->center_id)) {
                int has_space =
                    set != SET_JOKER || (state->joker_count < BALATRO_MAX_JOKERS &&
                                         (state->joker_count < state->joker_slots || card->edition == BALATRO_EDITION_NEGATIVE));
                if ((set == SET_PLAYING || set == SET_ENHANCED) && state->deck_count >= BALATRO_MAX_DECK) has_space = 0;
                if (has_space) ADD(((BalatroAction){.type = BALATRO_ACTION_PICK_PACK_CARD, .primary = i}));
            }
        }
        ADD(((BalatroAction){.type = BALATRO_ACTION_SKIP_PACK}));
    }
    if (state->phase == BALATRO_PHASE_BLIND_SELECT || state->phase == BALATRO_PHASE_SELECTING_HAND || state->phase == BALATRO_PHASE_SHOP ||
        state->phase == BALATRO_PHASE_PACK_OPENING) {
        for (uint8_t i = 0; i < state->joker_count; ++i) {
            if (i) ADD(((BalatroAction){.type = BALATRO_ACTION_SWAP_JOKERS_LEFT, .primary = i}));
            if (i + 1 < state->joker_count) ADD(((BalatroAction){.type = BALATRO_ACTION_SWAP_JOKERS_RIGHT, .primary = i}));
            if (!(state->jokers[i].flags & BALATRO_CARD_ETERNAL)) ADD(((BalatroAction){.type = BALATRO_ACTION_SELL_JOKER, .primary = i}));
        }
    }
    if (state->phase == BALATRO_PHASE_BLIND_SELECT || state->phase == BALATRO_PHASE_SELECTING_HAND ||
        state->phase == BALATRO_PHASE_ROUND_EVAL || state->phase == BALATRO_PHASE_SHOP || state->phase == BALATRO_PHASE_PACK_OPENING) {
        view_add_consumables(state, out, count_actions);
    }
#undef ADD
    return BALATRO_OK;
}

int balatro_legal_view(const BalatroState *state, BalatroLegalView *out) {
    return legal_view_impl(state, out, 1);
}

float balatro_signed_log2(double value) {
    if (isnan(value)) return 0.0f;
    if (isinf(value)) return signbit(value) ? -1024.0f : 1024.0f;
    if (value == 0.0) return 0.0f;
    return (float)copysign(log2(1.0 + fabs(value)), value);
}

static uint8_t number_class(double value) {
    if (isnan(value)) return BALATRO_NUMBER_NAN;
    if (isinf(value)) return signbit(value) ? BALATRO_NUMBER_NEGATIVE_INFINITY : BALATRO_NUMBER_POSITIVE_INFINITY;
    return BALATRO_NUMBER_FINITE;
}

typedef struct ObservationVariantBuild {
    uint64_t key;
    uint8_t rank;
    uint8_t suit;
    uint8_t enhancement;
    uint8_t edition;
    uint8_t seal;
    uint8_t flags;
    int16_t perma_bonus;
    uint16_t owned_count;
    uint16_t draw_count;
    uint16_t hand_count;
    uint16_t discard_count;
} ObservationVariantBuild;

static uint8_t public_playing_flags(const BalatroCard *card) {
    uint8_t flags = card->flags & (BALATRO_CARD_DEBUFFED | BALATRO_CARD_ETERNAL | BALATRO_CARD_PERISHABLE | BALATRO_CARD_RENTAL |
                                   BALATRO_CARD_FORCED);
    if (card->state[3]) flags |= BALATRO_PUBLIC_CARD_PLAYED_THIS_ANTE;
    return flags;
}

static uint64_t observation_variant_key(const BalatroCard *card) {
    uint64_t key = card->rank;
    key = (key << 2) | card->suit;
    key = (key << 4) | card->enhancement;
    key = (key << 3) | card->edition;
    key = (key << 3) | card->seal;
    key = (key << 16) | (uint16_t)((int32_t)card->perma_bonus - INT16_MIN);
    return (key << 7) | public_playing_flags(card);
}

static uint64_t observation_variant_build_key(const ObservationVariantBuild *variant) {
    uint64_t key = variant->rank;
    key = (key << 2) | variant->suit;
    key = (key << 4) | variant->enhancement;
    key = (key << 3) | variant->edition;
    key = (key << 3) | variant->seal;
    key = (key << 16) | (uint16_t)((int32_t)variant->perma_bonus - INT16_MIN);
    return (key << 7) | variant->flags;
}

static void append_observation_variant(ObservationVariantBuild *variants, uint16_t *count, const BalatroCard *card, uint8_t zone) {
    ObservationVariantBuild value = {
        .rank = card->rank,
        .suit = card->suit,
        .enhancement = card->enhancement,
        .edition = card->edition,
        .seal = card->seal,
        .flags = public_playing_flags(card),
        .perma_bonus = card->perma_bonus,
        .owned_count = 1,
    };
    if (zone == 0)
        value.draw_count = 1;
    else if (zone == 1)
        value.hand_count = 1;
    else
        value.discard_count = 1;
    variants[(*count)++] = value;
}

static uint16_t merge_observation_variants(ObservationVariantBuild *variants, uint16_t count) {
    enum { SIMPLE_VARIANT_SLOTS = 13 * 4 * 2 };
    typedef struct ObservationZoneCounts {
        uint16_t owned;
        uint16_t draw;
        uint16_t hand;
        uint16_t discard;
    } ObservationZoneCounts;
    ObservationZoneCounts simple[SIMPLE_VARIANT_SLOTS] = {0};
    int simple_cards = 1;
    for (uint16_t i = 0; i < count; ++i) {
        const ObservationVariantBuild *variant = &variants[i];
        if (variant->rank < 2 || variant->rank > 14 || variant->suit >= 4 ||
            variant->enhancement != BALATRO_ENHANCEMENT_NONE || variant->edition != BALATRO_EDITION_NONE ||
            variant->seal != BALATRO_SEAL_NONE || variant->perma_bonus != 0 ||
            (variant->flags != 0 && variant->flags != BALATRO_PUBLIC_CARD_PLAYED_THIS_ANTE)) {
            simple_cards = 0;
            break;
        }
        uint16_t slot = (uint16_t)((((variant->rank - 2) * 4 + variant->suit) * 2) +
                                   (variant->flags == BALATRO_PUBLIC_CARD_PLAYED_THIS_ANTE));
        simple[slot].owned += variant->owned_count;
        simple[slot].draw += variant->draw_count;
        simple[slot].hand += variant->hand_count;
        simple[slot].discard += variant->discard_count;
    }
    if (simple_cards) {
        uint16_t merged = 0;
        for (uint16_t slot = 0; slot < SIMPLE_VARIANT_SLOTS; ++slot) {
            const ObservationZoneCounts *counts = &simple[slot];
            if (!counts->owned) continue;
            uint16_t card = (uint16_t)(slot / 2);
            ObservationVariantBuild *variant = &variants[merged++];
            *variant = (ObservationVariantBuild){
                .rank = (uint8_t)(card / 4 + 2),
                .suit = (uint8_t)(card % 4),
                .flags = (slot & 1) ? BALATRO_PUBLIC_CARD_PLAYED_THIS_ANTE : 0,
                .owned_count = counts->owned,
                .draw_count = counts->draw,
                .hand_count = counts->hand,
                .discard_count = counts->discard,
            };
            variant->key = observation_variant_build_key(variant);
        }
        return merged;
    }

    ObservationVariantBuild scratch[BALATRO_OBS_MAX_PLAYING_CARDS];
    ObservationVariantBuild *source = variants, *destination = scratch;
    for (uint16_t i = 0; i < count; ++i) variants[i].key = observation_variant_build_key(&variants[i]);
    /* The canonical tuple occupies 39 bits.  A five-pass stable byte radix
       sort avoids qsort's indirect comparator on every environment step. */
    uint16_t buckets[256] = {0};
    for (uint8_t pass = 0; pass < 5; ++pass) {
        uint8_t shift = (uint8_t)(pass * 8);
        uint8_t minimum = UINT8_MAX, maximum = 0;
        for (uint16_t i = 0; i < count; ++i) {
            uint8_t bucket = (uint8_t)((source[i].key >> shift) & 0xffu);
            buckets[bucket]++;
            if (bucket < minimum) minimum = bucket;
            if (bucket > maximum) maximum = bucket;
        }
        if (minimum == maximum) {
            buckets[minimum] = 0;
            continue;
        }
        uint16_t position = 0;
        for (uint16_t bucket = minimum; bucket <= maximum; ++bucket) {
            uint16_t size = buckets[bucket];
            buckets[bucket] = position;
            position = (uint16_t)(position + size);
        }
        for (uint16_t i = 0; i < count; ++i) destination[buckets[(source[i].key >> shift) & 0xffu]++] = source[i];
        memset(&buckets[minimum], 0, (size_t)(maximum - minimum + 1) * sizeof(*buckets));
        ObservationVariantBuild *swap = source;
        source = destination;
        destination = swap;
    }
    if (source != variants) memcpy(variants, source, count * sizeof(*variants));
    uint16_t merged = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (merged && variants[merged - 1].key == variants[i].key) {
            variants[merged - 1].owned_count += variants[i].owned_count;
            variants[merged - 1].draw_count += variants[i].draw_count;
            variants[merged - 1].hand_count += variants[i].hand_count;
            variants[merged - 1].discard_count += variants[i].discard_count;
        } else {
            variants[merged++] = variants[i];
        }
    }
    return merged;
}

static uint16_t find_observation_variant(const ObservationVariantBuild *variants, uint16_t count, const BalatroCard *card) {
    uint64_t key = observation_variant_key(card);
    uint16_t low = 0, high = count;
    while (low < high) {
        uint16_t middle = (uint16_t)(low + (high - low) / 2);
        if (variants[middle].key < key)
            low = (uint16_t)(middle + 1);
        else
            high = middle;
    }
    if (low < count && variants[low].key == key) return low;
    return UINT16_MAX;
}

static void deck_summary_add(BalatroDeckSummary *summary, const BalatroCard *card) {
    if (card->rank >= 2 && card->rank <= 14 && card->suit < 4 && card->enhancement == BALATRO_ENHANCEMENT_NONE &&
        card->edition == BALATRO_EDITION_NONE && card->seal == BALATRO_SEAL_NONE && card->perma_bonus == 0) {
        uint8_t rank = (uint8_t)(card->rank - 2);
        summary->rank[rank]++;
        summary->suit[card->suit]++;
        summary->rank_suit[card->suit][rank]++;
        summary->face += card->rank >= 11 && card->rank <= 13;
        summary->numbered += card->rank <= 10;
        summary->ace += card->rank == 14;
        summary->enhancement[BALATRO_ENHANCEMENT_NONE]++;
        summary->edition[BALATRO_EDITION_NONE]++;
        summary->seal[BALATRO_SEAL_NONE]++;
        summary->unmodified++;
        summary->total++;
        return;
    }
    int stone = card->enhancement == BALATRO_ENHANCEMENT_STONE;
    /* The deck viewer excludes Stone Cards from its base rank, suit,
       face, numbered, and Ace tallies.  Their physical front remains present
       losslessly in the variant dictionary. */
    if (!stone) {
        if (card->rank >= 2 && card->rank <= 14) {
            uint8_t rank = (uint8_t)(card->rank - 2);
            summary->rank[rank]++;
            if (card->suit < 4) summary->rank_suit[card->suit][rank]++;
        }
        if (card->suit < 4) summary->suit[card->suit]++;
        summary->face += card->rank >= 11 && card->rank <= 13;
        summary->numbered += card->rank >= 2 && card->rank <= 10;
        summary->ace += card->rank == 14;
    }
    if (card->enhancement <= BALATRO_ENHANCEMENT_LUCKY) summary->enhancement[card->enhancement]++;
    if (card->edition <= BALATRO_EDITION_NEGATIVE) summary->edition[card->edition]++;
    if (card->seal <= BALATRO_SEAL_PURPLE) summary->seal[card->seal]++;
    summary->stone += stone;
    summary->wild += card->enhancement == BALATRO_ENHANCEMENT_WILD;
    summary->steel += card->enhancement == BALATRO_ENHANCEMENT_STEEL;
    summary->gold += card->enhancement == BALATRO_ENHANCEMENT_GOLD;
    summary->glass += card->enhancement == BALATRO_ENHANCEMENT_GLASS;
    summary->enhanced += card->enhancement != BALATRO_ENHANCEMENT_NONE;
    summary->unmodified += card->enhancement == BALATRO_ENHANCEMENT_NONE && card->edition == BALATRO_EDITION_NONE &&
                           card->seal == BALATRO_SEAL_NONE && card->perma_bonus == 0;
    summary->total++;
}

static int observation_overflow(BalatroObservation *out, uint8_t section, uint16_t required) {
    out->truncation_reason = BALATRO_TRUNCATED_OBSERVATION_CAPACITY;
    out->overflow_section = section;
    out->required_capacity = required;
    return BALATRO_ERR_OBSERVATION_CAPACITY;
}

static int shop_observation_slot(const BalatroState *state, uint8_t native_index, uint8_t wanted_set) {
    int slot = 0;
    for (uint8_t i = 0; i < state->shop_count; ++i) {
        uint8_t set = balatro_card_set(&state->shop_cards[i]);
        int matches = wanted_set == SET_DEFAULT ? set != SET_VOUCHER && set != SET_BOOSTER
                                               : set == wanted_set;
        if (!matches) continue;
        if (i == native_index) return slot;
        slot++;
    }
    return -1;
}

static int native_shop_index(const BalatroState *state, uint16_t slot, uint8_t wanted_set) {
    uint16_t current = 0;
    for (uint8_t i = 0; i < state->shop_count; ++i) {
        uint8_t set = balatro_card_set(&state->shop_cards[i]);
        int matches = wanted_set == SET_DEFAULT ? set != SET_VOUCHER && set != SET_BOOSTER
                                               : set == wanted_set;
        if (matches && current++ == slot) return i;
    }
    return -1;
}

static int action_has_primary(uint8_t type) {
    return type >= BALATRO_ACTION_BUY_CARD && type <= BALATRO_ACTION_SWAP_HAND_RIGHT;
}

static int observed_action_primary(const BalatroState *state, uint8_t type, uint8_t native_primary) {
    if (type == BALATRO_ACTION_BUY_CARD || type == BALATRO_ACTION_BUY_AND_USE)
        return shop_observation_slot(state, native_primary, SET_DEFAULT);
    if (type == BALATRO_ACTION_REDEEM_VOUCHER) return shop_observation_slot(state, native_primary, SET_VOUCHER);
    if (type == BALATRO_ACTION_OPEN_BOOSTER) return shop_observation_slot(state, native_primary, SET_BOOSTER);
    return action_has_primary(type) ? native_primary : 0;
}

static BalatroObservedSelection *observed_selection(BalatroObservation *out, uint8_t type, uint16_t primary) {
    if (type == BALATRO_ACTION_PLAY_HAND) return &out->legal.play;
    if (type == BALATRO_ACTION_DISCARD) return &out->legal.discard;
    if (type == BALATRO_ACTION_USE_CONSUMABLE && primary < BALATRO_OBS_MAX_CONSUMABLES) return &out->legal.consumable[primary];
    if (type == BALATRO_ACTION_BUY_AND_USE && primary < BALATRO_OBS_MAX_SHOP_MAIN) return &out->legal.shop[primary];
    if (type == BALATRO_ACTION_PICK_PACK_CARD && primary < BALATRO_OBS_MAX_PACK_CARDS) return &out->legal.pack[primary];
    return NULL;
}

static int observe_legality(const BalatroState *state, BalatroObservation *out) {
    BalatroLegalView view;
    int error = legal_view_impl(state, &view, 0);
    if (error) return error;
    for (uint16_t i = 0; i < view.group_count; ++i) {
        const BalatroLegalGroup *group = &view.groups[i];
        uint8_t type = group->kind == BALATRO_LEGAL_DISCRETE ? group->action.type : group->selection.type;
        uint8_t native_primary = group->kind == BALATRO_LEGAL_DISCRETE ? group->action.primary : group->selection.primary;
        if (type >= BALATRO_ACTION_TYPE_COUNT) continue;
        int primary = observed_action_primary(state, type, native_primary);
        if (primary < 0 || primary >= 64) continue;
        out->legal.action_type[type] = 1;
        out->legal.primary[type] |= UINT64_C(1) << primary;
        if (group->kind == BALATRO_LEGAL_SELECTION) {
            BalatroObservedSelection *selection = observed_selection(out, type, (uint16_t)primary);
            if (selection) {
                selection->allowed_hand = group->selection.allowed_mask;
                selection->required_hand = group->selection.required_mask;
                selection->minimum = group->selection.minimum;
                selection->maximum = group->selection.maximum;
                selection->valid = 1;
            }
        }
        if (type == BALATRO_ACTION_SWAP_HAND_LEFT && native_primary < BALATRO_OBS_MAX_HAND)
            out->legal.hand_reorder_destination[native_primary] |= UINT64_C(1) << (native_primary - 1);
        else if (type == BALATRO_ACTION_SWAP_HAND_RIGHT && native_primary < BALATRO_OBS_MAX_HAND)
            out->legal.hand_reorder_destination[native_primary] |= UINT64_C(1) << (native_primary + 1);
        else if (type == BALATRO_ACTION_SWAP_JOKERS_LEFT && native_primary < BALATRO_OBS_MAX_JOKERS)
            out->legal.joker_reorder_destination[native_primary] |= UINT64_C(1) << (native_primary - 1);
        else if (type == BALATRO_ACTION_SWAP_JOKERS_RIGHT && native_primary < BALATRO_OBS_MAX_JOKERS)
            out->legal.joker_reorder_destination[native_primary] |= UINT64_C(1) << (native_primary + 1);
    }
    return BALATRO_OK;
}

#define OBSERVE_TABLE_CARD(table, index, source, playing)                                                                             \
    do {                                                                                                                              \
        const BalatroCard *_card = (source);                                                                                          \
        (table).center_id[index] = _card->center_id;                                                                                  \
        (table).rank[index] = _card->rank;                                                                                            \
        (table).suit[index] = _card->suit;                                                                                            \
        (table).enhancement[index] = _card->enhancement;                                                                              \
        (table).edition[index] = _card->edition;                                                                                      \
        (table).seal[index] = _card->seal;                                                                                            \
        (table).flags[index] = (playing) ? public_playing_flags(_card) : _card->flags;                                                \
        (table).perma_bonus[index] = balatro_signed_log2(_card->perma_bonus);                                                         \
        (table).cost[index] = balatro_signed_log2(_card->cost);                                                                       \
        (table).sell_cost[index] = balatro_signed_log2(_card->sell_cost);                                                             \
        for (uint8_t _mutable = 0; _mutable < 4; ++_mutable) {                                                                        \
            (table).mutable_raw[_mutable][index] = _card->state[_mutable];                                                            \
            (table).mutable_value[_mutable][index] = balatro_signed_log2(_card->state[_mutable]);                                     \
        }                                                                                                                             \
        (table).valid[index] = 1;                                                                                                    \
    } while (0)

int balatro_observe(const BalatroState *state, BalatroObservation *out) {
    if (!state || !out) return BALATRO_ERR_ARGUMENT;
    if (!state_layout_valid(state)) return BALATRO_ERR_INVARIANT;
    memset(out, 0, sizeof(*out));
    out->profile = state->config.observation;
    out->encoded_bytes = sizeof(*out);
    const BalatroObservationProfile *profile = &state->config.observation;
    uint16_t playing_count = (uint16_t)(state->deck_count + state->hand_count + state->discard_count);
    if (playing_count > profile->playing_cards)
        return observation_overflow(out, BALATRO_OBS_SECTION_PLAYING_CARDS, playing_count);
    if (state->hand_count > profile->hand) return observation_overflow(out, BALATRO_OBS_SECTION_HAND, state->hand_count);
    if (state->joker_count > profile->jokers) return observation_overflow(out, BALATRO_OBS_SECTION_JOKERS, state->joker_count);
    if (state->consumable_count > profile->consumables)
        return observation_overflow(out, BALATRO_OBS_SECTION_CONSUMABLES, state->consumable_count);
    if (state->pack_count > BALATRO_OBS_MAX_PACK_CARDS)
        return observation_overflow(out, BALATRO_OBS_SECTION_PACK_CARDS, state->pack_count);

    uint8_t shop_main_count = 0, shop_voucher_count = 0, shop_booster_count = 0;
    for (uint8_t i = 0; i < state->shop_count; ++i) {
        uint8_t set = balatro_card_set(&state->shop_cards[i]);
        if (set == SET_VOUCHER)
            shop_voucher_count++;
        else if (set == SET_BOOSTER)
            shop_booster_count++;
        else
            shop_main_count++;
    }
    if (shop_main_count > BALATRO_OBS_MAX_SHOP_MAIN)
        return observation_overflow(out, BALATRO_OBS_SECTION_SHOP_MAIN, shop_main_count);
    if (shop_voucher_count > profile->shop_vouchers)
        return observation_overflow(out, BALATRO_OBS_SECTION_SHOP_VOUCHERS, shop_voucher_count);
    if (shop_booster_count > BALATRO_OBS_MAX_SHOP_BOOSTERS)
        return observation_overflow(out, BALATRO_OBS_SECTION_SHOP_BOOSTERS, shop_booster_count);

    /* append_observation_variant initializes every live entry.  Clearing the
       full maximum-sized workspace here used to write about 8 KiB per call. */
    ObservationVariantBuild variants[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint16_t variant_count = 0;
    for (uint16_t i = 0; i < state->deck_count; ++i) {
        append_observation_variant(variants, &variant_count, &state->deck[i], 0);
        deck_summary_add(&out->draw_pile, &state->deck[i]);
    }
    out->owned_deck = out->draw_pile;
    for (uint8_t i = 0; i < state->hand_count; ++i) {
        append_observation_variant(variants, &variant_count, &state->hand[i], 1);
        deck_summary_add(&out->owned_deck, &state->hand[i]);
    }
    for (uint16_t i = 0; i < state->discard_count; ++i) {
        append_observation_variant(variants, &variant_count, &state->discard[i], 2);
        deck_summary_add(&out->owned_deck, &state->discard[i]);
    }
    variant_count = merge_observation_variants(variants, variant_count);
    if (variant_count > profile->playing_variants)
        return observation_overflow(out, BALATRO_OBS_SECTION_PLAYING_VARIANTS, variant_count);
    out->variants.count = variant_count;
    for (uint16_t i = 0; i < variant_count; ++i) {
        out->variants.rank[i] = variants[i].rank;
        out->variants.suit[i] = variants[i].suit;
        out->variants.enhancement[i] = variants[i].enhancement;
        out->variants.edition[i] = variants[i].edition;
        out->variants.seal[i] = variants[i].seal;
        out->variants.flags[i] = variants[i].flags;
        out->variants.perma_bonus[i] = balatro_signed_log2(variants[i].perma_bonus);
        out->variants.owned_count[i] = variants[i].owned_count;
        out->variants.draw_count[i] = variants[i].draw_count;
        out->variants.hand_count[i] = variants[i].hand_count;
        out->variants.discard_count[i] = variants[i].discard_count;
        out->variants.valid[i] = 1;
    }
    out->hand.count = state->hand_count;
    for (uint8_t i = 0; i < state->hand_count; ++i) {
        out->hand.variant[i] = find_observation_variant(variants, variant_count, &state->hand[i]);
        out->hand.flags[i] = public_playing_flags(&state->hand[i]);
        out->hand.valid[i] = 1;
    }
    out->jokers.count = state->joker_count;
    for (uint8_t i = 0; i < state->joker_count; ++i) OBSERVE_TABLE_CARD(out->jokers, i, &state->jokers[i], 0);
    out->consumables.count = state->consumable_count;
    for (uint8_t i = 0; i < state->consumable_count; ++i) OBSERVE_TABLE_CARD(out->consumables, i, &state->consumables[i], 0);
    for (uint8_t i = 0; i < state->shop_count; ++i) {
        const BalatroCard *card = &state->shop_cards[i];
        uint8_t set = balatro_card_set(card);
        if (set == SET_VOUCHER) {
            uint16_t slot = out->shop_vouchers.count++;
            OBSERVE_TABLE_CARD(out->shop_vouchers, slot, card, 0);
        } else if (set == SET_BOOSTER) {
            uint16_t slot = out->shop_boosters.count++;
            OBSERVE_TABLE_CARD(out->shop_boosters, slot, card, 0);
        } else {
            uint16_t slot = out->shop.count++;
            OBSERVE_TABLE_CARD(out->shop, slot, card, set == SET_DEFAULT || set == SET_ENHANCED);
        }
    }
    out->pack.count = state->pack_count;
    for (uint8_t i = 0; i < state->pack_count; ++i) {
        uint8_t set = balatro_card_set(&state->pack_cards[i]);
        OBSERVE_TABLE_CARD(out->pack, i, &state->pack_cards[i], set == SET_DEFAULT || set == SET_ENHANCED);
    }

    uint16_t tag_count = (uint16_t)(state->double_tag != 0) + (state->blind_tags[0] != BALATRO_TAG_NONE) +
                         (state->blind_tags[1] != BALATRO_TAG_NONE);
    if (tag_count > profile->tags) return observation_overflow(out, BALATRO_OBS_SECTION_TAGS, tag_count);
    if (state->double_tag) {
        uint16_t slot = out->tags.count++;
        out->tags.tag_id[slot] = BALATRO_TAG_TAG_DOUBLE;
        out->tags.flags[slot] = 1;
        out->tags.valid[slot] = 1;
    }
    for (uint8_t blind = 0; blind < 2; ++blind)
        if (state->blind_tags[blind] != BALATRO_TAG_NONE) {
            uint16_t slot = out->tags.count++;
            out->tags.tag_id[slot] = state->blind_tags[blind];
            out->tags.orbital_hand[slot] = state->orbital_hands[blind];
            out->tags.flags[slot] = (uint8_t)(1u << (blind + 1));
            out->tags.valid[slot] = 1;
        }

    static const uint16_t base_chips[BALATRO_HAND_COUNT] = {160, 140, 120, 100, 60, 40, 35, 30, 30, 20, 10, 5};
    static const uint8_t base_mult[BALATRO_HAND_COUNT] = {16, 14, 12, 8, 7, 4, 4, 4, 3, 2, 2, 1};
    static const uint8_t level_chips[BALATRO_HAND_COUNT] = {50, 40, 35, 40, 30, 25, 15, 30, 20, 20, 15, 10};
    static const uint8_t level_mult[BALATRO_HAND_COUNT] = {3, 4, 3, 4, 3, 2, 2, 3, 2, 1, 1, 1};
    static const float base_chips_log[BALATRO_HAND_COUNT] = {7.33091688f, 7.13955116f, 6.9188633f,  6.65821171f,
                                                             5.9307375f,  5.35755205f, 5.16992521f, 4.95419645f,
                                                             4.95419645f, 4.3923173f,  3.45943165f, 2.58496261f};
    static const float base_mult_log[BALATRO_HAND_COUNT] = {4.0874629f,  3.90689063f, 3.70043969f, 3.16992497f,
                                                            3.0f,        2.32192802f, 2.32192802f, 2.32192802f,
                                                            2.0f,        1.58496249f, 1.58496249f, 1.0f};
    for (uint8_t hand = 0; hand < BALATRO_HAND_COUNT; ++hand) {
        uint32_t level = state->hand_levels[hand] ? state->hand_levels[hand] : 1;
        out->poker_hands.visible[hand] = (uint8_t)hand_visible(state, hand);
        out->poker_hands.level[hand] = level;
        out->poker_hands.chips[hand] =
            level == 1 ? base_chips_log[hand] : balatro_signed_log2(base_chips[hand] + (double)level_chips[hand] * (level - 1));
        out->poker_hands.mult[hand] =
            level == 1 ? base_mult_log[hand] : balatro_signed_log2(base_mult[hand] + (double)level_mult[hand] * (level - 1));
        out->poker_hands.total_plays[hand] = state->hand_plays[hand];
        out->poker_hands.round_plays[hand] = state->hand_plays_round[hand];
    }

    BalatroObservationScalars *scalars = &out->scalars;
    scalars->deck_id = state->config.deck;
    scalars->blind_id = state->blind_id;
    scalars->next_boss_id = state->next_boss_id;
    /* The next voucher is sampled before its shop is entered, but that RNG
       result is not public until the voucher card is actually visible. */
    for (uint8_t i = 0; i < state->shop_count; ++i)
        if (state->shop_cards[i].center_id == state->next_voucher_id && balatro_card_set(&state->shop_cards[i]) == SET_VOUCHER) {
            scalars->next_voucher_id = state->next_voucher_id;
            break;
        }
    scalars->last_tarot_planet = state->last_tarot_planet;
    scalars->stake = state->config.stake;
    scalars->phase = state->phase;
    scalars->blind_on_deck = state->blind_on_deck;
    scalars->won = state->won;
    scalars->terminal = state->terminal;
    scalars->blind_disabled = state->blind_disabled;
    scalars->hand_sort_suit = state->hand_sort_suit;
    scalars->most_played_hand = state->most_played_hand;
    scalars->last_hand_type = state->last_hand_type;
    scalars->blind_skipped_mask = state->blind_skipped_mask;
    scalars->blind_only_hand = state->blind_only_hand;
    scalars->boss_rerolled = state->boss_rerolled;
    scalars->free_rerolls = state->free_rerolls;
    scalars->reroll_base = state->reroll_base;
    scalars->reroll_increase = state->reroll_increase;
    scalars->discount_percent = state->discount_percent;
    scalars->hands_per_round = state->hands_per_round;
    scalars->discards_per_round = state->discards_per_round;
    scalars->base_hand_size = state->base_hand_size;
    scalars->pack_kind = state->pack_kind;
    scalars->double_tag = state->double_tag;
    scalars->active_tag = state->active_tag;
    scalars->tag_hand_bonus = state->tag_hand_bonus;
    scalars->tag_force_rarity = state->tag_force_rarity;
    scalars->tag_force_rarity_count = state->tag_force_rarity_count;
    scalars->tag_force_edition = state->tag_force_edition;
    scalars->tag_force_edition_count = state->tag_force_edition_count;
    scalars->tag_voucher_pending = state->tag_voucher_pending;
    scalars->tag_coupon_pending = state->tag_coupon_pending;
    scalars->tag_coupon_active = state->tag_coupon_active;
    scalars->tag_investment_pending = state->tag_investment_pending;
    scalars->tag_d_six_pending = state->tag_d_six_pending;
    scalars->tag_d_six_active = state->tag_d_six_active;
    scalars->ecto_penalty = state->ecto_penalty;
    scalars->gros_michel_extinct = state->gros_michel_extinct;
    scalars->ante = state->ante;
    scalars->round = state->round;
    scalars->actions_taken = state->actions_taken;
    scalars->run_hands_played = state->run_hands_played;
    scalars->hands_left = state->hands_left;
    scalars->discards_left = state->discards_left;
    scalars->hands_played = state->hands_played;
    scalars->discards_used = state->discards_used;
    scalars->hand_size = state->hand_size;
    scalars->joker_slots = state->joker_slots;
    scalars->consumable_slots = state->consumable_slots;
    scalars->skips = state->skips;
    scalars->pack_choices = state->pack_choices;
    scalars->unused_discards = state->unused_discards;
    scalars->blind_hands_mask = state->blind_hands_mask;
    scalars->tarots_used = state->tarots_used;
    scalars->planet_usage_mask = state->planet_usage_mask;
    double chips_over_blind = state->blind_chips ? state->chips / state->blind_chips : 0.0;
    scalars->chips_number = number_class(state->chips);
    scalars->blind_chips_number = number_class(state->blind_chips);
    scalars->last_hand_score_number = number_class(state->last_hand_score);
    scalars->chips_over_blind_number = number_class(chips_over_blind);
    scalars->dollars = balatro_signed_log2(state->dollars);
    scalars->chips = balatro_signed_log2(state->chips);
    scalars->blind_chips = balatro_signed_log2(state->blind_chips);
    scalars->last_hand_score = balatro_signed_log2(state->last_hand_score);
    scalars->chips_over_blind = balatro_signed_log2(chips_over_blind);
    scalars->reroll_cost = balatro_signed_log2(state->reroll_cost);
    scalars->round_earnings = balatro_signed_log2(state->round_earnings);
    scalars->interest_cap = balatro_signed_log2(state->interest_cap);
    scalars->interest_amount = balatro_signed_log2(state->interest_amount);
    scalars->blind_reward = balatro_signed_log2(state->blind_reward);
    scalars->joker_rate = state->joker_rate;
    scalars->tarot_rate = state->tarot_rate;
    scalars->planet_rate = state->planet_rate;
    scalars->spectral_rate = state->spectral_rate;
    scalars->playing_card_rate = state->playing_card_rate;
    scalars->edition_rate = state->edition_rate;
    size_t voucher_count = 0;
    const uint16_t *vouchers = balatro_center_pool(SET_VOUCHER, &voucher_count);
    for (size_t i = 0; i < voucher_count; ++i) {
        uint16_t id = vouchers[i];
        if ((state->used_centers[id / 8] >> (id % 8)) & 1u) scalars->redeemed_vouchers[id] = 1;
    }
    return observe_legality(state, out);
}

int balatro_observe_batch(const BalatroState *states, size_t count, BalatroObservation *observations, int8_t *status) {
    if ((!states && count) || (!observations && count)) return BALATRO_ERR_ARGUMENT;
    for (size_t i = 0; i < count; ++i) {
        int error = balatro_observe(&states[i], &observations[i]);
        if (status)
            status[i] = (int8_t)error;
        else if (error)
            return error;
    }
    return BALATRO_OK;
}

#undef OBSERVE_TABLE_CARD

static int translate_policy_action(const BalatroState *state, const BalatroPolicyAction *policy, BalatroAction *out) {
    if (!state || !policy || !out || policy->type >= BALATRO_ACTION_TYPE_COUNT || policy->selection_count > BALATRO_MAX_SELECTION)
        return BALATRO_ERR_ARGUMENT;
    *out = (BalatroAction){.type = policy->type, .selection_count = policy->selection_count};
    int primary = policy->primary;
    if (policy->type == BALATRO_ACTION_BUY_CARD || policy->type == BALATRO_ACTION_BUY_AND_USE)
        primary = native_shop_index(state, policy->primary, SET_DEFAULT);
    else if (policy->type == BALATRO_ACTION_REDEEM_VOUCHER)
        primary = native_shop_index(state, policy->primary, SET_VOUCHER);
    else if (policy->type == BALATRO_ACTION_OPEN_BOOSTER)
        primary = native_shop_index(state, policy->primary, SET_BOOSTER);
    if (primary < 0 || primary > UINT8_MAX) return BALATRO_ERR_ACTION;
    out->primary = (uint8_t)primary;
    for (uint8_t i = 0; i < policy->selection_count; ++i) {
        if (policy->selection[i] >= state->hand_count || policy->selection[i] > UINT8_MAX) return BALATRO_ERR_ACTION;
        out->selection[i] = (uint8_t)policy->selection[i];
    }
    return BALATRO_OK;
}

int balatro_action_from_observation(const BalatroState *state, const BalatroPolicyAction *policy, BalatroAction *out) {
    int error = translate_policy_action(state, policy, out);
    if (error) return error;
    return action_is_legal(state, out) ? BALATRO_OK : BALATRO_ERR_ACTION;
}

static int step_observe_impl(BalatroState *state, const BalatroPolicyAction *action, BalatroStepResult *result,
                             BalatroObservation *observation, int trusted) {
    if (!state || !action || !result || !observation) return BALATRO_ERR_ARGUMENT;
    BalatroAction native;
    int error = translate_policy_action(state, action, &native);
    if (error) return error;
    error = balatro_step_impl(state, &native, result, trusted);
    if (error) return error;
    error = balatro_observe(state, observation);
    if (error == BALATRO_ERR_OBSERVATION_CAPACITY) {
        result->truncated = 1;
        result->truncation_reason = BALATRO_TRUNCATED_OBSERVATION_CAPACITY;
        return BALATRO_OK;
    }
    return error;
}

int balatro_step_observe(BalatroState *state, const BalatroPolicyAction *action, BalatroStepResult *result, BalatroObservation *observation) {
    return step_observe_impl(state, action, result, observation, 0);
}

int balatro_step_observe_trusted(BalatroState *state, const BalatroPolicyAction *action, BalatroStepResult *result,
                                 BalatroObservation *observation) {
    return step_observe_impl(state, action, result, observation, 1);
}

int balatro_step_observe_batch(BalatroState *states, const BalatroPolicyAction *actions, size_t count, BalatroStepResult *results,
                               BalatroObservation *observations, int8_t *status) {
    if ((!states && count) || (!actions && count) || (!results && count) || (!observations && count)) return BALATRO_ERR_ARGUMENT;
    for (size_t i = 0; i < count; ++i) {
        int error = balatro_step_observe(&states[i], &actions[i], &results[i], &observations[i]);
        if (status)
            status[i] = (int8_t)error;
        else if (error)
            return error;
    }
    return BALATRO_OK;
}

int balatro_step_observe_batch_trusted(BalatroState *states, const BalatroPolicyAction *actions, size_t count, BalatroStepResult *results,
                                       BalatroObservation *observations, int8_t *status) {
    if ((!states && count) || (!actions && count) || (!results && count) || (!observations && count)) return BALATRO_ERR_ARGUMENT;
    for (size_t i = 0; i < count; ++i) {
        int error = step_observe_impl(&states[i], &actions[i], &results[i], &observations[i], 1);
        if (status)
            status[i] = (int8_t)error;
        else if (error)
            return error;
    }
    return BALATRO_OK;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t length) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t balatro_state_hash(const BalatroState *state) {
    if (!state_layout_valid(state)) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_bytes(hash, state, offsetof(BalatroState, rng));
    hash = hash_bytes(hash, state->rng, (size_t)state->rng_count * sizeof(state->rng[0]));
    hash = hash_bytes(hash, &state->rng_count, offsetof(BalatroState, deck) - offsetof(BalatroState, rng_count));
    hash = hash_bytes(hash, state->deck, (size_t)state->deck_count * sizeof(state->deck[0]));
    hash = hash_bytes(hash, state->hand, (size_t)state->hand_count * sizeof(state->hand[0]));
    hash = hash_bytes(hash, state->discard, (size_t)state->discard_count * sizeof(state->discard[0]));
    hash = hash_bytes(hash, state->jokers, (size_t)state->joker_count * sizeof(state->jokers[0]));
    hash = hash_bytes(hash, state->consumables, (size_t)state->consumable_count * sizeof(state->consumables[0]));
    hash = hash_bytes(hash, state->shop_cards, (size_t)state->shop_count * sizeof(state->shop_cards[0]));
    hash = hash_bytes(hash, state->pack_cards, (size_t)state->pack_count * sizeof(state->pack_cards[0]));
    return hash_bytes(hash, &state->deck_count, sizeof(*state) - offsetof(BalatroState, deck_count));
}

typedef struct BalatroSnapshotHeader {
    uint8_t magic[8];
    uint32_t state_size;
    uint64_t state_hash;
} BalatroSnapshotHeader;

size_t balatro_serialize(const BalatroState *state, void *out, size_t capacity) {
    if (!state_layout_valid(state)) return 0;
    size_t required = sizeof(BalatroSnapshotHeader) + sizeof(*state);
    if (!out) return required;
    if (capacity < required) return 0;
    BalatroState canonical = {0};
    balatro_clone_state(&canonical, state);
    BalatroSnapshotHeader header = {
        .magic = {'B', 'A', 'L', 'A', 'T', 'R', 'O', '\0'},
        .state_size = sizeof(*state),
        .state_hash = balatro_state_hash(&canonical),
    };
    memcpy(out, &header, sizeof(header));
    memcpy((uint8_t *)out + sizeof(header), &canonical, sizeof(canonical));
    return required;
}

int balatro_deserialize(BalatroState *state, const void *data, size_t length) {
    if (!state || !data) return BALATRO_ERR_ARGUMENT;
    if (length != sizeof(BalatroSnapshotHeader) + sizeof(*state)) return BALATRO_ERR_SNAPSHOT;
    BalatroSnapshotHeader header;
    memcpy(&header, data, sizeof(header));
    static const uint8_t magic[8] = {'B', 'A', 'L', 'A', 'T', 'R', 'O', '\0'};
    if (memcmp(header.magic, magic, sizeof(magic)) || header.state_size != sizeof(*state))
        return BALATRO_ERR_SNAPSHOT;
    BalatroState copy;
    memcpy(&copy, (const uint8_t *)data + sizeof(header), sizeof(copy));
    if (!state_layout_valid(&copy)) return BALATRO_ERR_SNAPSHOT;
    if (balatro_state_hash(&copy) != header.state_hash) return BALATRO_ERR_SNAPSHOT;
    *state = copy;
    return BALATRO_OK;
}
