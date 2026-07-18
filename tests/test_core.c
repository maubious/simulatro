#include "balatro.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(BalatroAction) == 8, "actions must stay compact");

/* Internal card factory/RNG entry points used by source-stream regressions. */
BalatroCard balatro_create_pooled_card(BalatroState *state, uint8_t set, const char *append, int pack_area);
double balatro_pseudorandom(BalatroState *state, const char *key);
double balatro_round_decimal13(double value);
void balatro_price_card(const BalatroState *state, BalatroCard *card);
void balatro_initialize_joker_card(BalatroState *state, BalatroCard *card);

static BalatroCard card(uint8_t suit, uint8_t rank) {
    return (BalatroCard){.suit = suit, .rank = rank};
}

static int legal_actions(const BalatroState *state, BalatroAction *out, size_t capacity) {
    if (!state || (capacity && !out)) return BALATRO_ERR_ARGUMENT;
    BalatroLegalView view;
    int error = balatro_legal_view(state, &view);
    if (error) return error;
    uint32_t written = 0;
    for (uint16_t i = 0; i < view.group_count; ++i) {
        uint32_t count = balatro_legal_group_count(&view.groups[i]);
        size_t room = written < capacity ? capacity - written : 0;
        size_t group_capacity = count < room ? count : room;
        error = balatro_legal_group_actions(&view.groups[i], group_capacity ? &out[written] : NULL, group_capacity);
        if (error < 0) return error;
        written += count;
    }
    return (int)written;
}

static void assert_legal_view_matches_flat(const BalatroState *state) {
    BalatroAction flat[BALATRO_MAX_LEGAL_ACTIONS];
    uint8_t matched[BALATRO_MAX_LEGAL_ACTIONS] = {0};
    int flat_count = legal_actions(state, flat, BALATRO_MAX_LEGAL_ACTIONS);
    assert(flat_count >= 0 && flat_count <= BALATRO_MAX_LEGAL_ACTIONS);
    BalatroLegalView view;
    assert(balatro_legal_view(state, &view) == BALATRO_OK);
    assert(view.action_count == (uint32_t)flat_count);
    uint32_t decoded = 0;
    for (uint16_t group_index = 0; group_index < view.group_count; ++group_index) {
        const BalatroLegalGroup *group = &view.groups[group_index];
        uint32_t group_count = balatro_legal_group_count(group);
        for (uint32_t ordinal = 0; ordinal < group_count; ++ordinal) {
            BalatroAction action;
            assert(balatro_legal_group_action(group, ordinal, &action) == BALATRO_OK);
            int found = -1;
            for (int i = 0; i < flat_count; ++i)
                if (!matched[i] && !memcmp(&action, &flat[i], sizeof(action))) {
                    found = i;
                    break;
                }
            assert(found >= 0);
            matched[found] = 1;
            decoded++;
        }
    }
    assert(decoded == (uint32_t)flat_count);
}

static double reference_pseudohash(const char *text) {
    size_t length = strlen(text);
    double number = 1.0;
    for (size_t i = length; i > 0; --i)
        number = fmod((1.1239285023 / number) * (uint8_t)text[i - 1] * 3.14159265358979323846 + 3.14159265358979323846 * (double)i, 1.0);
    return number;
}

static double reference_round_decimal13(double value) {
    char text[32];
    (void)snprintf(text, sizeof(text), "%.13f", value);
    return fabs(strtod(text, NULL));
}

static uint64_t double_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void test_exact_rng_fast_paths(void) {
    uint64_t random = UINT64_C(0x9e3779b97f4a7c15);
    char text[48];
    for (unsigned sample = 0; sample < 1000000; ++sample) {
        random ^= random << 13;
        random ^= random >> 7;
        random ^= random << 17;
        double value = (double)(random >> 11) * (1.0 / 9007199254740992.0);
        assert(double_bits(reference_round_decimal13(value)) == double_bits(balatro_round_decimal13(value)));
        if (sample < 100000) {
            unsigned length = 1 + (unsigned)(random % 40);
            for (unsigned i = 0; i < length; ++i) {
                random ^= random << 13;
                random ^= random >> 7;
                random ^= random << 17;
                text[i] = (char)('!' + random % 90);
            }
            text[length] = '\0';
            assert(double_bits(reference_pseudohash(text)) == double_bits(balatro_pseudohash(text)));
        }
    }
    for (uint64_t decimal = 0; decimal < 100000; decimal += 97) {
        double halfway = ((double)decimal + 0.5) / 10000000000000.0;
        double values[3] = {
            nextafter(halfway, 0.0),
            halfway,
            nextafter(halfway, 1.0),
        };
        for (size_t i = 0; i < 3; ++i)
            assert(double_bits(reference_round_decimal13(values[i])) == double_bits(balatro_round_decimal13(values[i])));
    }
}

static void test_poker(void) {
    uint8_t mask;
    BalatroCard royal[5] = {card(0, 10), card(0, 11), card(0, 12), card(0, 13), card(0, 14)};
    assert(balatro_classify_hand(royal, 5, &mask) == BALATRO_STRAIGHT_FLUSH);
    assert(mask == 31);
    BalatroCard house[5] = {card(0, 14), card(1, 14), card(2, 14), card(0, 13), card(1, 13)};
    assert(balatro_classify_hand(house, 5, &mask) == BALATRO_FULL_HOUSE);
    BalatroCard wheel[5] = {card(0, 14), card(1, 2), card(2, 3), card(0, 4), card(1, 5)};
    assert(balatro_classify_hand(wheel, 5, &mask) == BALATRO_STRAIGHT);
}

static void test_state(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 42) == BALATRO_OK);
    assert(state.deck_count == 52);
    assert(state.phase == BALATRO_PHASE_BLIND_SELECT);
    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    assert(count == 2);
    BalatroStepResult result;
    assert(balatro_step(&state, &actions[0], &result) == BALATRO_OK);
    assert(state.hand_count == 8);
    assert(state.deck_count == 44);
    uint8_t snapshot[sizeof(BalatroState) + 64];
    size_t snapshot_size = balatro_serialize(&state, snapshot, sizeof(snapshot));
    assert(snapshot_size > sizeof(state));
    uint64_t hash = balatro_state_hash(&state);
    memset(&state, 0, sizeof(state));
    assert(balatro_deserialize(&state, snapshot, snapshot_size) == BALATRO_OK);
    assert(hash == balatro_state_hash(&state));
}

static void test_legal_view(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "LEGAL_VIEW") == BALATRO_OK);
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.hand_count == 8);

    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    BalatroAction prefix[17];
    int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    assert(count == 452);
    assert(legal_actions(&state, NULL, 0) == count);
    assert(legal_actions(&state, prefix, 17) == count);
    assert(!memcmp(prefix, actions, sizeof(prefix)));

    BalatroLegalView view;
    assert(balatro_legal_view(&state, &view) == BALATRO_OK);
    assert(view.action_count == (uint32_t)count && view.group_count == 18);
    BalatroAction materialized[452];
    uint32_t materialized_count = 0;
    for (uint16_t group_index = 0; group_index < view.group_count; ++group_index) {
        const BalatroLegalGroup *group = &view.groups[group_index];
        uint32_t group_count = balatro_legal_group_count(group);
        for (uint32_t ordinal = 0; ordinal < group_count; ++ordinal) {
            assert(materialized_count < 452);
            assert(balatro_legal_group_action(group, ordinal, &materialized[materialized_count++]) == BALATRO_OK);
        }
    }
    assert(materialized_count == (uint32_t)count);
    assert(!memcmp(materialized, actions, sizeof(materialized)));
    assert_legal_view_matches_flat(&state);

    for (int i = 0; i < count; ++i) {
        BalatroState checked = {0}, trusted = {0};
        balatro_clone_state(&checked, &state);
        balatro_clone_state(&trusted, &state);
        BalatroStepResult checked_result, trusted_result;
        assert(balatro_step(&checked, &actions[i], &checked_result) == BALATRO_OK);
        assert(balatro_step_trusted(&trusted, &actions[i], &trusted_result) == BALATRO_OK);
        assert(balatro_state_hash(&checked) == balatro_state_hash(&trusted));
        assert(!memcmp(&checked_result, &trusted_result, sizeof(checked_result)));
    }

    uint64_t unchanged = balatro_state_hash(&state);
    BalatroAction invalid = {
        .type = BALATRO_ACTION_PLAY_HAND,
        .selection_count = 2,
        .selection = {0, 0},
    };
    assert(balatro_step(&state, &invalid, &result) == BALATRO_ERR_ACTION);
    assert(balatro_state_hash(&state) == unchanged);
    invalid.selection[0] = 2;
    invalid.selection[1] = 1;
    assert(balatro_step(&state, &invalid, &result) == BALATRO_ERR_ACTION);
    invalid = (BalatroAction){.type = BALATRO_ACTION_SORT_HAND_RANK, .primary = 1};
    assert(balatro_step(&state, &invalid, &result) == BALATRO_ERR_ACTION);

    BalatroState full_discard = state;
    full_discard.discard_count = BALATRO_MAX_DECK;
    count = legal_actions(&full_discard, actions, BALATRO_MAX_LEGAL_ACTIONS);
    for (int i = 0; i < count; ++i) assert(actions[i].type != BALATRO_ACTION_PLAY_HAND && actions[i].type != BALATRO_ACTION_DISCARD);

    state.hand_count = BALATRO_MAX_HAND;
    state.hands_left = 1;
    state.discards_left = 1;
    for (uint8_t i = 8; i < BALATRO_MAX_HAND; ++i) state.hand[i] = card((uint8_t)(i & 3), (uint8_t)(2 + i % 13));
    count = legal_actions(&state, NULL, 0);
    assert(count == 13800);
    assert(legal_actions(&state, prefix, 17) == count);
    state.consumable_count = BALATRO_MAX_CONSUMABLES;
    for (uint8_t i = 0; i < state.consumable_count; ++i) state.consumables[i].center_id = BALATRO_CENTER_C_WORLD;
    count = legal_actions(&state, NULL, 0);
    assert(count == 19376 && count < BALATRO_MAX_LEGAL_ACTIONS);
    assert(balatro_legal_view(&state, &view) == BALATRO_OK);
    assert(view.action_count == (uint32_t)count);
    uint32_t counted = 0;
    for (uint16_t i = 0; i < view.group_count; ++i) counted += balatro_legal_group_count(&view.groups[i]);
    assert(counted == view.action_count);

    state.hand_count = 8;
    state.hand[3].flags |= BALATRO_CARD_FORCED;
    state.hand[1].edition = BALATRO_EDITION_FOIL;
    state.consumable_count = 2;
    state.consumables[0].center_id = BALATRO_CENTER_C_DEATH;
    state.consumables[1].center_id = BALATRO_CENTER_C_AURA;
    assert_legal_view_matches_flat(&state);

    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    state.consumable_count = 0;
    state.joker_count = 0;
    balatro_populate_shop(&state);
    assert_legal_view_matches_flat(&state);

    state.phase = BALATRO_PHASE_PACK_OPENING;
    state.shop_count = 0;
    state.pack_count = 2;
    state.pack_choices = 1;
    state.pack_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_DEATH};
    state.pack_cards[1] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE};
    assert_legal_view_matches_flat(&state);
}

static void test_clone_hash_and_snapshots(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "CANONICAL") == BALATRO_OK);
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);

    BalatroState clone;
    memset(&clone, 0xa5, sizeof(clone));
    balatro_clone_state(&clone, &state);
    assert(balatro_state_hash(&clone) == balatro_state_hash(&state));
    clone.hand[BALATRO_MAX_HAND - 1].state[3] ^= 1234567;
    clone.deck[BALATRO_MAX_DECK - 1].rank ^= 7;
    clone.rng[BALATRO_MAX_RNG_STREAMS - 1].key_hash ^= UINT64_C(0x1234);
    assert(balatro_state_hash(&clone) == balatro_state_hash(&state));

    size_t snapshot_size = balatro_serialize(&state, NULL, 0);
    uint8_t snapshot[sizeof(BalatroState) + 64];
    uint8_t second[sizeof(BalatroState) + 64];
    assert(snapshot_size <= sizeof(snapshot));
    assert(balatro_serialize(&state, snapshot, sizeof(snapshot)) == snapshot_size);
    assert(balatro_serialize(&clone, second, sizeof(second)) == snapshot_size);
    assert(!memcmp(snapshot, second, snapshot_size));

    BalatroState restored;
    assert(balatro_deserialize(&restored, snapshot, snapshot_size) == BALATRO_OK);
    assert(balatro_state_hash(&restored) == balatro_state_hash(&state));

    BalatroState invalid = state;
    invalid.deck_count = BALATRO_MAX_DECK + 1;
    assert(balatro_state_hash(&invalid) == 0);
    assert(balatro_serialize(&invalid, snapshot, sizeof(snapshot)) == 0);
    assert(balatro_step(&invalid, &select, &(BalatroStepResult){0}) == BALATRO_ERR_INVARIANT);
}

static void test_validation_card_injection(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "DEBUG") == BALATRO_OK);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_JOKER, BALATRO_EDITION_NONE, 0) == BALATRO_ERR_ARGUMENT);

    config.validation = 1;
    assert(balatro_init_seed_string(&state, &config, "DEBUG") == BALATRO_OK);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_JOKER, BALATRO_EDITION_NONE, BALATRO_CARD_ETERNAL) == BALATRO_OK);
    assert(state.joker_count == 1);
    assert(state.jokers[0].center_id == BALATRO_CENTER_J_JOKER);
    assert(state.jokers[0].flags & BALATRO_CARD_ETERNAL);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_C_HERMIT, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    assert(state.consumable_count == 1);

    state.phase = BALATRO_PHASE_SELECTING_HAND;
    assert(balatro_debug_add_playing_card(&state, BALATRO_HEARTS, 14, BALATRO_ENHANCEMENT_BONUS, BALATRO_EDITION_FOIL, BALATRO_SEAL_RED) ==
           BALATRO_OK);
    assert(state.hand_count == 1);
    assert(state.hand[0].rank == 14);
    assert(state.hand[0].enhancement == BALATRO_ENHANCEMENT_BONUS);
    assert(state.hand[0].edition == BALATRO_EDITION_FOIL);
    assert(state.hand[0].seal == BALATRO_SEAL_RED);

    assert(balatro_init_seed_string(&state, &config, "DEBUG_PASSIVE") == BALATRO_OK);
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.hand_count == 8 && state.hand_size == 8 && state.deck_count == 44);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_JUGGLER, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    assert(state.hand_size == 9 && state.hand_count == 9 && state.deck_count == 43);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_MERRY_ANDY, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    assert(state.hand_size == 8 && state.discards_left == 7);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_DRUNKARD, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    assert(state.discards_left == 8);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_TROUBADOUR, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    assert(state.hand_size == 10 && state.hand_count == 10 && state.deck_count == 42);
}

static void test_source_wraith_factory_streams(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.validation = 1;
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "S_WRAITH") == BALATRO_OK);
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_C_WRAITH, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    state.dollars = 10;
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    /* create_card('Joker', ..., 0.99, ..., 'wra') uses Joker3wra1,
       then polls ediwra1. The fully-unlocked pinned profile selects Stuntman. */
    assert(state.joker_count == 1);
    assert(state.jokers[0].center_id == BALATRO_CENTER_J_STUNTMAN);
    assert(state.jokers[0].edition == BALATRO_EDITION_NONE);
    assert(state.dollars == 0);
}

static void test_source_riff_raff_factory_resample(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.validation = 1;
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "CLV00002") == BALATRO_OK);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_RIFF_RAFF, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.joker_count == 3);
    assert(state.jokers[1].center_id == BALATRO_CENTER_J_LUSTY_JOKER);
    assert(state.jokers[2].center_id == BALATRO_CENTER_J_MAD);
}

static void test_source_soul_legendary_stream(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.validation = 1;
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "CLV00005") == BALATRO_OK);
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_C_SOUL, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    /* Legendary get_current_pool omits the ante suffix: Joker4, not
       Joker4{ante}. The source/live selection for this seed is Chicot. */
    assert(state.joker_count == 1);
    assert(state.jokers[0].center_id == BALATRO_CENTER_J_CHICOT);
}

static void test_source_soul_pack_type_gates(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    char seed[32];
    int found_tarot_gate = 0, found_joker_gate = 0;

    for (unsigned i = 0; i < 10000 && (!found_tarot_gate || !found_joker_gate); ++i) {
        BalatroState state;
        (void)snprintf(seed, sizeof(seed), "SOUL_TYPE_%u", i);
        assert(balatro_init_seed_string(&state, &config, seed) == BALATRO_OK);
        if (!found_tarot_gate && balatro_pseudorandom(&state, "soul_Tarot1") > 0.997) {
            assert(balatro_init_seed_string(&state, &config, seed) == BALATRO_OK);
            BalatroCard tarot = balatro_create_pooled_card(&state, 4, "ar1", 1);
            assert(tarot.center_id == BALATRO_CENTER_C_SOUL);
            found_tarot_gate = 1;
        }
        assert(balatro_init_seed_string(&state, &config, seed) == BALATRO_OK);
        if (!found_joker_gate && balatro_pseudorandom(&state, "soul_Joker1") > 0.997) {
            assert(balatro_init_seed_string(&state, &config, seed) == BALATRO_OK);
            BalatroCard joker = balatro_create_pooled_card(&state, 3, "buf", 1);
            assert(joker.center_id != BALATRO_CENTER_C_SOUL);
            found_joker_gate = 1;
        }
    }
    assert(found_tarot_gate && found_joker_gate);
}

static void test_source_pack_overlay_joker_sale_and_cryptid(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_PACK_OPENING;
    state.shop_return_phase = BALATRO_PHASE_SHOP;
    state.pack_kind = 3;
    state.pack_count = state.pack_choices = 1;
    state.pack_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_CRYPTID};
    state.joker_count = 1;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_JOKER, .sell_cost = 2};

    BalatroAction legal[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(&state, legal, BALATRO_MAX_LEGAL_ACTIONS);
    int found_sell = 0;
    for (int i = 0; i < count; ++i)
        if (legal[i].type == BALATRO_ACTION_SELL_JOKER && legal[i].primary == 0) found_sell = 1;
    assert(found_sell);

    state.deck_count = 1;
    state.deck[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .sort_id = 1};
    state.hand_count = 1;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 7, .suit = BALATRO_SPADES, .sort_id = 10};
    state.next_sort_id = 10;
    BalatroAction pick = {.type = BALATRO_ACTION_PICK_PACK_CARD, .primary = 0, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &pick, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_SHOP && state.hand_count == 0);
    assert(state.deck_count == 4);
    assert(state.deck[0].sort_id == 12 && state.deck[1].sort_id == 11 && state.deck[2].sort_id == 10 && state.deck[3].sort_id == 1);
}

static void test_source_buy_and_use_bypasses_consumable_capacity(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 10;
    state.consumable_slots = state.consumable_count = 2;
    state.consumables[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_FOOL};
    state.consumables[1] = (BalatroCard){.center_id = BALATRO_CENTER_C_TOWER};
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_PLANET_X, .cost = 3, .sell_cost = 1};
    state.hand_levels[BALATRO_FIVE_OF_A_KIND] = 1;

    BalatroAction action = {.type = BALATRO_ACTION_BUY_AND_USE, .primary = 0};
    BalatroStepResult result;
    assert(balatro_step(&state, &action, &result) == BALATRO_OK);
    assert(state.dollars == 7 && state.shop_count == 0);
    assert(state.consumable_count == 2);
    assert(state.hand_levels[BALATRO_FIVE_OF_A_KIND] == 2);
}

static void test_source_suit_boss_stone_and_wild_debuffs(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_BLIND_SELECT;
    state.blind_on_deck = 2;
    state.next_boss_id = BALATRO_BLIND_BL_WINDOW;
    state.base_hand_size = state.hand_size = 3;
    state.deck_count = 3;
    state.deck[0] =
        (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_DIAMONDS, .enhancement = BALATRO_ENHANCEMENT_STONE};
    state.deck[1] =
        (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 8, .suit = BALATRO_CLUBS, .enhancement = BALATRO_ENHANCEMENT_WILD};
    state.deck[2] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 7, .suit = BALATRO_DIAMONDS};
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.hand_count == 3);
    for (uint8_t i = 0; i < state.hand_count; ++i) {
        if (state.hand[i].enhancement == BALATRO_ENHANCEMENT_STONE)
            assert(!(state.hand[i].flags & 1u));
        else
            assert(state.hand[i].flags & 1u);
    }
    uint8_t diamond = 0;
    while (diamond < state.hand_count &&
           (state.hand[diamond].enhancement != BALATRO_ENHANCEMENT_NONE || state.hand[diamond].suit != BALATRO_DIAMONDS))
        diamond++;
    assert(diamond < state.hand_count && (state.hand[diamond].flags & 1u));
    state.consumable_count = 1;
    state.consumables[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_TOWER};
    BalatroAction tower = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0, .selection_count = 1, .selection = {diamond}};
    assert(balatro_step(&state, &tower, &result) == BALATRO_OK);
    assert(state.hand[diamond].enhancement == BALATRO_ENHANCEMENT_STONE);
    assert(!(state.hand[diamond].flags & 1u));
}

static void test_source_grim_rolls_suit_without_fixed_rank_poll(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "GRIM_STREAM") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = state.hand_size = 3;
    for (uint8_t i = 0; i < state.hand_count; ++i)
        state.hand[i] =
            (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = (uint8_t)(7 + i), .suit = i, .sort_id = (uint16_t)(i + 1)};
    state.next_sort_id = 3;
    state.deck_count = state.discard_count = 0;
    state.consumable_count = 1;
    state.consumables[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_GRIM};

    BalatroState expected = state;
    (void)balatro_pseudorandom(&expected, "random_destroy");
    static const uint8_t suits[] = {
        BALATRO_SPADES,
        BALATRO_HEARTS,
        BALATRO_DIAMONDS,
        BALATRO_CLUBS,
    };
    uint8_t expected_suits[2];
    for (uint8_t i = 0; i < 2; ++i) {
        size_t pick = (size_t)floor(balatro_pseudorandom(&expected, "grim_create") * 4.0);
        expected_suits[i] = suits[pick < 4 ? pick : 3];
        (void)balatro_pseudorandom(&expected, "spe_card");
    }

    BalatroAction grim = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    BalatroStepResult result;
    assert(balatro_step(&state, &grim, &result) == BALATRO_OK);
    assert(state.hand_count == 4);
    for (uint8_t generated = 0; generated < 2; ++generated) {
        uint8_t index = 0;
        while (index < state.hand_count && state.hand[index].sort_id != 4 + generated) index++;
        assert(index < state.hand_count && state.hand[index].rank == 14);
        assert(state.hand[index].suit == expected_suits[generated]);
    }
}

static void test_source_hook_named_stream_targets(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "CLV00004") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.blind_id = BALATRO_BLIND_BL_HOOK;
    state.blind_chips = 100000;
    state.chips = 0;
    state.hands_left = 4;
    state.discards_left = 3;
    state.hand_size = state.hand_count = 8;
    state.deck_count = state.discard_count = 0;
    state.consumable_slots = 2;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_GREEN_JOKER;
    state.jokers[0].state[0] = 3;
    const BalatroCard hand[8] = {
        {.center_id = BALATRO_CENTER_C_BASE, .suit = BALATRO_SPADES, .rank = 14},
        {.center_id = BALATRO_CENTER_C_BASE, .suit = BALATRO_HEARTS, .rank = 14},
        {.center_id = BALATRO_CENTER_C_BASE, .suit = BALATRO_HEARTS, .rank = 13},
        {.center_id = BALATRO_CENTER_C_BASE, .suit = BALATRO_HEARTS, .rank = 12},
        {.center_id = BALATRO_CENTER_C_BASE, .suit = BALATRO_SPADES, .rank = 8},
        {.center_id = BALATRO_CENTER_C_BASE, .suit = BALATRO_HEARTS, .rank = 8},
        {.center_id = BALATRO_CENTER_C_BASE, .suit = BALATRO_HEARTS, .rank = 4},
        {.center_id = BALATRO_CENTER_C_BASE, .suit = BALATRO_HEARTS, .rank = 2, .seal = BALATRO_SEAL_PURPLE},
    };
    memcpy(state.hand, hand, sizeof(hand));
    BalatroAction play = {
        .type = BALATRO_ACTION_PLAY_HAND,
        .selection_count = 5,
        .selection = {1, 2, 3, 5, 6},
    };
    BalatroStepResult result;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    /* blind.lua copies the three unplayed cards, then two consecutive
       pseudoseed('hook') calls choose H_2 and S_8 for this seed. */
    assert(state.hand_count == 1);
    assert(state.hand[0].suit == BALATRO_SPADES && state.hand[0].rank == 14);
    /* Hook routes its forced cards through discard contexts without spending
       a discard: Purple Seal creates a Tarot, and Green loses one Mult before
       its before-scoring hook adds one back. */
    assert(state.discards_used == 0 && state.discards_left == 3);
    assert(state.consumable_count == 1);
    assert(state.jokers[0].state[0] == 3);
}

static void test_source_aura_target_and_edition(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "S_AURA") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 2;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
    state.hand[1] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 13, .suit = BALATRO_SPADES, .edition = BALATRO_EDITION_FOIL};
    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_AURA;
    BalatroAction illegal = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0, .selection_count = 1, .selection = {1}};
    BalatroStepResult result;
    assert(balatro_step(&state, &illegal, &result) == BALATRO_ERR_ACTION);
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.consumable_count == 0);
    assert(state.hand[0].edition >= BALATRO_EDITION_FOIL && state.hand[0].edition <= BALATRO_EDITION_POLYCHROME);
    assert(state.hand[1].edition == BALATRO_EDITION_FOIL);
}

static void test_shaped_reward_progress(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.shaped_reward = 1;
    BalatroState state;
    assert(balatro_init(&state, &config, 7) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 1;
    state.discards_left = 0;
    state.blind_chips = 1;
    state.chips = 0;
    state.deck_count = 0;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS, .sort_id = 1};
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    /* Crossing a blind boundary must produce a positive training signal even
       though the public sparse win reward remains zero. */
    assert(result.sparse_reward == 0.0f);
    assert(result.reward > 0.5f);
}

static void test_observation_layout(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    BalatroObservation observation;
    assert(balatro_init(&state, &config, 9) == BALATRO_OK);
    assert(balatro_observe(&state, &observation) == BALATRO_OK);
    assert(observation.encoded_bytes == sizeof(observation));
    assert(observation.truncation_reason == BALATRO_TRUNCATED_NONE);
    assert(observation.profile.playing_cards == BALATRO_OBS_MAX_PLAYING_CARDS);
    assert(observation.variants.count == 52);
    assert(observation.owned_deck.total == 52 && observation.draw_pile.total == 52);
    assert(observation.owned_deck.ace == 4 && observation.owned_deck.face == 12);
    assert(observation.hand.count == 0);
    assert(observation.tags.count == 2);
    assert(state.next_voucher_id != 0 && observation.scalars.next_voucher_id == 0);
    assert(observation.scalars.dollars == balatro_signed_log2(4));
    assert(observation.poker_hands.visible[BALATRO_HIGH_CARD]);
    assert(!observation.poker_hands.visible[BALATRO_FLUSH_FIVE]);
    assert(observation.legal.action_type[BALATRO_ACTION_SELECT_BLIND]);

    /* Hidden RNG values, creation identifiers, and shuffled draw order are
       deliberately absent from the public observation. */
    BalatroState hidden = state;
    for (uint16_t i = 0; i < hidden.deck_count / 2; ++i) {
        BalatroCard swap = hidden.deck[i];
        hidden.deck[i] = hidden.deck[hidden.deck_count - i - 1];
        hidden.deck[hidden.deck_count - i - 1] = swap;
    }
    hidden.rng[0].value += 0.125;
    hidden.deck[0].sort_id += 1000;
    BalatroObservation hidden_observation;
    assert(balatro_observe(&hidden, &hidden_observation) == BALATRO_OK);
    assert(memcmp(&observation, &hidden_observation, sizeof(observation)) == 0);

    /* Physical duplicates collapse only when their complete public tuple is
       equal; ordered hand slots retain references into that dictionary. */
    state.deck_count = 2;
    state.hand_count = 1;
    state.discard_count = 1;
    state.deck[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
    state.deck[1] = state.deck[0];
    state.hand[0] = state.deck[0];
    state.discard[0] = state.deck[0];
    state.discard[0].perma_bonus = 10;
    state.discard[0].enhancement = BALATRO_ENHANCEMENT_STONE;
    assert(balatro_observe(&state, &observation) == BALATRO_OK);
    assert(observation.variants.count == 2);
    uint16_t hand_variant = observation.hand.variant[0];
    assert(observation.variants.owned_count[hand_variant] == 3);
    assert(observation.variants.draw_count[hand_variant] == 2);
    assert(observation.variants.hand_count[hand_variant] == 1);
    assert(observation.variants.discard_count[hand_variant] == 0);
    assert(observation.owned_deck.stone == 1 && observation.owned_deck.ace == 3);
    state.dollars = 1000000000;
    assert(balatro_observe(&state, &observation) == BALATRO_OK);
    assert(observation.scalars.dollars > 20.0f);

    config.observation.playing_cards = 51;
    assert(balatro_init(&state, &config, 9) == BALATRO_OK);
    assert(balatro_observe(&state, &observation) == BALATRO_ERR_OBSERVATION_CAPACITY);
    assert(observation.truncation_reason == BALATRO_TRUNCATED_OBSERVATION_CAPACITY);
    assert(observation.overflow_section == BALATRO_OBS_SECTION_PLAYING_CARDS);
    assert(observation.required_capacity == 52);
}

static void test_observation_policy_actions(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "OBS_ACTION") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    state.shop_count = 3;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_V_BLANK, .cost = 10};
    state.shop_cards[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_JOKER, .cost = 2};
    state.shop_cards[2] = (BalatroCard){.center_id = BALATRO_CENTER_P_ARCANA_NORMAL_1, .cost = 4};
    BalatroObservation observation;
    assert(balatro_observe(&state, &observation) == BALATRO_OK);
    assert(observation.shop.count == 1 && observation.shop.center_id[0] == BALATRO_CENTER_J_JOKER);
    assert(observation.shop_vouchers.count == 1 && observation.shop_vouchers.center_id[0] == BALATRO_CENTER_V_BLANK);
    assert(observation.shop_boosters.count == 1 && observation.shop_boosters.center_id[0] == BALATRO_CENTER_P_ARCANA_NORMAL_1);
    assert(observation.legal.primary[BALATRO_ACTION_BUY_CARD] == 1);
    assert(observation.legal.primary[BALATRO_ACTION_REDEEM_VOUCHER] == 1);
    assert(observation.legal.primary[BALATRO_ACTION_OPEN_BOOSTER] == 1);

    BalatroPolicyAction policy = {.type = BALATRO_ACTION_BUY_CARD, .primary = 0};
    BalatroAction native;
    assert(balatro_action_from_observation(&state, &policy, &native) == BALATRO_OK);
    assert(native.primary == 1);

    assert(balatro_init_seed_string(&state, &config, "OBS_STEP") == BALATRO_OK);
    policy = (BalatroPolicyAction){.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step_observe(&state, &policy, &result, &observation) == BALATRO_OK);
    assert(!result.terminal && !result.truncated);
    assert(observation.scalars.phase == BALATRO_PHASE_SELECTING_HAND);
    assert(observation.hand.count == state.hand_count);
    assert(observation.legal.action_type[BALATRO_ACTION_PLAY_HAND]);

    BalatroState safe_state, trusted_state;
    assert(balatro_init_seed_string(&safe_state, &config, "OBS_TRUSTED") == BALATRO_OK);
    trusted_state = safe_state;
    BalatroStepResult safe_result, trusted_result;
    BalatroObservation safe_observation, trusted_observation;
    assert(balatro_step_observe(&safe_state, &policy, &safe_result, &safe_observation) == BALATRO_OK);
    assert(balatro_step_observe_trusted(&trusted_state, &policy, &trusted_result, &trusted_observation) == BALATRO_OK);
    assert(balatro_state_hash(&safe_state) == balatro_state_hash(&trusted_state));
    assert(memcmp(&safe_result, &trusted_result, sizeof(safe_result)) == 0);
    assert(memcmp(&safe_observation, &trusted_observation, sizeof(safe_observation)) == 0);

    config.observation.hand = 1;
    assert(balatro_init_seed_string(&state, &config, "OBS_TRUNCATE") == BALATRO_OK);
    policy = (BalatroPolicyAction){.type = BALATRO_ACTION_SELECT_BLIND};
    assert(balatro_step_observe(&state, &policy, &result, &observation) == BALATRO_OK);
    assert(result.truncated && result.truncation_reason == BALATRO_TRUNCATED_OBSERVATION_CAPACITY);
    assert(observation.overflow_section == BALATRO_OBS_SECTION_HAND);

    balatro_default_config(&config);
    BalatroState states[2];
    assert(balatro_init_seed_string(&states[0], &config, "OBS_BATCH_A") == BALATRO_OK);
    assert(balatro_init_seed_string(&states[1], &config, "OBS_BATCH_B") == BALATRO_OK);
    BalatroPolicyAction policies[2] = {{.type = BALATRO_ACTION_SELECT_BLIND}, {.type = BALATRO_ACTION_SELECT_BLIND}};
    BalatroStepResult results[2];
    BalatroObservation observations[2];
    int8_t status[2];
    assert(balatro_step_observe_batch(states, policies, 2, results, observations, status) == BALATRO_OK);
    assert(status[0] == BALATRO_OK && status[1] == BALATRO_OK);
    assert(observations[0].hand.count == 8 && observations[1].hand.count == 8);
}

static void test_rng(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "TESTSEED") == BALATRO_OK);
    const uint8_t expected_deck[52] = {
        17, 21, 14, 36, 29, 31, 35, 41, 43, 34, 44, 4,  49, 50, 32, 26, 10, 42, 12, 27, 6, 19, 48, 52, 51, 38,
        8,  20, 11, 1,  30, 37, 13, 25, 47, 22, 18, 24, 16, 2,  40, 39, 15, 28, 5,  45, 7, 3,  33, 9,  23, 46,
    };
    for (size_t i = 0; i < 52; ++i) assert(state.deck[i].sort_id == expected_deck[i]);
    /* Source start_run consumes the first boss draw before the public state
       is exposed; these are the second and third stream values. */
    assert(fabs(balatro_pseudorandom(&state, "boss") - 0.7843391176293266) < 1e-15);
    assert(fabs(balatro_pseudorandom(&state, "boss") - 0.3362779203407824) < 1e-15);
    /* reset advances the named stream once, then takes 51 draws from TW223 */
    assert(fabs(balatro_pseudorandom(&state, "shuffle") - 0.8121214036707356) < 1e-15);
    assert(fabs(balatro_pseudorandom(&state, "lucky_mult") - 0.32170301340597174) < 1e-15);

    BalatroState tag_state;
    assert(balatro_init_seed_string(&tag_state, &config, "TAG_0") == BALATRO_OK);
    assert(tag_state.blind_tags[0] == BALATRO_TAG_TAG_BOSS);
    assert(tag_state.blind_tags[1] == BALATRO_TAG_TAG_POLYCHROME);
    assert(balatro_init_seed_string(&tag_state, &config, "TAG_13") == BALATRO_OK);
    assert(tag_state.blind_tags[0] == BALATRO_TAG_TAG_D_SIX);
    assert(tag_state.blind_tags[1] == BALATRO_TAG_TAG_BOSS);
}

static void test_blind_scaling_and_economy(void) {
    assert(balatro_blind_amount(1, 1) == 300);
    assert(balatro_blind_amount(8, 1) == 50000);
    assert(balatro_blind_amount(8, 3) == 200000);
    assert(balatro_blind_target(2, 1, 1) == 1200);
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.dollars = 24;
    state.hands_left = 2;
    state.blind_reward = 4;
    assert(balatro_calculate_round_earnings(&state) == 10); /* $4 blind + $2 hands + $4 interest */
    config.deck = BALATRO_CENTER_B_GREEN;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.dollars = 24;
    state.hands_left = 2;
    state.discards_left = 3;
    state.blind_reward = 4;
    assert(balatro_calculate_round_earnings(&state) == 11); /* $4 + $4 hands + $3 discards; no interest */
}

static void test_shop_lifecycle(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "TESTSEED") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    balatro_populate_shop(&state);
    assert(state.shop_count == 5);

    /* A sold Joker becomes eligible for its pool again once the final live
       copy is removed. */
    config.validation = 1;
    assert(balatro_init_seed_string(&state, &config, "SELL_POOL_CLEAR") == BALATRO_OK);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_SPLASH, 0, 0) == BALATRO_OK);
    assert(state.used_centers[BALATRO_CENTER_J_SPLASH / 8] & (1u << (BALATRO_CENTER_J_SPLASH % 8)));
    assert(balatro_sell_joker(&state, 0) == BALATRO_OK);
    assert(!(state.used_centers[BALATRO_CENTER_J_SPLASH / 8] & (1u << (BALATRO_CENTER_J_SPLASH % 8))));

    config.validation = 0;
    assert(balatro_init_seed_string(&state, &config, "TESTSEED") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    balatro_populate_shop(&state);
    uint16_t first_center = state.shop_cards[0].center_id;
    assert(balatro_buy_shop_card(&state, 0) == BALATRO_OK);
    assert(state.shop_count == 4);
    assert((state.joker_count && state.jokers[0].center_id == first_center) || state.consumable_count);
    int dollars = state.dollars;
    assert(balatro_reroll_shop(&state) == BALATRO_OK);
    assert(state.dollars == dollars - 5);
    assert(state.reroll_cost == 6);
    assert(state.shop_count == 5);

    /* Paint Brush changes the persistent hand size, not only the value
       displayed while still in the shop. */
    assert(balatro_init_seed_string(&state, &config, "PAINT_BRUSH") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 20;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_V_PAINT_BRUSH, .cost = 10};
    assert(balatro_redeem_voucher(&state, 0) == BALATRO_OK);
    assert(state.hand_size == 9 && state.base_hand_size == 9);
    state.phase = BALATRO_PHASE_BLIND_SELECT;
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.hand_size == 9 && state.hand_count == 9);

    /* Overstock's change_shop_size(1) fills all empty shop-Joker slots
       immediately.  One prior purchase plus the added slot creates two
       replacement cards while retaining the separate booster. */
    assert(balatro_init_seed_string(&state, &config, "OVERSTOCK_REFILL") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 20;
    state.shop_joker_max = 2;
    state.shop_count = 3;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_CRAFTY, .cost = 4};
    state.shop_cards[1] = (BalatroCard){.center_id = BALATRO_CENTER_V_OVERSTOCK_NORM, .cost = 10};
    state.shop_cards[2] = (BalatroCard){.center_id = BALATRO_CENTER_P_ARCANA_NORMAL_1, .cost = 4};
    assert(balatro_redeem_voucher(&state, 1) == BALATRO_OK);
    assert(state.shop_joker_max == 3);
    assert(state.shop_count == 4);
    assert(state.shop_cards[0].center_id == BALATRO_CENTER_J_CRAFTY);
    assert(state.shop_cards[1].center_id == BALATRO_CENTER_P_ARCANA_NORMAL_1);

    /* Reroll vouchers reduce round_resets.reroll_cost, so the discount
       survives new_round rather than applying only to the current shop. */
    assert(balatro_init_seed_string(&state, &config, "REROLL_SURPLUS") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 20;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_V_REROLL_SURPLUS, .cost = 10};
    assert(balatro_redeem_voucher(&state, 0) == BALATRO_OK);
    assert(state.reroll_base == 3 && state.reroll_cost == 3);
    state.phase = BALATRO_PHASE_BLIND_SELECT;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.reroll_base == 3 && state.reroll_cost == 3);

    /* Director's Cut and Retcon govern the distinct $10 Boss reroll action.
       They must not create free shop rerolls. */
    static const uint16_t boss_reroll_vouchers[] = {
        BALATRO_CENTER_V_DIRECTORS_CUT,
        BALATRO_CENTER_V_RETCON,
    };
    for (size_t i = 0; i < sizeof(boss_reroll_vouchers) / sizeof(boss_reroll_vouchers[0]); ++i) {
        assert(balatro_init_seed_string(&state, &config, "BOSS_REROLL_VOUCHER") == BALATRO_OK);
        state.phase = BALATRO_PHASE_SHOP;
        state.dollars = 10;
        state.reroll_cost = 5;
        state.free_rerolls = 0;
        state.shop_count = 1;
        state.shop_cards[0] = (BalatroCard){
            .center_id = boss_reroll_vouchers[i],
            .cost = 10,
        };
        assert(balatro_redeem_voucher(&state, 0) == BALATRO_OK);
        assert(state.dollars == 0);
        assert(state.free_rerolls == 0 && state.reroll_cost == 5);
        BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
        int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
        assert(count > 0);
        for (int action_index = 0; action_index < count; ++action_index) assert(actions[action_index].type != BALATRO_ACTION_REROLL);
    }

    /* Clearance Sale immediately calls set_cost on owned and visible cards.
       Preserve Gift Card/Egg-style extra sell value while repricing. */
    assert(balatro_init_seed_string(&state, &config, "CLEARANCE_REPRICE") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 10;
    state.joker_count = 1;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_MAD};
    balatro_price_card(&state, &state.jokers[0]);
    state.jokers[0].sell_cost += 2;
    state.shop_count = 4;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_SMILEY};
    state.shop_cards[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_MADNESS};
    state.shop_cards[2] = (BalatroCard){.center_id = BALATRO_CENTER_P_STANDARD_NORMAL_1};
    state.shop_cards[3] = (BalatroCard){.center_id = BALATRO_CENTER_V_CLEARANCE_SALE};
    for (uint8_t i = 0; i < state.shop_count; ++i) balatro_price_card(&state, &state.shop_cards[i]);
    assert(balatro_redeem_voucher(&state, 3) == BALATRO_OK);
    assert(state.discount_percent == 25 && state.dollars == 0);
    assert(state.jokers[0].cost == 3 && state.jokers[0].sell_cost == 3);
    assert(state.shop_cards[0].cost == 3 && state.shop_cards[0].sell_cost == 1);
    assert(state.shop_cards[1].cost == 5 && state.shop_cards[1].sell_cost == 2);
    assert(state.shop_cards[2].cost == 3 && state.shop_cards[2].sell_cost == 1);

    /* Hone/Glow Up's edition_rate participates in poll_edition thresholds.
       This stream roll is 0.98544: Holographic at rate 1, Polychrome at 4. */
    assert(balatro_init_seed_string(&state, &config, "EDITION_17") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.edition_rate = 4.0f;
    state.shop_joker_max = 1;
    state.joker_rate = 1.0f;
    state.tarot_rate = state.planet_rate = state.spectral_rate = state.playing_card_rate = 0.0f;
    state.next_voucher_id = 0;
    balatro_populate_shop(&state);
    assert(state.shop_cards[0].edition == 3);

    /* enhancement_gate entries remain UNAVAILABLE until the corresponding
       enhancement exists in G.playing_cards.  This seed's rarity-2 roll is
       Lucky Cat; without a Lucky card source resamples to a different Joker. */
    assert(balatro_init_seed_string(&state, &config, "GATE_144") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.ante = 3;
    state.shop_joker_max = 1;
    state.next_voucher_id = 0;
    state.joker_rate = 1.0f;
    state.tarot_rate = state.planet_rate = state.spectral_rate = state.playing_card_rate = 0.0f;
    balatro_populate_shop(&state);
    assert(state.shop_cards[0].center_id != BALATRO_CENTER_J_LUCKY_CAT);
    assert(balatro_init_seed_string(&state, &config, "GATE_144") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.ante = 3;
    state.shop_joker_max = 1;
    state.next_voucher_id = 0;
    state.joker_rate = 1.0f;
    state.tarot_rate = state.planet_rate = state.spectral_rate = state.playing_card_rate = 0.0f;
    state.deck[0].enhancement = BALATRO_ENHANCEMENT_LUCKY;
    balatro_populate_shop(&state);
    assert(state.shop_cards[0].center_id == BALATRO_CENTER_J_LUCKY_CAT);

    /* Unsold shop inventory participates in G.GAME.used_jokers.  A Planet
       already offered in the shop must be resampled out of a concurrently
       opened Celestial pack. */
    assert(balatro_init_seed_string(&state, &config, "SHOP_PACK_DUP") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 20;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_P_CELESTIAL_NORMAL_1, .cost = 4};
    assert(balatro_open_booster(&state, 0) == BALATRO_OK);
    uint16_t unblocked_planet = state.pack_cards[0].center_id;
    assert(balatro_init_seed_string(&state, &config, "SHOP_PACK_DUP") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 20;
    state.shop_count = 2;
    state.shop_cards[0] = (BalatroCard){.center_id = unblocked_planet, .cost = 3};
    state.shop_cards[1] = (BalatroCard){.center_id = BALATRO_CENTER_P_CELESTIAL_NORMAL_1, .cost = 4};
    assert(balatro_open_booster(&state, 1) == BALATRO_OK);
    assert(state.pack_cards[0].center_id != unblocked_planet);
}

static int has_legal_action_type(const BalatroState *state, uint8_t type) {
    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    assert(count >= 0);
    for (int i = 0; i < count; ++i)
        if (actions[i].type == type) return 1;
    return 0;
}

static void test_source_boss_reroll_action(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    BalatroAction reroll = {.type = BALATRO_ACTION_REROLL_BOSS};
    BalatroStepResult result;

    assert(balatro_init_seed_string(&state, &config, "DIRECTORS_CUT") == BALATRO_OK);
    state.dollars = 20;
    state.used_centers[BALATRO_CENTER_V_DIRECTORS_CUT / 8] |= (uint8_t)(1u << (BALATRO_CENTER_V_DIRECTORS_CUT % 8));
    state.used_centers[BALATRO_CENTER_V_REROLL_SURPLUS / 8] |= (uint8_t)(1u << (BALATRO_CENTER_V_REROLL_SURPLUS % 8));
    state.reroll_base = 3;
    state.reroll_cost = 3;
    uint16_t first_boss = state.next_boss_id;
    assert(has_legal_action_type(&state, BALATRO_ACTION_REROLL_BOSS));
    assert(balatro_step(&state, &reroll, &result) == BALATRO_OK);
    /* Shop-reroll discounts do not change the separate $10 Boss reroll. */
    assert(state.dollars == 10 && state.boss_rerolled == 1);
    assert(state.reroll_base == 3 && state.reroll_cost == 3);
    assert(state.next_boss_id != first_boss);
    assert(!has_legal_action_type(&state, BALATRO_ACTION_REROLL_BOSS));
    assert(balatro_step(&state, &reroll, &result) == BALATRO_ERR_ACTION);

    /* Boss Tag calls reroll_boss with its price waived. It still sets
       round_resets.boss_rerolled and consumes Director's Cut for the ante. */
    assert(balatro_init_seed_string(&state, &config, "BOSS_TAG_CUT") == BALATRO_OK);
    state.dollars = 20;
    state.blind_tags[0] = BALATRO_TAG_TAG_BOSS;
    state.used_centers[BALATRO_CENTER_V_DIRECTORS_CUT / 8] |= (uint8_t)(1u << (BALATRO_CENTER_V_DIRECTORS_CUT % 8));
    BalatroAction skip = {.type = BALATRO_ACTION_SKIP_BLIND};
    assert(balatro_step(&state, &skip, &result) == BALATRO_OK);
    assert(state.dollars == 20 && state.boss_rerolled == 1);
    assert(!has_legal_action_type(&state, BALATRO_ACTION_REROLL_BOSS));

    /* Retcon bypasses only the per-ante use limit; every roll still costs
       $10 and draws another Boss from the shared `boss` stream. */
    assert(balatro_init_seed_string(&state, &config, "RETCON") == BALATRO_OK);
    state.dollars = 20;
    state.used_centers[BALATRO_CENTER_V_RETCON / 8] |= (uint8_t)(1u << (BALATRO_CENTER_V_RETCON % 8));
    assert(balatro_step(&state, &reroll, &result) == BALATRO_OK);
    assert(state.dollars == 10 && state.boss_rerolled == 1);
    assert(has_legal_action_type(&state, BALATRO_ACTION_REROLL_BOSS));
    assert(balatro_step(&state, &reroll, &result) == BALATRO_OK);
    assert(state.dollars == 0 && state.boss_rerolled == 1);
    assert(!has_legal_action_type(&state, BALATRO_ACTION_REROLL_BOSS));

    /* The affordability test uses bankrupt_at, including Credit Card. */
    state.dollars = 0;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_CREDIT_CARD;
    assert(has_legal_action_type(&state, BALATRO_ACTION_REROLL_BOSS));
    assert(balatro_step(&state, &reroll, &result) == BALATRO_OK);
    assert(state.dollars == -10);
}

static void test_spectral_lifecycle(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "SPECTRAL") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 5;
    for (uint8_t i = 0; i < state.hand_count; ++i) {
        state.hand[i].center_id = BALATRO_CENTER_C_BASE;
        state.hand[i].sort_id = (uint16_t)(i + 1);
        state.hand[i].rank = (uint8_t)(2 + i);
        state.hand[i].suit = i % 4;
    }
    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_FAMILIAR;
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    BalatroStepResult result;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.hand_count == 7);
    assert(state.deck_count == 52);
    for (uint8_t i = 0; i < state.hand_count; ++i) {
        if (state.hand[i].sort_id > 5) {
            assert(state.hand[i].rank >= 11 && state.hand[i].rank <= 13);
            assert(state.hand[i].enhancement != 0 && state.hand[i].enhancement != 6);
        }
    }

    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_HEX;
    state.joker_count = 3;
    for (uint8_t i = 0; i < state.joker_count; ++i) {
        state.jokers[i].center_id = (uint16_t)(BALATRO_CENTER_J_JOKER + i);
        state.jokers[i].sort_id = (uint16_t)(100 + i);
        state.jokers[i].sell_cost = 1;
    }
    use = (BalatroAction){.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.joker_count == 1);
    assert(state.jokers[0].edition == 3);

    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_ECTOPLASM;
    state.joker_count = 1;
    state.joker_slots = 5;
    state.jokers[0].center_id = BALATRO_CENTER_J_JOKER;
    state.jokers[0].edition = 0;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.jokers[0].edition == 4 && state.joker_slots == 6);
    assert(state.ecto_penalty == 2);
    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_ECTOPLASM;
    state.jokers[1].center_id = BALATRO_CENTER_J_GREEDY_JOKER;
    state.jokers[1].sort_id = 101;
    state.jokers[1].edition = 0;
    state.joker_count = 2;
    state.hand_size = state.base_hand_size = 7;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.jokers[1].edition == 4 && state.joker_slots == 7);
    assert(state.hand_size == 5 && state.base_hand_size == 5);
    assert(state.ecto_penalty == 3);
}

static void test_juggle_tag_next_round(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "JUGGLE") == BALATRO_OK);
    /* Force the fixture to exercise the tag lifecycle independent of the
       seed's generated tag pair. */
    state.blind_tags[0] = BALATRO_TAG_TAG_JUGGLE;
    BalatroAction action = {.type = BALATRO_ACTION_SKIP_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &action, &result) == BALATRO_OK);
    assert(state.blind_on_deck == 1);
    assert(state.tag_hand_bonus == 3);
    action.type = BALATRO_ACTION_SELECT_BLIND;
    assert(balatro_step(&state, &action, &result) == BALATRO_OK);
    assert(state.hand_size == 11);
    assert(state.hand_count == 11);
    assert(state.tag_hand_bonus == 0);

    /* Double Tag duplicates additive Juggle effects (+6 for one round). */
    assert(balatro_init_seed_string(&state, &config, "JUGGLE_DOUBLE") == BALATRO_OK);
    state.blind_tags[0] = BALATRO_TAG_TAG_JUGGLE;
    state.double_tag = 1;
    action = (BalatroAction){.type = BALATRO_ACTION_SKIP_BLIND};
    assert(balatro_step(&state, &action, &result) == BALATRO_OK);
    assert(state.tag_hand_bonus == 6);
    action.type = BALATRO_ACTION_SELECT_BLIND;
    assert(balatro_step(&state, &action, &result) == BALATRO_OK);
    assert(state.hand_size == 14 && state.tag_hand_bonus == 0);
}

static void test_source_orbital_choices_are_preselected(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "MC17") == BALATRO_OK);
    assert(state.orbital_hands[0] == BALATRO_STRAIGHT_FLUSH);
    assert(state.orbital_hands[1] == BALATRO_FLUSH);
    assert(state.orbital_hands[2] == BALATRO_STRAIGHT_FLUSH);

    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.blind_on_deck = 2;
    state.blind_id = BALATRO_BLIND_BL_SMALL;
    state.blind_chips = 1;
    state.hands_left = 1;
    state.deck_count = 0;
    state.hand_count = 1;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .suit = BALATRO_SPADES, .rank = 14, .sort_id = 1};
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.ante == 2 && state.orbital_hands[1] == BALATRO_STRAIGHT);

    state.phase = BALATRO_PHASE_BLIND_SELECT;
    state.blind_on_deck = 1;
    state.blind_tags[1] = BALATRO_TAG_TAG_ORBITAL;
    BalatroAction skip = {.type = BALATRO_ACTION_SKIP_BLIND};
    assert(balatro_step(&state, &skip, &result) == BALATRO_OK);
    assert(state.hand_levels[BALATRO_STRAIGHT] == 4);
    assert(state.hand_levels[BALATRO_FULL_HOUSE] == 1);
}

static void test_handy_tag_uses_run_hand_total(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "HANDY_TOTAL") == BALATRO_OK);
    state.blind_tags[0] = BALATRO_TAG_TAG_HANDY;
    state.dollars = 1;
    state.hands_played = 1;
    state.run_hands_played = 4;
    BalatroAction skip = {.type = BALATRO_ACTION_SKIP_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &skip, &result) == BALATRO_OK);
    assert(state.dollars == 5);
    assert(state.run_hands_played == 4);
}

static void test_source_top_up_tag_uses_joker_creation_pipeline(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "MC11") == BALATRO_OK);
    state.phase = BALATRO_PHASE_BLIND_SELECT;
    state.ante = 3;
    state.blind_on_deck = 0;
    state.blind_tags[0] = BALATRO_TAG_TAG_TOP_UP;
    state.joker_slots = 5;
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_MISPRINT;
    state.jokers[1].center_id = BALATRO_CENTER_J_GREEDY_JOKER;
    state.jokers[2].center_id = BALATRO_CENTER_J_ODD_TODD;
    memset(state.used_centers, 0, sizeof(state.used_centers));
    for (uint8_t i = 0; i < state.joker_count; ++i) {
        uint16_t center = state.jokers[i].center_id;
        state.used_centers[center / 8] |= (uint8_t)(1u << (center % 8));
    }

    BalatroAction skip = {.type = BALATRO_ACTION_SKIP_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &skip, &result) == BALATRO_OK);
    assert(state.joker_count == 5);
    assert(state.jokers[3].center_id == BALATRO_CENTER_J_SQUARE);
    assert(state.jokers[4].center_id == BALATRO_CENTER_J_SWASHBUCKLER);
    assert(state.jokers[3].edition == 0 && state.jokers[4].edition == 0);
}

static void test_source_immolate_preserves_survivor_visual_order(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "IMMOLATE_ORDER") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.hand_count = 8;
    static const uint16_t visual_order[8] = {8, 2, 7, 1, 6, 3, 5, 4};
    for (uint8_t i = 0; i < state.hand_count; ++i) {
        state.hand[i] = (BalatroCard){
            .center_id = BALATRO_CENTER_C_BASE,
            .sort_id = visual_order[i],
            .rank = (uint8_t)(2 + i),
        };
    }
    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_IMMOLATE;
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    BalatroStepResult result;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.hand_count == 3 && state.dollars == 24);

    uint8_t previous_position = 0;
    for (uint8_t survivor = 0; survivor < state.hand_count; ++survivor) {
        uint8_t position = 0;
        while (position < 8 && visual_order[position] != state.hand[survivor].sort_id) position++;
        assert(position < 8);
        if (survivor) assert(position > previous_position);
        previous_position = position;
    }
}

static void test_source_end_of_round_seals(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "BLUE_SEAL") == BALATRO_OK);
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    state.blind_chips = 1;
    /* Keep one Blue Seal and one held Gold card while playing a different
       card; the source emits a Planet and both held cards pay end-of-round. */
    state.hand[1].seal = 3;
    state.hand[2].enhancement = 7;
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_ROUND_EVAL);
    assert(state.consumable_count == 1);
    assert(state.consumables[0].center_id == BALATRO_CENTER_C_PLUTO);
    assert(state.dollars == 7);
}

static void test_source_dollar_jokers(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "DOLLAR_JOKERS") == BALATRO_OK);
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    state.blind_chips = 1;
    state.joker_count = 5;
    state.jokers[0].center_id = BALATRO_CENTER_J_GOLDEN;
    state.jokers[1].center_id = BALATRO_CENTER_J_CLOUD_9;
    state.jokers[2].center_id = BALATRO_CENTER_J_ROCKET;
    state.jokers[3].center_id = BALATRO_CENTER_J_SATELLITE;
    state.jokers[4].center_id = BALATRO_CENTER_J_DELAYED_GRAT;
    state.planet_usage_mask = (uint16_t)((1u << BALATRO_PAIR) | (1u << BALATRO_FLUSH));
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_ROUND_EVAL);
    /* $4 Golden + four 9s + $1 Rocket + two unique Planets + unused discards. */
    assert(state.round_earnings >= 11);

    assert(balatro_init_seed_string(&state, &config, "TO_MOON") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    state.shop_count = 1;
    state.shop_cards[0].center_id = BALATRO_CENTER_J_TO_THE_MOON;
    state.shop_cards[0].cost = 0;
    assert(balatro_buy_shop_card(&state, 0) == BALATRO_OK);
    assert(state.interest_amount == 2);
    assert(balatro_sell_joker(&state, 0) == BALATRO_OK);
    assert(state.interest_amount == 1);

    /* Source charges rentals before round evaluation (including debuffed
       rentals), then computes interest from the reduced liquid balance. */
    assert(balatro_init_seed_string(&state, &config, "RENTAL_TIMING") == BALATRO_OK);
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    state.dollars = 6;
    state.blind_chips = 1;
    state.blind_reward = 3;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_JOKER;
    state.jokers[0].flags = (1u << 3) | 1u;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_ROUND_EVAL);
    assert(state.dollars == 3);
    assert(state.round_earnings == 6); /* $3 blind + three unused hands; no interest. */
    BalatroAction cash_out = {.type = BALATRO_ACTION_CASH_OUT};
    assert(balatro_step(&state, &cash_out, &result) == BALATRO_OK);
    assert(state.dollars == 9);
}

static void test_source_perishable_lifecycle(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "PERISHABLE") == BALATRO_OK);
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    state.blind_chips = 1;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_JOKER;
    state.jokers[0].flags = 1u << 2;
    state.jokers[0].state[3] = 1;
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.jokers[0].flags & 1u);
}

static void test_stake_round_modifiers(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.stake = 5;
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "STAKE") == BALATRO_OK);
    assert(state.discards_per_round == 3);
    config.stake = 2;
    assert(balatro_init_seed_string(&state, &config, "STAKE2") == BALATRO_OK);
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.blind_reward == 0);
}

static void test_source_starting_vouchers(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    config.deck = BALATRO_CENTER_B_MAGIC;
    assert(balatro_init_seed_string(&state, &config, "MAGIC_VOUCHER") == BALATRO_OK);
    assert(state.consumable_count == 2);
    assert(state.consumable_slots == 3);
    config.deck = BALATRO_CENTER_B_NEBULA;
    assert(balatro_init_seed_string(&state, &config, "NEBULA_VOUCHER") == BALATRO_OK);
    assert(state.consumable_slots == 1);
    config.deck = BALATRO_CENTER_B_ZODIAC;
    assert(balatro_init_seed_string(&state, &config, "ZODIAC_VOUCHER") == BALATRO_OK);
    assert(state.shop_joker_max == 3);
    assert(state.tarot_rate == 9.6f && state.planet_rate == 9.6f);
}

static void test_anaglyph_boss_tag(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.deck = BALATRO_CENTER_B_ANAGLYPH;
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "ANAGLYPH") == BALATRO_OK);
    state.blind_on_deck = 2;
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.blind_id = BALATRO_BLIND_BL_SMALL;
    state.blind_chips = 1;
    state.hands_left = 1;
    state.discards_left = 1;
    state.hand_size = 1;
    state.hand_count = 1;
    state.hand[0].center_id = BALATRO_CENTER_C_BASE;
    state.hand[0].rank = 14;
    state.hand[0].suit = BALATRO_SPADES;
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.double_tag == 1);
}

static void test_telescope_planet_pack(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.deck = BALATRO_CENTER_B_NEBULA;
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "TELESCOPE") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    state.hand_plays[BALATRO_PAIR] = 2;
    state.shop_count = 1;
    state.shop_cards[0].center_id = BALATRO_CENTER_P_CELESTIAL_NORMAL_1;
    state.shop_cards[0].cost = 0;
    BalatroStepResult result;
    BalatroAction open = {.type = BALATRO_ACTION_OPEN_BOOSTER, .primary = 0};
    assert(balatro_step(&state, &open, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_PACK_OPENING);
    assert(state.pack_cards[0].center_id == BALATRO_CENTER_C_MERCURY);
}

static void test_source_tag_shop_hooks(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "TAG_HOOKS") == BALATRO_OK);
    state.blind_tags[0] = BALATRO_TAG_TAG_D_SIX;
    state.phase = BALATRO_PHASE_BLIND_SELECT;
    BalatroAction skip = {.type = BALATRO_ACTION_SKIP_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &skip, &result) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    balatro_populate_shop(&state);
    assert(state.reroll_cost == 0);
    assert(state.free_rerolls == 0);
    assert(balatro_reroll_shop(&state) == BALATRO_OK);
    assert(state.reroll_cost == 1);
    BalatroAction next_round = {.type = BALATRO_ACTION_NEXT_ROUND};
    assert(balatro_step(&state, &next_round, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_BLIND_SELECT);
    assert(state.reroll_cost == 1 && state.reroll_increase == 1 && state.tag_d_six_active);
    BalatroAction select_blind = {.type = BALATRO_ACTION_SELECT_BLIND};
    assert(balatro_step(&state, &select_blind, &result) == BALATRO_OK);
    assert(state.reroll_cost == 0 && state.reroll_increase == 0 && state.tag_d_six_active);
    state.blind_chips = 1;
    BalatroAction finish = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&state, &finish, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_ROUND_EVAL);
    assert(state.reroll_cost == 5 && !state.tag_d_six_active);

    assert(balatro_init_seed_string(&state, &config, "TAG_COUPON") == BALATRO_OK);
    state.blind_tags[0] = BALATRO_TAG_TAG_COUPON;
    assert(balatro_step(&state, &skip, &result) == BALATRO_OK);
    assert(state.discount_percent == 0);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    balatro_populate_shop(&state);
    assert(state.discount_percent == 0);

    assert(balatro_init_seed_string(&state, &config, "TAG_COUPON_TAROT") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.tag_coupon_pending = 1;
    state.shop_joker_max = 1;
    state.joker_rate = 0.0f;
    state.tarot_rate = 100.0f;
    state.planet_rate = 0.0f;
    state.spectral_rate = 0.0f;
    state.playing_card_rate = 0.0f;
    balatro_populate_shop(&state);
    assert(state.shop_count == 4);
    assert(state.shop_cards[0].center_id >= BALATRO_CENTER_C_CHARIOT && state.shop_cards[0].center_id <= BALATRO_CENTER_C_WORLD);
    assert(state.shop_cards[0].cost == 0);
    assert(state.shop_cards[1].center_id >= BALATRO_CENTER_V_ANTIMATTER && state.shop_cards[1].center_id < BALATRO_CENTER_COUNT);
    assert(state.shop_cards[1].cost > 0);
    assert(state.shop_cards[2].cost == 0 && state.shop_cards[3].cost == 0);

    assert(balatro_init_seed_string(&state, &config, "TAG_RARE") == BALATRO_OK);
    state.tag_force_rarity = 3;
    state.tag_force_rarity_count = 1;
    state.phase = BALATRO_PHASE_SHOP;
    balatro_populate_shop(&state);
    assert(state.shop_cards[0].cost == 0);
    assert(state.tag_force_rarity == 0);
    assert(state.tag_force_rarity_count == 0);

    assert(balatro_init_seed_string(&state, &config, "TAG_RARE_DOUBLE") == BALATRO_OK);
    state.tag_force_rarity = 3;
    state.tag_force_rarity_count = 2;
    state.phase = BALATRO_PHASE_SHOP;
    balatro_populate_shop(&state);
    assert(state.shop_cards[0].cost == 0 && state.shop_cards[1].cost == 0);
    assert(state.tag_force_rarity == 0 && state.tag_force_rarity_count == 0);

    assert(balatro_init_seed_string(&state, &config, "TAG_VOUCHER_DOUBLE") == BALATRO_OK);
    state.tag_voucher_pending = 2;
    state.phase = BALATRO_PHASE_SHOP;
    balatro_populate_shop(&state);
    assert(state.tag_voucher_pending == 0);
    uint8_t free_vouchers = 0;
    for (uint8_t i = 0; i < state.shop_count; ++i)
        if (state.shop_cards[i].cost == 0 && state.shop_cards[i].sell_cost == 0) free_vouchers++;
    assert(free_vouchers == 2);

    assert(balatro_init_seed_string(&state, &config, "TAG_FOIL") == BALATRO_OK);
    state.tag_force_edition = 1;
    state.phase = BALATRO_PHASE_SHOP;
    balatro_populate_shop(&state);
    assert(state.tag_force_edition == 0);

    /* A store_joker_modify tag remains pending when this shop produces no
       Joker at all; the source only marks it triggered after an eligible
       editionless Joker is actually modified. */
    assert(balatro_init_seed_string(&state, &config, "TAG_FOIL_NO_JOKER") == BALATRO_OK);
    state.shop_joker_max = 0;
    state.tag_force_edition = 1;
    balatro_populate_shop(&state);
    assert(state.tag_force_edition == 1);
}

static void test_source_joker_lifecycle_hooks(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "JOKER_LIFECYCLE") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_STUNTMAN, .cost = 0, .sell_cost = 1};
    assert(balatro_buy_shop_card(&state, 0) == BALATRO_OK);
    assert(state.hand_size == 6 && state.base_hand_size == 6);
    assert(balatro_sell_joker(&state, 0) == BALATRO_OK);
    assert(state.hand_size == 8 && state.base_hand_size == 8);

    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_CHAOS, .cost = 0, .sell_cost = 1};
    assert(balatro_buy_shop_card(&state, 0) == BALATRO_OK);
    assert(state.free_rerolls == 1 && state.reroll_cost == 0);
    assert(balatro_reroll_shop(&state) == BALATRO_OK);
    assert(state.free_rerolls == 0 && state.reroll_cost == 5);
    assert(balatro_sell_joker(&state, 0) == BALATRO_OK);
    assert(state.free_rerolls == 0 && state.reroll_cost == 5);

    state.joker_count = 0;
    state.consumable_count = state.consumable_slots;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_HERMIT, .edition = 4, .cost = 0, .sell_cost = 1};
    assert(balatro_buy_shop_card(&state, 0) == BALATRO_OK);
    assert(state.consumable_count == 3 && state.consumable_slots == 3);
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 2};
    BalatroStepResult result;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.consumable_count == 2 && state.consumable_slots == 2);

    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 1;
    state.blind_chips = 1;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
    state.joker_count = 1;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_TURTLE_BEAN, .state = {1, 0, 0, 0}};
    BalatroAction finish = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&state, &finish, &result) == BALATRO_OK);
    assert(state.joker_count == 0);

    BalatroState invisible;
    assert(balatro_init_seed_string(&invisible, &config, "INVISIBLE_FULL") == BALATRO_OK);
    invisible.phase = BALATRO_PHASE_SHOP;
    invisible.joker_count = 5;
    invisible.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_INVISIBLE, .state = {2, 0, 0, 0}, .sell_cost = 1, .sort_id = 1};
    for (uint8_t i = 1; i < invisible.joker_count; ++i)
        invisible.jokers[i] = (BalatroCard){.center_id = BALATRO_CENTER_J_JOKER, .sell_cost = 1, .sort_id = (uint16_t)(i + 1)};
    assert(balatro_sell_joker(&invisible, 0) == BALATRO_OK);
    assert(invisible.joker_count == 5 && invisible.jokers[4].center_id == BALATRO_CENTER_J_JOKER);

    BalatroState extinct;
    assert(balatro_init_seed_string(&extinct, &config, "GROS_MICHEL_EXTINCT") == BALATRO_OK);
    extinct.phase = BALATRO_PHASE_SELECTING_HAND;
    extinct.hand_count = 1;
    extinct.hand_size = 1;
    extinct.hands_left = 1;
    extinct.blind_chips = 1;
    extinct.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
    extinct.joker_count = 4;
    extinct.jokers[0].center_id = BALATRO_CENTER_J_GROS_MICHEL;
    extinct.jokers[1].center_id = BALATRO_CENTER_J_OOPS;
    extinct.jokers[2].center_id = BALATRO_CENTER_J_OOPS;
    extinct.jokers[3].center_id = BALATRO_CENTER_J_OOPS;
    BalatroAction advance = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&extinct, &advance, &result) == BALATRO_OK);
    assert(extinct.joker_count == 3);
    for (uint8_t i = 0; i < extinct.joker_count; ++i) assert(extinct.jokers[i].center_id == BALATRO_CENTER_J_OOPS);
    assert(extinct.gros_michel_extinct == 1);
    /* Source pool flags are reciprocal: extinction enables Cavendish and
       removes Gros Michel from subsequent Joker pools. */
    extinct.phase = BALATRO_PHASE_SHOP;
    extinct.dollars = 100;
    extinct.shop_count = 0;
    extinct.shop_joker_max = 2;
    assert(balatro_reroll_shop(&extinct) == BALATRO_OK);
    for (uint8_t i = 0; i < extinct.shop_count; ++i) assert(extinct.shop_cards[i].center_id != BALATRO_CENTER_J_GROS_MICHEL);

    BalatroState marble;
    assert(balatro_init_seed_string(&marble, &config, "MARBLE_RANDOM") == BALATRO_OK);
    marble.joker_count = 1;
    marble.jokers[0].center_id = BALATRO_CENTER_J_MARBLE;
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    assert(balatro_step(&marble, &select, &result) == BALATRO_OK);
    assert(marble.deck_count + marble.hand_count == 53);
    int stone_found = 0;
    for (uint16_t i = 0; i < marble.deck_count; ++i)
        if (marble.deck[i].center_id == BALATRO_CENTER_C_BASE && marble.deck[i].enhancement == 6) stone_found = 1;
    for (uint8_t i = 0; i < marble.hand_count; ++i)
        if (marble.hand[i].center_id == BALATRO_CENTER_C_BASE && marble.hand[i].enhancement == 6) stone_found = 1;
    assert(stone_found);

    BalatroState certificate;
    assert(balatro_init_seed_string(&certificate, &config, "CERTIFICATE_HAND") == BALATRO_OK);
    certificate.joker_count = 1;
    certificate.jokers[0].center_id = BALATRO_CENTER_J_CERTIFICATE;
    assert(balatro_step(&certificate, &select, &result) == BALATRO_OK);
    assert(certificate.hand_count == 9 && certificate.deck_count == 44);

    BalatroState wheel_state;
    assert(balatro_init_seed_string(&wheel_state, &config, "WHEEL_OF_FORTUNE") == BALATRO_OK);
    wheel_state.phase = BALATRO_PHASE_SHOP;
    wheel_state.joker_count = 4;
    wheel_state.jokers[0].center_id = BALATRO_CENTER_J_JOKER;
    wheel_state.jokers[1].center_id = BALATRO_CENTER_J_OOPS;
    wheel_state.jokers[2].center_id = BALATRO_CENTER_J_OOPS;
    wheel_state.jokers[3].center_id = BALATRO_CENTER_J_OOPS;
    wheel_state.consumable_count = 1;
    wheel_state.consumables[0].center_id = BALATRO_CENTER_C_WHEEL_OF_FORTUNE;
    BalatroAction use_wheel = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    assert(balatro_step(&wheel_state, &use_wheel, &result) == BALATRO_OK);
    int editioned = 0;
    for (uint8_t i = 0; i < wheel_state.joker_count; ++i)
        if (wheel_state.jokers[i].edition) editioned = 1;
    assert(editioned);

    BalatroState tarot;
    assert(balatro_init_seed_string(&tarot, &config, "TAROT_GENERATORS") == BALATRO_OK);
    tarot.phase = BALATRO_PHASE_SHOP;
    tarot.consumable_count = 1;
    tarot.consumables[0].center_id = BALATRO_CENTER_C_EMPEROR;
    assert(balatro_step(&tarot, &use_wheel, &result) == BALATRO_OK);
    assert(tarot.consumable_count == 2);
    tarot.consumable_count = 1;
    tarot.consumables[0].center_id = BALATRO_CENTER_C_JUDGEMENT;
    assert(balatro_step(&tarot, &use_wheel, &result) == BALATRO_OK);
    assert(tarot.joker_count == 1);
    tarot.consumable_count = 1;
    tarot.consumables[0].center_id = BALATRO_CENTER_C_FOOL;
    assert(tarot.last_tarot_planet == BALATRO_CENTER_C_JUDGEMENT);
    assert(balatro_step(&tarot, &use_wheel, &result) == BALATRO_OK);
    assert(tarot.consumable_count == 1 && tarot.consumables[0].center_id == BALATRO_CENTER_C_JUDGEMENT);

    BalatroState death;
    assert(balatro_init_seed_string(&death, &config, "DEATH_HANGED") == BALATRO_OK);
    death.phase = BALATRO_PHASE_SHOP;
    death.hand_count = 2;
    death.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_HEARTS, .sort_id = 1};
    death.hand[1] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_SPADES, .sort_id = 2};
    death.consumable_count = 1;
    death.consumables[0].center_id = BALATRO_CENTER_C_DEATH;
    BalatroAction use_death = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0, .selection_count = 2, .selection = {0, 1}};
    assert(balatro_step(&death, &use_death, &result) == BALATRO_OK);
    assert(death.hand[0].rank == 14 && death.hand[0].suit == BALATRO_SPADES);

    BalatroState strength;
    assert(balatro_init_seed_string(&strength, &config, "STRENGTH_ACE") == BALATRO_OK);
    strength.phase = BALATRO_PHASE_SHOP;
    strength.hand_count = 1;
    strength.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14};
    strength.consumable_count = 1;
    strength.consumables[0].center_id = BALATRO_CENTER_C_STRENGTH;
    BalatroAction use_strength = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&strength, &use_strength, &result) == BALATRO_OK);
    assert(strength.hand[0].rank == 2);

    BalatroState hermit;
    assert(balatro_init_seed_string(&hermit, &config, "HERMIT_NEGATIVE") == BALATRO_OK);
    hermit.phase = BALATRO_PHASE_SHOP;
    hermit.dollars = -5;
    hermit.consumable_count = 1;
    hermit.consumables[0].center_id = BALATRO_CENTER_C_HERMIT;
    assert(balatro_step(&hermit, &use_wheel, &result) == BALATRO_OK);
    assert(hermit.dollars == -5);
}

static void test_full_joker_slots_accept_negative_shop_joker(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "NEGATIVE_FULL_SLOTS") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    state.joker_count = state.joker_slots;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){
        .center_id = BALATRO_CENTER_J_JOKER,
        .edition = BALATRO_EDITION_NEGATIVE,
        .cost = 4,
        .sell_cost = 1,
    };

    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    int buy_index = -1;
    for (int i = 0; i < count; ++i)
        if (actions[i].type == BALATRO_ACTION_BUY_CARD && actions[i].primary == 0) buy_index = i;
    assert(buy_index >= 0);

    BalatroStepResult result;
    assert(balatro_step(&state, &actions[buy_index], &result) == BALATRO_OK);
    assert(state.joker_count == 6 && state.joker_slots == 6);
    assert(state.jokers[5].edition == BALATRO_EDITION_NEGATIVE);
}

static void test_source_verdant_leaf_sale_disables_blind(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "VERDANT_SALE") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.blind_on_deck = 2;
    state.blind_id = BALATRO_BLIND_BL_FINAL_LEAF;
    state.blind_disabled = 0;
    state.deck_count = state.hand_count = state.discard_count = 1;
    state.deck[0].flags = BALATRO_CARD_DEBUFFED;
    state.hand[0].flags = BALATRO_CARD_DEBUFFED;
    state.discard[0].flags = BALATRO_CARD_DEBUFFED;
    state.joker_count = 2;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_JOKER, .sell_cost = 1};
    state.jokers[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_GREEDY_JOKER, .flags = BALATRO_CARD_DEBUFFED};

    assert(balatro_sell_joker(&state, 0) == BALATRO_OK);
    assert(state.blind_disabled);
    assert(state.joker_count == 1);
    assert(!(state.deck[0].flags & BALATRO_CARD_DEBUFFED));
    assert(!(state.hand[0].flags & BALATRO_CARD_DEBUFFED));
    assert(!(state.discard[0].flags & BALATRO_CARD_DEBUFFED));
    assert(!(state.jokers[0].flags & BALATRO_CARD_DEBUFFED));
}

static void test_source_free_pack_tags(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "TAG_PACK") == BALATRO_OK);
    state.blind_tags[0] = BALATRO_TAG_TAG_STANDARD;
    BalatroAction skip = {.type = BALATRO_ACTION_SKIP_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &skip, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_PACK_OPENING);
    assert(state.pack_count == 5);
    assert(state.pack_choices == 2);
    assert(state.blind_on_deck == 1);
    BalatroAction skip_pack = {.type = BALATRO_ACTION_SKIP_PACK};
    assert(balatro_step(&state, &skip_pack, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_BLIND_SELECT);

    /* A Double Tag duplicates a pack-producing tag.  The second pack must be
       queued until the first pack is closed, rather than replacing its cards. */
    assert(balatro_init_seed_string(&state, &config, "TAG_PACK_DOUBLE") == BALATRO_OK);
    state.blind_tags[0] = BALATRO_TAG_TAG_STANDARD;
    state.double_tag = 1;
    assert(balatro_step(&state, &skip, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_PACK_OPENING);
    assert(state.pending_free_pack_id == BALATRO_CENTER_P_STANDARD_MEGA_1);
    assert(state.pack_count == 5 && state.pack_choices == 2);
    assert(balatro_step(&state, &skip_pack, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_PACK_OPENING);
    assert(state.pending_free_pack_id == 0);
    assert(state.pack_count == 5 && state.pack_choices == 2);
    assert(balatro_step(&state, &skip_pack, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_BLIND_SELECT);

    /* Standard packs can contain enhanced (set 2) playing cards; those are
       still valid deck picks, not an invalid action. */
    state.phase = BALATRO_PHASE_PACK_OPENING;
    state.pack_count = 1;
    state.pack_choices = 1;
    state.deck_count = 2;
    state.deck[0].sort_id = 1;
    state.deck[1].sort_id = 2;
    state.pack_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_M_BONUS, .enhancement = 1, .sort_id = 9};
    assert(balatro_pick_pack_card(&state, 0) == BALATRO_OK);
    assert(state.deck_count == 3 && state.deck[0].center_id == BALATRO_CENTER_M_BONUS);
    assert(state.deck[0].sort_id == 9 && state.deck[1].sort_id == 1 && state.deck[2].sort_id == 2);

    /* Legal-action enumeration must not advertise a Buffoon pick which the
       transition will reject because all Joker slots are occupied. */
    state.phase = BALATRO_PHASE_PACK_OPENING;
    state.pack_count = 1;
    state.pack_choices = 1;
    state.joker_count = state.joker_slots;
    state.pack_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_JOKER};
    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int action_count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    for (int i = 0; i < action_count; ++i) assert(actions[i].type != BALATRO_ACTION_PICK_PACK_CARD);
    state.pack_cards[0].edition = 4;
    action_count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    int negative_pick = 0;
    for (int i = 0; i < action_count; ++i)
        if (actions[i].type == BALATRO_ACTION_PICK_PACK_CARD) negative_pick = 1;
    assert(negative_pick);

    /* LONG_TRACE_3's first Ante 2 Jumbo Standard pack is a source-derived
       regression for get_current_pool("Enhanced", ..., "sta").  The pool
       stream includes the ante: Enhancedsta2. */
    assert(balatro_init_seed_string(&state, &config, "LONG_TRACE_3") == BALATRO_OK);
    state.ante = 2;
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_P_STANDARD_JUMBO_1, .cost = 6};
    assert(balatro_open_booster(&state, 0) == BALATRO_OK);
    assert(state.pack_count == 5);
    assert(state.pack_cards[1].rank == 8 && state.pack_cards[1].suit == BALATRO_HEARTS);
    assert(state.pack_cards[1].center_id == BALATRO_CENTER_M_BONUS);
    assert(state.pack_cards[1].enhancement == 1);

    /* Standard Pack poll_edition uses modifier 2 multiplied by the run's
       edition_rate. This stream roll becomes Polychrome at rate 4. */
    assert(balatro_init_seed_string(&state, &config, "STDED_126") == BALATRO_OK);
    state.edition_rate = 4.0f;
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_P_STANDARD_NORMAL_1, .cost = 4};
    assert(balatro_open_booster(&state, 0) == BALATRO_OK);
    assert(state.pack_cards[0].edition == 3);
}

static void test_source_midas_vampire_mutation(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    BalatroCard face = {.center_id = BALATRO_CENTER_C_BASE, .rank = 12, .suit = BALATRO_HEARTS, .enhancement = 0};
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_MIDAS_MASK;
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &face, 1, NULL, 0, &score) == BALATRO_OK);
    assert(face.enhancement == 7);

    state.jokers[0].center_id = BALATRO_CENTER_J_VAMPIRE;
    state.jokers[0].state[0] = 0;
    face.enhancement = 2;
    assert(balatro_score_hand(&state, &face, 1, NULL, 0, &score) == BALATRO_OK);
    assert(face.enhancement == 0);
    /* Vampire strips the enhancement before card scoring, so Bonus's +30
       chips must not leak into this hand.  High Card is 5 base + Queen 10. */
    assert(score.chips == 15.0);
    assert(score.mult > 1.0);
}

static void test_source_glass_joker_destruction_hooks(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 12, .suit = BALATRO_HEARTS, .enhancement = 4, .sort_id = 1};
    state.hands_left = 1;
    state.discards_left = 3;
    state.blind_chips = 100000;
    state.joker_count = 5;
    state.jokers[0].center_id = BALATRO_CENTER_J_GLASS;
    for (uint8_t i = 1; i < state.joker_count; ++i) state.jokers[i].center_id = BALATRO_CENTER_J_OOPS;
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    /* The source dispatches both remove_playing_cards and cards_destroyed;
       the face glass card therefore gives Glass Joker +1.5 Xmult. */
    assert(state.jokers[0].state[0] == 250);
}

static void test_source_glass_hanged_man_hooks(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .enhancement = 4, .sort_id = 1};
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_GLASS;
    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_HANGED_MAN;
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    /* Hanged Man's using_consumeable hook counts the highlighted Glass card.
       Its remove_playing_cards hook only counts `shattered` cards, so an
       ordinary Hanged Man removal must not apply the upgrade twice. */
    assert(state.jokers[0].state[0] == 175 && state.hand_count == 0);
}

static void test_source_matador_debuff_bonus(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.blind_id = BALATRO_BLIND_BL_PSYCHIC;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_MATADOR;
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    assert(score.dollars == 8 && score.total == 0);
}

static void test_source_hologram_playing_card_added(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .sort_id = 1};
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_HOLOGRAM;
    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_CRYPTID;
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.hand_count == 3 && state.jokers[0].state[0] == 150);
}

static void test_source_gold_seal_only_pays_when_played(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState held, played;
    assert(balatro_init(&held, &config, 1) == BALATRO_OK);
    assert(balatro_init(&played, &config, 1) == BALATRO_OK);
    for (int which = 0; which < 2; ++which) {
        BalatroState *state = which ? &played : &held;
        state->phase = BALATRO_PHASE_SELECTING_HAND;
        state->hand_count = 2;
        state->hand_size = 2;
        state->hands_left = 1;
        state->discards_left = 3;
        state->blind_id = BALATRO_BLIND_BL_SMALL;
        state->blind_on_deck = 0;
        state->blind_chips = 1;
        state->hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS, .sort_id = 1};
        state->hand[1] =
            (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .seal = which ? 1 : 0, .sort_id = 2};
        BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {1}};
        BalatroStepResult result;
        assert(balatro_step(state, &play, &result) == BALATRO_OK);
        assert(state->phase == BALATRO_PHASE_ROUND_EVAL);
    }
    /* The played seal's dollars are present before round evaluation, so it
       may also increase interest in the displayed round earnings. */
    assert(played.dollars == held.dollars + 3);
}

static void test_source_seltzer_expires_after_hand(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 2;
    state.blind_chips = 100000;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_SELZER;
    state.jokers[0].state[0] = 1;
    BalatroAction next = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &next, &result) == BALATRO_OK);
    assert(state.joker_count == 0 && state.phase == BALATRO_PHASE_SELECTING_HAND);
}

static void test_source_mime_repeats_round_eval_cards(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState plain, mime;
    assert(balatro_init(&plain, &config, 1) == BALATRO_OK);
    assert(balatro_init(&mime, &config, 1) == BALATRO_OK);
    for (int which = 0; which < 2; ++which) {
        BalatroState *state = which ? &mime : &plain;
        state->phase = BALATRO_PHASE_SELECTING_HAND;
        state->hand_count = 2;
        state->hand_size = 2;
        state->hands_left = 1;
        state->discards_left = 3;
        state->blind_id = BALATRO_BLIND_BL_SMALL;
        state->blind_chips = 1;
        state->hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS, .sort_id = 1};
        state->hand[1] =
            (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .enhancement = 7, .sort_id = 2};
        if (which) {
            state->joker_count = 1;
            state->jokers[0].center_id = BALATRO_CENTER_J_MIME;
        }
        BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
        BalatroStepResult result;
        assert(balatro_step(state, &play, &result) == BALATRO_OK);
    }
    assert(mime.dollars == plain.dollars + 3);
    assert(mime.round_earnings == plain.round_earnings + 1); /* $10 crosses an interest step. */
}

static void test_source_ice_cream_melts(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 1;
    state.discards_left = 3;
    state.blind_id = BALATRO_BLIND_BL_SMALL;
    state.blind_chips = 1;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_ICE_CREAM;
    state.jokers[0].state[0] = 5;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS, .sort_id = 1};
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.joker_count == 0);
}

static void test_source_to_do_list_rollover(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.validation = 1;
    BalatroState initialized;
    assert(balatro_init_seed_string(&initialized, &config, "CLV00012") == BALATRO_OK);
    assert(balatro_debug_add_center(&initialized, BALATRO_CENTER_J_TODO_LIST, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    /* Card:set_ability's pairs(G.GAME.hands) order under Balatro's LuaJIT
       maps this seed's second candidate to Flush. */
    assert(initialized.jokers[0].state[1] == BALATRO_FLUSH);

    /* Unbought shop inventory also initializes stateful Joker abilities, so
       a transient To Do List advances the stream before an owned copy. */
    BalatroState construction;
    assert(balatro_init_seed_string(&construction, &config, "MC12") == BALATRO_OK);
    BalatroCard transient = {.center_id = BALATRO_CENTER_J_TODO_LIST};
    balatro_initialize_joker_card(&construction, &transient);
    assert(transient.state[1] == BALATRO_THREE_OF_A_KIND);
    assert(balatro_debug_add_center(&construction, BALATRO_CENTER_J_TODO_LIST, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    assert(construction.jokers[0].state[1] == BALATRO_STRAIGHT);

    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 1;
    state.blind_chips = 1;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_TODO_LIST;
    state.jokers[0].state[1] = BALATRO_PAIR;
    state.jokers[0].state[2] = 1;
    BalatroAction next = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &next, &result) == BALATRO_OK);
    assert(state.jokers[0].state[1] != BALATRO_PAIR);
    assert(state.jokers[0].state[1] >= BALATRO_STRAIGHT_FLUSH);
    assert(state.jokers[0].state[1] <= BALATRO_HIGH_CARD);

    /* Joker-main pays dollars; To Do List does not add
       Mult.  Ace High Card therefore remains (5 + 11) x 1. */
    BalatroState scoring;
    assert(balatro_init(&scoring, &config, 1) == BALATRO_OK);
    scoring.joker_count = 1;
    scoring.jokers[0].center_id = BALATRO_CENTER_J_TODO_LIST;
    scoring.jokers[0].state[1] = BALATRO_HIGH_CARD;
    scoring.jokers[0].state[2] = 1;
    BalatroCard ace = {.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
    BalatroScoreResult score;
    assert(balatro_score_hand(&scoring, &ace, 1, NULL, 0, &score) == BALATRO_OK);
    assert(score.chips == 16.0 && score.mult == 1.0 && score.total == 16);
    assert(score.dollars == 4);
}

static void test_source_flint_modifies_base_hand_before_cards(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.blind_id = BALATRO_BLIND_BL_FLINT;
    state.deck_count = 44;
    state.joker_count = 5;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_SIXTH_SENSE, .sell_cost = 3};
    state.jokers[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_SWASHBUCKLER, .sell_cost = 2};
    state.jokers[2] = (BalatroCard){.center_id = BALATRO_CENTER_J_EVEN_STEVEN, .sell_cost = 2};
    state.jokers[3] = (BalatroCard){.center_id = BALATRO_CENTER_J_BLUE_JOKER, .sell_cost = 2};
    state.jokers[4] = (BalatroCard){.center_id = BALATRO_CENTER_J_DELAYED_GRAT, .sell_cost = 2};
    BalatroCard flush[5] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 10, .suit = BALATRO_DIAMONDS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 7, .suit = BALATRO_DIAMONDS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 6, .suit = BALATRO_DIAMONDS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 4, .suit = BALATRO_DIAMONDS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 3, .suit = BALATRO_DIAMONDS},
    };
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, flush, 5, NULL, 0, &score) == BALATRO_OK);
    /* blind.lua:512 halves and rounds base Flush 35x4 to 18x2. Cards then
       add 30 Chips, Blue Joker adds 88, Even Steven adds 12 Mult, and
       Swashbuckler adds the other Jokers' $9 sell value. */
    assert(score.chips == 136.0 && score.mult == 23.0 && score.total == 3128);
}

static void test_source_stone_tally_avoids_played_duplicate(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_STONE;
    BalatroCard stone = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .enhancement = 6, .sort_id = 1};
    state.discard_count = 1;
    state.discard[0] = stone;
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &stone, 1, NULL, 0, &score) == BALATRO_OK);
    /* High Card 5 + Stone card 50 + one unique Stone Joker tally 25. */
    assert(score.chips == 80.0);
}

static void test_source_idol_target_reset(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.validation = 1;
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.deck_count = 1;
    state.deck[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .sort_id = 1};
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.idol_rank == 9 && state.idol_suit == BALATRO_CLUBS);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_IDOL, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    assert(state.jokers[0].state[0] == 9 && state.jokers[0].state[1] == BALATRO_CLUBS);
}

static void test_source_mail_castle_target_reset(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.validation = 1;
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.deck_count = 1;
    state.deck[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .sort_id = 1};
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.mail_rank == 9 && state.castle_suit == BALATRO_CLUBS);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_MAIL, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    assert(balatro_debug_add_center(&state, BALATRO_CENTER_J_CASTLE, BALATRO_EDITION_NONE, 0) == BALATRO_OK);
    assert(state.jokers[0].state[1] == 9 && state.jokers[1].state[1] == BALATRO_CLUBS);
}

static void test_source_loyalty_card_cycle(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.run_hands_played = 4;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_LOYALTY_CARD;
    state.jokers[0].state[1] = 0;
    state.jokers[0].state[2] = 1;
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS};
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    /* Evaluation occurs before incrementing hands_played: hand five only
       becomes active, while hand six receives the ×4 multiplier. */
    assert(score.mult == 1.0);
    state.run_hands_played = 5;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    assert(score.mult == 4.0);
}

static void test_source_hand_counter_timing(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState supernova, sharp;
    assert(balatro_init(&supernova, &config, 1) == BALATRO_OK);
    assert(balatro_init(&sharp, &config, 1) == BALATRO_OK);
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS};
    BalatroScoreResult score;
    supernova.joker_count = 1;
    supernova.jokers[0].center_id = BALATRO_CENTER_J_SUPERNOVA;
    assert(balatro_score_hand(&supernova, &card, 1, NULL, 0, &score) == BALATRO_OK);
    assert(score.mult == 2.0 && supernova.hand_plays[BALATRO_HIGH_CARD] == 1);
    sharp.joker_count = 1;
    sharp.jokers[0].center_id = BALATRO_CENTER_J_CARD_SHARP;
    sharp.hand_plays_round[BALATRO_HIGH_CARD] = 1;
    assert(balatro_score_hand(&sharp, &card, 1, NULL, 0, &score) == BALATRO_OK);
    assert(score.mult == 3.0 && sharp.hand_plays_round[BALATRO_HIGH_CARD] == 2);
}

static void test_source_brainstorm_first_slot(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS};
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BRAINSTORM;
    state.jokers[1].center_id = BALATRO_CENTER_J_JOKER;
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    /* Brainstorm targets only the first Joker. In slot zero it
       has no distinct target, so the second Joker contributes the only +4. */
    assert(score.mult == 5.0);
}

static void test_source_flower_pot_wildcard_order(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_FLOWER_POT;
    /* Wild first, then a duplicate Heart and two other ordinary suits.
       Source's two-pass Flower Pot evaluation uses the Wild to fill the
       missing Spade; a single-pass implementation incorrectly consumes Heart.
       A five-card straight makes every card part of scoring_hand. */
    BalatroCard cards[5] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_HEARTS, .enhancement = 3},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 3, .suit = BALATRO_HEARTS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 4, .suit = BALATRO_DIAMONDS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 5, .suit = BALATRO_CLUBS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 6, .suit = BALATRO_SPADES},
    };
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, cards, 5, NULL, 0, &score) == BALATRO_OK);
    assert(score.mult == 12.0);
}

static void test_source_debuffed_discard_hooks(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    BalatroStepResult result;
    BalatroAction discard = {.type = BALATRO_ACTION_DISCARD, .selection_count = 1, .selection = {0}};

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand_size = 1;
    state.discards_left = 1;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .flags = 1u};
    state.joker_count = 1;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_MAIL, .state = {0, 9, 0, 0}};
    int dollars = state.dollars;
    assert(balatro_step(&state, &discard, &result) == BALATRO_OK);
    assert(state.dollars == dollars);

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.joker_count = 1;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_POPCORN, .flags = 1u, .state = {4, 0, 0, 0}};
    BalatroAction next = {.type = BALATRO_ACTION_NEXT_ROUND};
    assert(balatro_step(&state, &next, &result) == BALATRO_OK);
    assert(state.jokers[0].state[0] == 4);
}

static void test_source_matador_ordinary_debuff(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.blind_id = BALATRO_BLIND_BL_CLUB;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_MATADOR;
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .flags = 1u};
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    assert(score.dollars == 8 && score.total == 5);
}

static void test_source_erosion_abandoned_deck_size(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.deck = BALATRO_CENTER_B_ABANDONED;
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    assert(state.deck_count == 40);
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_EROSION;
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS};
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    /* No cards have been removed from the 40-card Abandoned starting deck. */
    assert(score.mult == 1.0);
}

static void test_source_plasma_final_scoring(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    config.deck = BALATRO_CENTER_B_PLASMA;
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS};
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    /* High Card starts at 14 chips and 1 mult after the 9 is scored; Plasma
       balances both components to floor(15/2) at final_scoring_step. */
    assert(score.chips == 7.0 && score.mult == 7.0 && score.total == 49);
}

static void test_source_retrigger_stateful_jokers(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS, .seal = 2};
    BalatroScoreResult score;

    BalatroState hiker;
    assert(balatro_init(&hiker, &config, 1) == BALATRO_OK);
    hiker.joker_count = 1;
    hiker.jokers[0].center_id = BALATRO_CENTER_J_HIKER;
    assert(balatro_score_hand(&hiker, &card, 1, NULL, 0, &score) == BALATRO_OK);
    assert(card.perma_bonus == 10);

    BalatroState wee;
    assert(balatro_init(&wee, &config, 1) == BALATRO_OK);
    wee.joker_count = 1;
    wee.jokers[0].center_id = BALATRO_CENTER_J_WEE;
    card.perma_bonus = 0;
    assert(balatro_score_hand(&wee, &card, 1, NULL, 0, &score) == BALATRO_OK);
    /* Both evaluations upgrade Wee; its final +16 Chips applies once. */
    assert(wee.jokers[0].state[0] == 16 && score.chips == 25.0);

    assert(balatro_init(&wee, &config, 1) == BALATRO_OK);
    wee.joker_count = 2;
    wee.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    wee.jokers[1].center_id = BALATRO_CENTER_J_WEE;
    card.seal = BALATRO_SEAL_NONE;
    assert(balatro_score_hand(&wee, &card, 1, NULL, 0, &score) == BALATRO_OK);
    assert(wee.jokers[1].state[0] == 24 && score.chips == 35.0);
}

static void test_source_retrigger_interaction_matrix(void) {
    typedef struct RetriggerCase {
        const char *name;
        uint8_t rank;
        uint8_t seal;
        uint8_t hands_left;
        uint16_t jokers[3];
        uint8_t joker_count;
        uint8_t repetitions;
    } RetriggerCase;
    static const RetriggerCase cases[] = {
        {"red+chad", 9, BALATRO_SEAL_RED, 2, {BALATRO_CENTER_J_HANGING_CHAD}, 1, 4},
        {"red+hack", 2, BALATRO_SEAL_RED, 2, {BALATRO_CENTER_J_HACK}, 1, 3},
        {"chad+hack", 2, 0, 2, {BALATRO_CENTER_J_HANGING_CHAD, BALATRO_CENTER_J_HACK}, 2, 4},
        {"chad+seltzer", 9, 0, 2, {BALATRO_CENTER_J_HANGING_CHAD, BALATRO_CENTER_J_SELZER}, 2, 4},
        {"red+sock", 13, BALATRO_SEAL_RED, 2, {BALATRO_CENTER_J_SOCK_AND_BUSKIN}, 1, 3},
        {"red+dusk", 9, BALATRO_SEAL_RED, 0, {BALATRO_CENTER_J_DUSK}, 1, 3},
        {"red+chad+hack", 2, BALATRO_SEAL_RED, 2, {BALATRO_CENTER_J_HANGING_CHAD, BALATRO_CENTER_J_HACK}, 2, 5},
        {"red+chad+seltzer", 9, BALATRO_SEAL_RED, 2, {BALATRO_CENTER_J_HANGING_CHAD, BALATRO_CENTER_J_SELZER}, 2, 5},
        {"red+chad+dusk", 9, BALATRO_SEAL_RED, 0, {BALATRO_CENTER_J_HANGING_CHAD, BALATRO_CENTER_J_DUSK}, 2, 5},
        {"red+chad+sock", 13, BALATRO_SEAL_RED, 2, {BALATRO_CENTER_J_HANGING_CHAD, BALATRO_CENTER_J_SOCK_AND_BUSKIN}, 2, 5},
        {"blueprint+chad", 9, 0, 2, {BALATRO_CENTER_J_BLUEPRINT, BALATRO_CENTER_J_HANGING_CHAD}, 2, 5},
        {"chad+brainstorm", 9, 0, 2, {BALATRO_CENTER_J_HANGING_CHAD, BALATRO_CENTER_J_BRAINSTORM}, 2, 5},
        {"blueprint+hack", 2, 0, 2, {BALATRO_CENTER_J_BLUEPRINT, BALATRO_CENTER_J_HACK}, 2, 3},
        {"red+blueprint+chad", 9, BALATRO_SEAL_RED, 2, {BALATRO_CENTER_J_BLUEPRINT, BALATRO_CENTER_J_HANGING_CHAD}, 2, 6},
        {"blueprint+blueprint+chad",
         9,
         0,
         2,
         {BALATRO_CENTER_J_BLUEPRINT, BALATRO_CENTER_J_BLUEPRINT, BALATRO_CENTER_J_HANGING_CHAD},
         3,
         7},
    };
    BalatroConfig config;
    balatro_default_config(&config);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const RetriggerCase *test = &cases[i];
        BalatroState state;
        assert(balatro_init(&state, &config, 1) == BALATRO_OK);
        state.hands_left = test->hands_left;
        state.joker_count = (uint8_t)(test->joker_count + 1);
        for (uint8_t j = 0; j < test->joker_count; ++j) state.jokers[j].center_id = test->jokers[j];
        state.jokers[test->joker_count].center_id = BALATRO_CENTER_J_HIKER;
        BalatroCard played = {.center_id = BALATRO_CENTER_C_BASE, .rank = test->rank, .suit = BALATRO_CLUBS, .seal = test->seal};
        BalatroScoreResult score;
        assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);
        if (played.perma_bonus != 5 * test->repetitions)
            fprintf(stderr, "retrigger case %s: got %d Hiker chips, expected %d\n", test->name, played.perma_bonus, 5 * test->repetitions);
        assert(played.perma_bonus == 5 * test->repetitions);
    }

    /* Hanging Chad only repeats the first scoring card. Hack repeats both
       rank-2 cards, while the Red Seal here belongs only to the second card. */
    BalatroState positional;
    assert(balatro_init(&positional, &config, 1) == BALATRO_OK);
    positional.joker_count = 3;
    positional.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    positional.jokers[1].center_id = BALATRO_CENTER_J_HACK;
    positional.jokers[2].center_id = BALATRO_CENTER_J_HIKER;
    BalatroCard pair[2] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_DIAMONDS, .seal = BALATRO_SEAL_RED},
    };
    BalatroScoreResult score;
    assert(balatro_score_hand(&positional, pair, 2, NULL, 0, &score) == BALATRO_OK);
    assert(pair[0].perma_bonus == 20);
    assert(pair[1].perma_bonus == 15);
}

static void test_source_retrigger_card_effect_order(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    BalatroScoreResult score;
    BalatroCard played;

#define RESET_CHAD_CARD(enhancement_value, edition_value, seal_value)                                                                      \
    do {                                                                                                                                   \
        assert(balatro_init(&state, &config, 1) == BALATRO_OK);                                                                            \
        state.joker_count = 1;                                                                                                             \
        state.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;                                                                         \
        played = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE,                                                                         \
                               .rank = 9,                                                                                                  \
                               .suit = BALATRO_CLUBS,                                                                                      \
                               .enhancement = (enhancement_value),                                                                         \
                               .edition = (edition_value),                                                                                 \
                               .seal = (seal_value)};                                                                                      \
        assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);                                                     \
    } while (0)

    /* Four evaluations: the ordinary pass, Red Seal, and two Chad repeats. */
    RESET_CHAD_CARD(BALATRO_ENHANCEMENT_BONUS, BALATRO_EDITION_NONE, BALATRO_SEAL_RED);
    assert(score.chips == 161.0 && score.mult == 1.0);
    RESET_CHAD_CARD(BALATRO_ENHANCEMENT_MULT, BALATRO_EDITION_NONE, BALATRO_SEAL_RED);
    assert(score.chips == 41.0 && score.mult == 17.0);
    RESET_CHAD_CARD(BALATRO_ENHANCEMENT_STONE, BALATRO_EDITION_NONE, BALATRO_SEAL_RED);
    assert(score.chips == 205.0 && score.mult == 1.0);
    RESET_CHAD_CARD(BALATRO_ENHANCEMENT_NONE, BALATRO_EDITION_FOIL, BALATRO_SEAL_RED);
    assert(score.chips == 241.0 && score.mult == 1.0);
    RESET_CHAD_CARD(BALATRO_ENHANCEMENT_NONE, BALATRO_EDITION_HOLO, BALATRO_SEAL_RED);
    assert(score.chips == 41.0 && score.mult == 41.0);
    RESET_CHAD_CARD(BALATRO_ENHANCEMENT_NONE, BALATRO_EDITION_POLYCHROME, BALATRO_SEAL_RED);
    assert(score.chips == 41.0 && fabs(score.mult - 5.0625) < 1e-12);

    /* Gold Seal is mutually exclusive with Red Seal, but Chad repeats its
       played-card dollar hook on all three evaluations. */
    RESET_CHAD_CARD(BALATRO_ENHANCEMENT_NONE, BALATRO_EDITION_NONE, BALATRO_SEAL_GOLD);
    assert(score.chips == 32.0 && score.dollars == 9);

    /* Oops makes Glass destruction certain, but the one destruction check is
       deferred until all four Glass x2 evaluations have completed. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[1].center_id = BALATRO_CENTER_J_OOPS;
    state.jokers[2].center_id = BALATRO_CENTER_J_OOPS;
    played = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE,
                           .rank = 9,
                           .suit = BALATRO_CLUBS,
                           .enhancement = BALATRO_ENHANCEMENT_GLASS,
                           .seal = BALATRO_SEAL_RED};
    assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);
    assert(score.chips == 41.0 && score.mult == 16.0 && score.total == 656);
    assert(score.destroyed_mask == 1);

    /* Four Oops Jokers make both Lucky rolls certain. Chad evaluates the
       Lucky card three times, and Lucky Cat observes all three triggers. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 6;
    state.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    for (uint8_t i = 1; i <= 4; ++i) state.jokers[i].center_id = BALATRO_CENTER_J_OOPS;
    state.jokers[5].center_id = BALATRO_CENTER_J_LUCKY_CAT;
    played = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 7, .suit = BALATRO_CLUBS, .enhancement = BALATRO_ENHANCEMENT_LUCKY};
    assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);
    if (score.chips != 26.0 || score.mult != 106.75 || score.dollars != 60)
        fprintf(stderr, "Lucky retrigger result: chips=%g mult=%g dollars=%d cat=%d\n", score.chips, score.mult, score.dollars,
                state.jokers[5].state[0]);
    /* Lucky adds +60 Mult, then Lucky Cat's updated x1.75 applies at main. */
    assert(score.chips == 26.0 && score.mult == 106.75 && score.dollars == 60);
    assert(state.jokers[5].state[0] == 175);

    /* Probability and per-card dollar Jokers are individual hooks, so they
       run once for every Chad evaluation as well. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[1].center_id = BALATRO_CENTER_J_OOPS;
    state.jokers[2].center_id = BALATRO_CENTER_J_BLOODSTONE;
    played = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_HEARTS};
    assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);
    assert(fabs(score.mult - 3.375) < 1e-12);

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[1].center_id = BALATRO_CENTER_J_ROUGH_GEM;
    played.suit = BALATRO_DIAMONDS;
    assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);
    assert(score.dollars == 3);

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[1].center_id = BALATRO_CENTER_J_OOPS;
    state.jokers[2].center_id = BALATRO_CENTER_J_BUSINESS;
    played.rank = 13;
    assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);
    assert(score.dollars == 6);

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[1].center_id = BALATRO_CENTER_J_TICKET;
    played =
        (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_DIAMONDS, .enhancement = BALATRO_ENHANCEMENT_GOLD};
    assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);
    assert(score.dollars == 12);

    /* Midas/Vampire are before hooks: they mutate a card once before any
       repetition, then all repetitions observe the resulting enhancement. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_MIDAS_MASK;
    state.jokers[1].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[2].center_id = BALATRO_CENTER_J_TICKET;
    played = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 12, .suit = BALATRO_HEARTS};
    assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);
    assert(played.enhancement == BALATRO_ENHANCEMENT_GOLD && score.dollars == 12);

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_VAMPIRE;
    state.jokers[1].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    played = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_HEARTS, .enhancement = BALATRO_ENHANCEMENT_BONUS};
    assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);
    assert(played.enhancement == BALATRO_ENHANCEMENT_NONE);
    assert(state.jokers[0].state[0] == 110 && score.chips == 32.0);

#undef RESET_CHAD_CARD
}

static void test_source_retrigger_eligibility_boundaries(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    BalatroScoreResult score;

    /* Chad targets the first card in the scoring hand, not the first card
       submitted by the player. Only the Ace scores in this High Card hand. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[1].center_id = BALATRO_CENTER_J_HIKER;
    BalatroCard high[2] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS},
    };
    assert(balatro_score_hand(&state, high, 2, NULL, 0, &score) == BALATRO_OK);
    assert(high[0].perma_bonus == 0 && high[1].perma_bonus == 15);

    /* Splash puts both cards in the scoring hand; Chad still repeats only
       the first one in play order. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[1].center_id = BALATRO_CENTER_J_SPLASH;
    state.jokers[2].center_id = BALATRO_CENTER_J_HIKER;
    BalatroCard splash[2] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_HEARTS},
    };
    assert(balatro_score_hand(&state, splash, 2, NULL, 0, &score) == BALATRO_OK);
    assert(splash[0].perma_bonus == 15 && splash[1].perma_bonus == 5);

    /* Hack includes ranks 2 through 5, and excludes Ace and 6. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_SPLASH;
    state.jokers[1].center_id = BALATRO_CENTER_J_HACK;
    state.jokers[2].center_id = BALATRO_CENTER_J_HIKER;
    BalatroCard ranks[4] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 5, .suit = BALATRO_DIAMONDS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 6, .suit = BALATRO_SPADES},
    };
    assert(balatro_score_hand(&state, ranks, 4, NULL, 0, &score) == BALATRO_OK);
    assert(ranks[0].perma_bonus == 5 && ranks[1].perma_bonus == 10);
    assert(ranks[2].perma_bonus == 10 && ranks[3].perma_bonus == 5);

    /* Pareidolia makes an ordinary 9 eligible for Sock and Buskin. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_SOCK_AND_BUSKIN;
    state.jokers[1].center_id = BALATRO_CENTER_J_PAREIDOLIA;
    state.jokers[2].center_id = BALATRO_CENTER_J_HIKER;
    BalatroCard nine = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS};
    assert(balatro_score_hand(&state, &nine, 1, NULL, 0, &score) == BALATRO_OK);
    assert(nine.perma_bonus == 10);

    /* Card and source debuffs suppress their corresponding evaluations. */
    BalatroCard king = {.center_id = BALATRO_CENTER_C_BASE, .rank = 13, .suit = BALATRO_HEARTS, .flags = BALATRO_CARD_DEBUFFED};
    assert(balatro_score_hand(&state, &king, 1, NULL, 0, &score) == BALATRO_OK);
    assert(king.perma_bonus == 0);
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_SOCK_AND_BUSKIN, .flags = BALATRO_CARD_DEBUFFED};
    state.jokers[1].center_id = BALATRO_CENTER_J_HIKER;
    king.flags = 0;
    assert(balatro_score_hand(&state, &king, 1, NULL, 0, &score) == BALATRO_OK);
    assert(king.perma_bonus == 5);

    /* A debuffed copier contributes nothing; a debuffed target suppresses
       both its physical evaluation and Blueprint's copied evaluation. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_BLUEPRINT, .flags = BALATRO_CARD_DEBUFFED};
    state.jokers[1].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[2].center_id = BALATRO_CENTER_J_HIKER;
    nine.perma_bonus = 0;
    assert(balatro_score_hand(&state, &nine, 1, NULL, 0, &score) == BALATRO_OK);
    assert(nine.perma_bonus == 15);
    state.jokers[0].flags = 0;
    state.jokers[1].flags = BALATRO_CARD_DEBUFFED;
    nine.perma_bonus = 0;
    assert(balatro_score_hand(&state, &nine, 1, NULL, 0, &score) == BALATRO_OK);
    assert(nine.perma_bonus == 5);

    /* Physical copies stack. Blueprint/Brainstorm cycles terminate without
       producing a copied effect. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[1].center_id = BALATRO_CENTER_J_HANGING_CHAD;
    state.jokers[2].center_id = BALATRO_CENTER_J_HIKER;
    nine.perma_bonus = 0;
    assert(balatro_score_hand(&state, &nine, 1, NULL, 0, &score) == BALATRO_OK);
    assert(nine.perma_bonus == 25);
    state.jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
    state.jokers[1].center_id = BALATRO_CENTER_J_BRAINSTORM;
    nine.perma_bonus = 0;
    assert(balatro_score_hand(&state, &nine, 1, NULL, 0, &score) == BALATRO_OK);
    assert(nine.perma_bonus == 5);
}

static void test_source_held_retrigger_interactions(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    BalatroScoreResult score;
    BalatroCard played = {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS};

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_MIME;
    BalatroCard steel = {.center_id = BALATRO_CENTER_C_BASE,
                         .rank = 9,
                         .suit = BALATRO_HEARTS,
                         .enhancement = BALATRO_ENHANCEMENT_STEEL,
                         .seal = BALATRO_SEAL_RED};
    assert(balatro_score_hand(&state, &played, 1, &steel, 1, &score) == BALATRO_OK);
    assert(fabs(score.mult - 3.375) < 1e-12);

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_MIME;
    state.jokers[1].center_id = BALATRO_CENTER_J_BARON;
    BalatroCard king = {.center_id = BALATRO_CENTER_C_BASE, .rank = 13, .suit = BALATRO_HEARTS, .seal = BALATRO_SEAL_RED};
    assert(balatro_score_hand(&state, &played, 1, &king, 1, &score) == BALATRO_OK);
    assert(fabs(score.mult - 3.375) < 1e-12);

    state.jokers[1].center_id = BALATRO_CENTER_J_SHOOT_THE_MOON;
    BalatroCard queen = {.center_id = BALATRO_CENTER_C_BASE, .rank = 12, .suit = BALATRO_HEARTS, .seal = BALATRO_SEAL_RED};
    assert(balatro_score_hand(&state, &played, 1, &queen, 1, &score) == BALATRO_OK);
    assert(score.mult == 40.0);

    /* Blueprint + physical Mime gives three held evaluations without a Red
       Seal: ordinary, copied Mime, then physical Mime. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
    state.jokers[1].center_id = BALATRO_CENTER_J_MIME;
    steel.seal = BALATRO_SEAL_NONE;
    assert(balatro_score_hand(&state, &played, 1, &steel, 1, &score) == BALATRO_OK);
    assert(fabs(score.mult - 3.375) < 1e-12);
    state.jokers[0].center_id = BALATRO_CENTER_J_MIME;
    state.jokers[1].center_id = BALATRO_CENTER_J_BRAINSTORM;
    assert(balatro_score_hand(&state, &played, 1, &steel, 1, &score) == BALATRO_OK);
    assert(fabs(score.mult - 3.375) < 1e-12);

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
    state.jokers[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_MIME, .flags = BALATRO_CARD_DEBUFFED};
    assert(balatro_score_hand(&state, &played, 1, &steel, 1, &score) == BALATRO_OK);
    assert(fabs(score.mult - 1.5) < 1e-12);
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_BLUEPRINT, .flags = BALATRO_CARD_DEBUFFED};
    state.jokers[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_MIME};
    assert(balatro_score_hand(&state, &played, 1, &steel, 1, &score) == BALATRO_OK);
    assert(fabs(score.mult - 2.25) < 1e-12);

    /* A debuffed held card has no effect. A debuffed Mime leaves only the
       Red Seal's two Steel evaluations. */
    steel.seal = BALATRO_SEAL_RED;
    steel.flags = BALATRO_CARD_DEBUFFED;
    assert(balatro_score_hand(&state, &played, 1, &steel, 1, &score) == BALATRO_OK);
    assert(score.mult == 1.0);
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 1;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_MIME, .flags = BALATRO_CARD_DEBUFFED};
    steel.flags = 0;
    assert(balatro_score_hand(&state, &played, 1, &steel, 1, &score) == BALATRO_OK);
    assert(fabs(score.mult - 2.25) < 1e-12);

    BalatroCard base = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_HEARTS, .seal = BALATRO_SEAL_RED};
    state.jokers[0].flags = 0;
    assert(balatro_score_hand(&state, &played, 1, &base, 1, &score) == BALATRO_OK);
    assert(score.mult == 1.0 && score.dollars == 0);
}

static void test_source_end_round_copied_mime(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    BalatroState gold[2];
    for (int copied = 0; copied < 2; ++copied) {
        assert(balatro_init(&gold[copied], &config, 1) == BALATRO_OK);
        BalatroState *state = &gold[copied];
        state->phase = BALATRO_PHASE_SELECTING_HAND;
        state->blind_id = BALATRO_BLIND_BL_SMALL;
        state->blind_chips = 1;
        state->hand_count = 2;
        state->hand_size = 2;
        state->hands_left = 1;
        state->discards_left = 0;
        state->deck_count = 0;
        state->hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
        state->hand[1] =
            (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .enhancement = BALATRO_ENHANCEMENT_GOLD};
        if (copied) {
            state->joker_count = 2;
            state->jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
            state->jokers[1].center_id = BALATRO_CENTER_J_MIME;
        }
        assert(balatro_step(state, &play, &result) == BALATRO_OK);
        assert(state->phase == BALATRO_PHASE_ROUND_EVAL);
    }
    assert(gold[1].dollars == gold[0].dollars + 6);
    assert(gold[1].round_earnings == gold[0].round_earnings + 1);

    BalatroState blue[2];
    for (int copied = 0; copied < 2; ++copied) {
        assert(balatro_init(&blue[copied], &config, 1) == BALATRO_OK);
        BalatroState *state = &blue[copied];
        state->phase = BALATRO_PHASE_SELECTING_HAND;
        state->blind_id = BALATRO_BLIND_BL_SMALL;
        state->blind_chips = 1;
        state->hand_count = 2;
        state->hand_size = 2;
        state->hands_left = 1;
        state->discards_left = 0;
        state->deck_count = 0;
        state->consumable_slots = 5;
        state->hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
        state->hand[1] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .seal = BALATRO_SEAL_BLUE};
        if (copied) {
            state->joker_count = 2;
            state->jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
            state->jokers[1].center_id = BALATRO_CENTER_J_MIME;
        }
        assert(balatro_step(state, &play, &result) == BALATRO_OK);
        assert(state->phase == BALATRO_PHASE_ROUND_EVAL);
    }
    assert(blue[0].consumable_count == 1 && blue[1].consumable_count == 3);
}

static void test_source_retrigger_lifecycle_persistence(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.blind_chips = 100000;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 2;
    state.discards_left = 0;
    state.deck_count = 1;
    state.deck[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 3, .suit = BALATRO_DIAMONDS, .sort_id = 2};
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .sort_id = 1};
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_HIKER;
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.discard_count == 1 && state.discard[0].sort_id == 1);
    assert(state.discard[0].perma_bonus == 5);
    uint8_t snapshot[sizeof(BalatroState) + 64];
    size_t size = balatro_serialize(&state, snapshot, sizeof(snapshot));
    BalatroState restored;
    assert(size > 0 && balatro_deserialize(&restored, snapshot, size) == BALATRO_OK);
    assert(restored.discard[0].perma_bonus == 5);

    /* Seltzer's copied repetition effect does not make its physical card age
       more than once at the end-of-round boundary. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.blind_chips = 1;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 1;
    state.deck_count = 0;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
    state.jokers[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_SELZER, .state = {2, 0, 0, 0}};
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.joker_count == 2 && state.jokers[1].state[0] == 1);

    /* A Hiker-upgraded Glass card that shatters is removed entirely; its
       permanent bonus must not survive in another card slot. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.blind_chips = 100000;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 2;
    state.deck_count = 1;
    state.deck[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 3, .suit = BALATRO_DIAMONDS, .sort_id = 2};
    state.hand[0] = (BalatroCard){
        .center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .enhancement = BALATRO_ENHANCEMENT_GLASS, .sort_id = 1};
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_HIKER;
    state.jokers[1].center_id = BALATRO_CENTER_J_OOPS;
    state.jokers[2].center_id = BALATRO_CENTER_J_OOPS;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    for (uint8_t i = 0; i < state.hand_count; ++i) assert(state.hand[i].sort_id != 1);
    for (uint16_t i = 0; i < state.deck_count; ++i) assert(state.deck[i].sort_id != 1);
    for (uint16_t i = 0; i < state.discard_count; ++i) assert(state.discard[i].sort_id != 1);
}

static void test_source_joker_activation_scoring_gaps(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    BalatroScoreResult score;
    BalatroCard nine = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS};

#define RESET_ONE_JOKER(center)                                                                                                            \
    do {                                                                                                                                   \
        assert(balatro_init(&state, &config, 1) == BALATRO_OK);                                                                            \
        state.joker_count = 1;                                                                                                             \
        state.jokers[0].center_id = (center);                                                                                              \
    } while (0)
#define SCORE_NINE() assert(balatro_score_hand(&state, &nine, 1, NULL, 0, &score) == BALATRO_OK)

    RESET_ONE_JOKER(BALATRO_CENTER_J_FORTUNE_TELLER);
    state.tarots_used = 3;
    SCORE_NINE();
    assert(score.mult == 4.0);

    RESET_ONE_JOKER(BALATRO_CENTER_J_BOOTSTRAPS);
    state.dollars = 10;
    SCORE_NINE();
    assert(score.mult == 5.0);

    RESET_ONE_JOKER(BALATRO_CENTER_J_STONE);
    state.deck_count = 1;
    state.deck[0].enhancement = BALATRO_ENHANCEMENT_STONE;
    SCORE_NINE();
    assert(score.chips == 39.0);

    RESET_ONE_JOKER(BALATRO_CENTER_J_EROSION);
    state.deck_count = 50;
    state.hand_count = 0;
    state.discard_count = 0;
    SCORE_NINE();
    assert(score.mult == 9.0);

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BASEBALL;
    state.jokers[1].center_id = BALATRO_CENTER_J_ACROBAT;
    state.hands_left = 2;
    SCORE_NINE();
    if (fabs(score.mult - 1.5) >= 1e-12) fprintf(stderr, "Baseball activation mult=%g hands_left=%u\n", score.mult, state.hands_left);
    assert(fabs(score.mult - 1.5) < 1e-12);

    RESET_ONE_JOKER(BALATRO_CENTER_J_STEEL_JOKER);
    state.deck_count = 1;
    state.deck[0].enhancement = BALATRO_ENHANCEMENT_STEEL;
    SCORE_NINE();
    assert(fabs(score.mult - 1.2) < 1e-12);

    RESET_ONE_JOKER(BALATRO_CENTER_J_DRIVERS_LICENSE);
    state.deck_count = 16;
    for (uint16_t i = 0; i < state.deck_count; ++i) state.deck[i].enhancement = BALATRO_ENHANCEMENT_BONUS;
    SCORE_NINE();
    assert(score.mult == 3.0);

    RESET_ONE_JOKER(BALATRO_CENTER_J_OBELISK);
    state.hand_plays[BALATRO_PAIR] = 1;
    SCORE_NINE();
    assert(state.jokers[0].state[0] == 120 && fabs(score.mult - 1.2) < 1e-12);

    RESET_ONE_JOKER(BALATRO_CENTER_J_FLASH);
    state.jokers[0].state[0] = 7;
    SCORE_NINE();
    assert(score.mult == 8.0);
    RESET_ONE_JOKER(BALATRO_CENTER_J_RED_CARD);
    state.jokers[0].state[0] = 9;
    SCORE_NINE();
    assert(score.mult == 10.0);

    static const uint16_t stateful_xmult[] = {
        BALATRO_CENTER_J_CAMPFIRE, BALATRO_CENTER_J_CONSTELLATION, BALATRO_CENTER_J_HOLOGRAM, BALATRO_CENTER_J_LUCKY_CAT,
        BALATRO_CENTER_J_GLASS,    BALATRO_CENTER_J_CAINO,         BALATRO_CENTER_J_MADNESS,  BALATRO_CENTER_J_YORICK,
    };
    for (size_t i = 0; i < sizeof(stateful_xmult) / sizeof(stateful_xmult[0]); ++i) {
        RESET_ONE_JOKER(stateful_xmult[i]);
        state.jokers[0].state[0] = 150;
        SCORE_NINE();
        assert(fabs(score.mult - 1.5) < 1e-12);
    }

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_SWASHBUCKLER;
    state.jokers[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_JOKER, .sell_cost = 7};
    SCORE_NINE();
    assert(score.mult == 12.0); /* Swashbuckler +7, then Joker +4. */

    RESET_ONE_JOKER(BALATRO_CENTER_J_THROWBACK);
    state.skips = 2;
    SCORE_NINE();
    assert(fabs(score.mult - 1.5) < 1e-12);

    RESET_ONE_JOKER(BALATRO_CENTER_J_BLACKBOARD);
    BalatroCard held = {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_SPADES};
    assert(balatro_score_hand(&state, &nine, 1, &held, 1, &score) == BALATRO_OK);
    assert(score.mult == 3.0);

    RESET_ONE_JOKER(BALATRO_CENTER_J_IDOL);
    state.jokers[0].state[0] = 9;
    state.jokers[0].state[1] = BALATRO_CLUBS;
    SCORE_NINE();
    assert(score.mult == 2.0);

    /* context.poker_hands exposes every matching category. This black Flush
       made possible by Smeared also contains a duplicated 8, so Sly must
       activate even though Flush is the highest-scoring hand. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_ARROWHEAD;
    state.jokers[1].center_id = BALATRO_CENTER_J_SLY;
    state.jokers[2].center_id = BALATRO_CENTER_J_SMEARED;
    BalatroCard smeared_flush_pair[5] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 13, .suit = BALATRO_SPADES},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 11, .suit = BALATRO_SPADES},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_SPADES},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 8, .suit = BALATRO_SPADES},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 8, .suit = BALATRO_CLUBS},
    };
    assert(balatro_score_hand(&state, smeared_flush_pair, 5, NULL, 0, &score) == BALATRO_OK);
    assert(score.hand_type == BALATRO_FLUSH);
    assert(score.chips == 380.0 && score.mult == 4.0 && score.total == 1520);

    RESET_ONE_JOKER(BALATRO_CENTER_J_CARD_SHARP);
    state.hand_plays_round[BALATRO_HIGH_CARD] = 1;
    SCORE_NINE();
    assert(score.mult == 3.0);

    RESET_ONE_JOKER(BALATRO_CENTER_J_ACROBAT);
    state.hands_left = 0;
    SCORE_NINE();
    assert(score.mult == 3.0);

    RESET_ONE_JOKER(BALATRO_CENTER_J_CEREMONIAL);
    state.jokers[0].state[0] = 6;
    SCORE_NINE();
    assert(score.mult == 7.0);

    RESET_ONE_JOKER(BALATRO_CENTER_J_SHORTCUT);
    BalatroCard shortcut_cards[5] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_HEARTS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 4, .suit = BALATRO_DIAMONDS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 6, .suit = BALATRO_CLUBS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 8, .suit = BALATRO_SPADES},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 10, .suit = BALATRO_HEARTS},
    };
    assert(balatro_score_hand(&state, shortcut_cards, 5, NULL, 0, &score) == BALATRO_OK);
    assert(score.hand_type == BALATRO_STRAIGHT && score.total == 240);

#undef SCORE_NINE
#undef RESET_ONE_JOKER
}

static void test_source_copied_held_joker_hook(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
    state.jokers[1].center_id = BALATRO_CENTER_J_BARON;
    BalatroCard played = {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS};
    BalatroCard held = {.center_id = BALATRO_CENTER_C_BASE, .rank = 13, .suit = BALATRO_HEARTS};
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &played, 1, &held, 1, &score) == BALATRO_OK);
    /* Baron is evaluated once at its own position and once through the
       preceding Blueprint: High Card's x1 becomes 1.5^2. */
    assert(fabs(score.mult - 2.25) < 1e-12);
}

static void test_source_raised_fist_selects_before_debuff(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_RAISED_FIST;
    BalatroCard played = {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_HEARTS};
    BalatroCard held[2] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 8, .suit = BALATRO_CLUBS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 3, .suit = BALATRO_SPADES, .flags = 1u},
    };
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &played, 1, held, 2, &score) == BALATRO_OK);
    /* Raised Fist selects the debuffed 3, then suppresses its held-card
       effect.  It must not retarget the 8 and grant +16 Mult. */
    assert(score.mult == 1.0);

    held[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 3, .suit = BALATRO_CLUBS};
    /* The target scan uses >= while walking left to right, so the rightmost tied 3
       remains the target.  Since that card is debuffed, the result is still
       suppressed rather than falling back to the left 3. */
    assert(balatro_score_hand(&state, &played, 1, held, 2, &score) == BALATRO_OK);
    assert(score.mult == 1.0);
}

static void test_source_copied_joker_self_exclusion(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_BLUEPRINT, .sell_cost = 7};
    state.jokers[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_SWASHBUCKLER, .sell_cost = 10};
    BalatroCard played = {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS};
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &played, 1, NULL, 0, &score) == BALATRO_OK);
    /* Both the copied and physical Swashbuckler exclude the Swashbuckler
       card itself; each sees only the Blueprint's $7 sell value. */
    assert(score.mult == 15.0);
}

static void test_source_copied_before_state_suppression(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    BalatroScoreResult score;
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS};
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
    state.jokers[1].center_id = BALATRO_CENTER_J_GREEN_JOKER;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    /* Green Joker's before hook is suppressed for Blueprint's copied pass;
       only the physical Joker upgrades from 0 to +1 mult. */
    assert(state.jokers[1].state[0] == 1 && score.mult == 2.0);

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
    state.jokers[1].center_id = BALATRO_CENTER_J_SQUARE;
    BalatroCard four[4] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 4, .suit = BALATRO_DIAMONDS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 6, .suit = BALATRO_HEARTS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 8, .suit = BALATRO_SPADES},
    };
    assert(balatro_score_hand(&state, four, 4, NULL, 0, &score) == BALATRO_OK);
    /* Square Joker's +4 upgrade occurs once, not once for the copy and once
       for the physical card. */
    assert(state.jokers[1].state[0] == 4 && score.chips == 17.0);
}

static void test_source_joker_on_joker_order(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BASEBALL;
    state.jokers[1].center_id = BALATRO_CENTER_J_BOOTSTRAPS;
    state.dollars = 10;
    BalatroCard played[2] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_HEARTS},
    };
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, played, 2, NULL, 0, &score) == BALATRO_OK);
    /* Bootstraps' +4 is applied before Baseball's other_joker x1.5 effect. */
    assert(fabs(score.mult - 9.0) < 1e-12);
}

static void test_source_joker_edition_order(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroCard played[2] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_HEARTS},
    };
    BalatroScoreResult score;
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.dollars = 10;
    state.joker_count = 2;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_BOOTSTRAPS, .edition = 3};
    state.jokers[1].center_id = BALATRO_CENTER_J_JOKER;
    assert(balatro_score_hand(&state, played, 2, NULL, 0, &score) == BALATRO_OK);
    /* Polychrome applies after Bootstraps' additive main hook. */
    assert(fabs(score.mult - 13.0) < 1e-12);

    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.dollars = 10;
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
    state.jokers[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_BOOTSTRAPS, .edition = 3};
    assert(balatro_score_hand(&state, played, 2, NULL, 0, &score) == BALATRO_OK);
    /* Blueprint copies Bootstraps' main hook, not its Polychrome edition. */
    assert(fabs(score.mult - 15.0) < 1e-12);
}

static void test_source_dna_before_scoring(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.deck_count = 0;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 1;
    state.discards_left = 0;
    state.blind_chips = 100000;
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_DNA;
    state.jokers[1].center_id = BALATRO_CENTER_J_HOLOGRAM;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .sort_id = 1};
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    /* DNA's copy remains in hand while the played card is scored; Hologram
       therefore receives the card-added hook before Joker-main evaluation. */
    assert(state.hand_count == 1 && state.deck_count == 0);
    assert(state.jokers[1].state[0] == 125);
}

static void test_source_eight_ball_retriggers(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_8_BALL;
    state.jokers[1].center_id = BALATRO_CENTER_J_OOPS;
    state.jokers[2].center_id = BALATRO_CENTER_J_OOPS;
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 8, .suit = BALATRO_CLUBS, .seal = 2};
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    /* Two Oops Jokers make the 8 Ball probability certain; Red Seal causes
       two individual hooks, hence two Tarot creations. */
    assert(state.consumable_count == 2);
}

static void test_source_pack_consumable_use(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_PACK_OPENING;
    state.pack_count = 1;
    state.pack_choices = 1;
    state.dollars = 10;
    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_HERMIT;
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    BalatroStepResult result;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.dollars == 20 && state.consumable_count == 0);
}

static void test_source_emperor_reuses_named_stream(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "MANUAL_RUN_3") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.ante = 3;
    state.consumable_slots = 2;
    state.consumable_count = 1;
    state.consumables[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_EMPEROR, .sort_id = ++state.next_sort_id};
    state.joker_count = 1;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_HOLOGRAM, .cost = 7, .sell_cost = 3};
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    BalatroStepResult result;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    /* Emperor queues two create-card operations with append='emp'. With the run's
       seed and Ante 3, consecutive Tarotemp3 draws are Wheel then Moon. */
    assert(state.consumable_count == 2);
    assert(state.consumables[0].center_id == BALATRO_CENTER_C_WHEEL_OF_FORTUNE);
    assert(state.consumables[1].center_id == BALATRO_CENTER_C_MOON);

    use.primary = 0;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.jokers[0].edition == BALATRO_EDITION_FOIL);
    /* set_edition ends by calling set_cost: Hologram's $7 base plus Foil's
       $2 edition cost becomes $9, with a $4 sell value. */
    assert(state.jokers[0].cost == 9 && state.jokers[0].sell_cost == 4);
}

static void test_source_pack_generators_respect_consumable_capacity(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    const uint16_t generators[] = {
        BALATRO_CENTER_C_EMPEROR,
        BALATRO_CENTER_C_HIGH_PRIESTESS,
        BALATRO_CENTER_C_FOOL,
    };
    for (size_t i = 0; i < sizeof(generators) / sizeof(generators[0]); ++i) {
        BalatroState state;
        assert(balatro_init_seed_string(&state, &config, "PACK_GENERATOR_CAP") == BALATRO_OK);
        state.phase = BALATRO_PHASE_PACK_OPENING;
        state.shop_return_phase = BALATRO_PHASE_SHOP;
        state.pack_choices = 1;
        state.pack_count = 1;
        state.pack_cards[0].center_id = generators[i];
        state.consumable_slots = 2;
        state.consumable_count = 1;
        state.consumables[0].center_id = BALATRO_CENTER_C_HEIROPHANT;
        state.last_tarot_planet = BALATRO_CENTER_C_MERCURY;
        BalatroAction pick = {.type = BALATRO_ACTION_PICK_PACK_CARD, .primary = 0};
        BalatroStepResult result;
        assert(balatro_step(&state, &pick, &result) == BALATRO_OK);
        assert(state.phase == BALATRO_PHASE_SHOP);
        assert(state.consumable_count == 2);
        assert(state.consumable_count <= state.consumable_slots);
    }
}

static void test_source_hallucination_open_booster_context(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    BalatroStepResult result;
    BalatroAction open = {.type = BALATRO_ACTION_OPEN_BOOSTER, .primary = 0};

    assert(balatro_init_seed_string(&state, &config, "HEURISTIC2") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.ante = 3;
    state.dollars = 20;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_HALLUCINATION;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_P_CELESTIAL_MEGA_1, .cost = 8};
    assert(balatro_step(&state, &open, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_PACK_OPENING);
    assert(state.consumable_count == 1);

    assert(balatro_init_seed_string(&state, &config, "HALLUCINATION_COPIES") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.ante = 3;
    state.dollars = 20;
    state.consumable_slots = 4;
    state.joker_count = 4;
    state.jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
    state.jokers[1].center_id = BALATRO_CENTER_J_HALLUCINATION;
    state.jokers[2].center_id = BALATRO_CENTER_J_HALLUCINATION;
    state.jokers[3].center_id = BALATRO_CENTER_J_OOPS;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_P_CELESTIAL_MEGA_1, .cost = 8};
    assert(balatro_step(&state, &open, &result) == BALATRO_OK);
    assert(state.consumable_count == 3);

    assert(balatro_init_seed_string(&state, &config, "HALLUCINATION_BRAINSTORM") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.ante = 3;
    state.dollars = 20;
    state.consumable_slots = 3;
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_HALLUCINATION;
    state.jokers[1].center_id = BALATRO_CENTER_J_BRAINSTORM;
    state.jokers[2].center_id = BALATRO_CENTER_J_OOPS;
    state.shop_count = 1;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_P_CELESTIAL_MEGA_1, .cost = 8};
    assert(balatro_step(&state, &open, &result) == BALATRO_OK);
    assert(state.consumable_count == 2);
}

static void test_source_blind_random_card_ordering(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroStepResult result;

    /* Hook copies the visually sorted hand into a temporary table, but
       pseudorandom_element reorders those Card objects by sort_id. */
    BalatroState hook;
    assert(balatro_init_seed_string(&hook, &config, "HOOK_SORT") == BALATRO_OK);
    hook.phase = BALATRO_PHASE_SELECTING_HAND;
    hook.blind_id = BALATRO_BLIND_BL_HOOK;
    hook.blind_chips = 1000000000;
    hook.hands_left = 4;
    hook.deck_count = hook.discard_count = 0;
    hook.hand_count = 5;
    const uint16_t hook_ids[] = {50, 40, 10, 30, 20};
    for (uint8_t i = 0; i < hook.hand_count; ++i)
        hook.hand[i] =
            (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = (uint8_t)(14 - i), .suit = BALATRO_SPADES, .sort_id = hook_ids[i]};
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&hook, &play, &result) == BALATRO_OK);
    assert(hook.discard_count == 3);
    /* Rolls 0.049... and 0.994... choose the lowest then highest
       creation IDs, not visual hand indices 0 then 3. */
    assert(hook.discard[1].sort_id == 10);
    assert(hook.discard[2].sort_id == 40);

    /* Crimson Heart uses the same creation ordering for Jokers.  On the next
       hand it excludes the currently debuffed card before sorting again. */
    BalatroState heart;
    assert(balatro_init_seed_string(&heart, &config, "MANUAL_RUN_4") == BALATRO_OK);
    heart.blind_on_deck = 2;
    heart.next_boss_id = BALATRO_BLIND_BL_FINAL_HEART;
    heart.joker_count = 6;
    const uint16_t heart_ids[] = {247, 205, 122, 311, 224, 151};
    for (uint8_t i = 0; i < heart.joker_count; ++i)
        heart.jokers[i] = (BalatroCard){.center_id = BALATRO_CENTER_J_JOKER, .sort_id = heart_ids[i]};
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    assert(balatro_step(&heart, &select, &result) == BALATRO_OK);
    assert(heart.jokers[4].flags & BALATRO_CARD_DEBUFFED); /* Baron position in the capture. */
    for (uint8_t i = 0; i < heart.joker_count; ++i)
        if (i != 4) assert(!(heart.jokers[i].flags & BALATRO_CARD_DEBUFFED));
    heart.blind_chips = 1000000000;
    BalatroAction discard = {.type = BALATRO_ACTION_DISCARD, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&heart, &discard, &result) == BALATRO_OK);
    assert(heart.jokers[4].flags & BALATRO_CARD_DEBUFFED);
    for (uint8_t i = 0; i < heart.joker_count; ++i)
        if (i != 4) assert(!(heart.jokers[i].flags & BALATRO_CARD_DEBUFFED));
    play.selection[0] = 0;
    assert(balatro_step(&heart, &play, &result) == BALATRO_OK);
    assert(heart.jokers[1].flags & BALATRO_CARD_DEBUFFED);
    assert(!(heart.jokers[4].flags & BALATRO_CARD_DEBUFFED));
}

static void test_source_manacle_restores_before_shop_pack(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "MANACLE_RESTORE") == BALATRO_OK);
    state.blind_on_deck = 2;
    state.next_boss_id = BALATRO_BLIND_BL_MANACLE;
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.hand_size == 7 && state.hand_count == 7);
    state.blind_chips = 1;
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_ROUND_EVAL && state.hand_size == 8);

    /* Chicot disables the boss during setting_blind; the temporary penalty
       must never be applied or subsequently restored a second time. */
    assert(balatro_init_seed_string(&state, &config, "MANACLE_CHICOT") == BALATRO_OK);
    state.blind_on_deck = 2;
    state.next_boss_id = BALATRO_BLIND_BL_MANACLE;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_CHICOT;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.blind_disabled && state.hand_size == 8 && state.hand_count == 8);
}

static void test_source_all_boss_temporary_state_clears_on_defeat(void) {
    static const uint16_t bosses[] = {
        BALATRO_BLIND_BL_ARM,        BALATRO_BLIND_BL_CLUB,        BALATRO_BLIND_BL_EYE,        BALATRO_BLIND_BL_FISH,
        BALATRO_BLIND_BL_FLINT,      BALATRO_BLIND_BL_GOAD,        BALATRO_BLIND_BL_HEAD,       BALATRO_BLIND_BL_HOOK,
        BALATRO_BLIND_BL_HOUSE,      BALATRO_BLIND_BL_MANACLE,     BALATRO_BLIND_BL_MARK,       BALATRO_BLIND_BL_MOUTH,
        BALATRO_BLIND_BL_NEEDLE,     BALATRO_BLIND_BL_OX,          BALATRO_BLIND_BL_PILLAR,     BALATRO_BLIND_BL_PLANT,
        BALATRO_BLIND_BL_PSYCHIC,    BALATRO_BLIND_BL_SERPENT,     BALATRO_BLIND_BL_TOOTH,      BALATRO_BLIND_BL_WALL,
        BALATRO_BLIND_BL_WATER,      BALATRO_BLIND_BL_WHEEL,       BALATRO_BLIND_BL_WINDOW,     BALATRO_BLIND_BL_FINAL_ACORN,
        BALATRO_BLIND_BL_FINAL_BELL, BALATRO_BLIND_BL_FINAL_HEART, BALATRO_BLIND_BL_FINAL_LEAF, BALATRO_BLIND_BL_FINAL_VESSEL,
    };
    BalatroConfig config;
    balatro_default_config(&config);
    for (size_t boss_index = 0; boss_index < sizeof(bosses) / sizeof(bosses[0]); ++boss_index) {
        BalatroState state;
        assert(balatro_init_seed_string(&state, &config, "BOSS_CLEANUP") == BALATRO_OK);
        state.phase = BALATRO_PHASE_SELECTING_HAND;
        state.blind_on_deck = 2;
        state.blind_id = bosses[boss_index];
        state.next_boss_id = bosses[boss_index];
        state.blind_disabled = 0;
        state.blind_only_hand = UINT8_MAX;
        state.blind_hands_mask = 0;
        state.blind_chips = 1;
        state.hands_left = 1;
        state.deck_count = state.discard_count = 0;
        state.hand_count = 6;
        state.hand_size = bosses[boss_index] == BALATRO_BLIND_BL_MANACLE ? 7 : 8;
        for (uint8_t i = 0; i < state.hand_count; ++i)
            state.hand[i] = (BalatroCard){
                .center_id = BALATRO_CENTER_C_BASE, .rank = (uint8_t)(2 + i), .suit = (uint8_t)(i % 4), .sort_id = (uint16_t)(i + 1)};
        state.hand[0].flags = BALATRO_CARD_FORCED;
        state.hand[5].flags = BALATRO_CARD_DEBUFFED;
        state.joker_count = 1;
        state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_JOKER, .flags = BALATRO_CARD_DEBUFFED};
        BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 5, .selection = {0, 1, 2, 3, 4}};
        BalatroStepResult result;
        assert(balatro_step(&state, &play, &result) == BALATRO_OK);
        assert(state.phase == BALATRO_PHASE_ROUND_EVAL && state.blind_disabled);
        assert(state.hand_size == 8);
        assert(!(state.jokers[0].flags & BALATRO_CARD_DEBUFFED));
        for (uint16_t i = 0; i < state.deck_count; ++i) assert(!(state.deck[i].flags & (BALATRO_CARD_DEBUFFED | BALATRO_CARD_FORCED)));
    }
}

static void test_source_chicot_disables_setup_penalties(void) {
    static const uint16_t bosses[] = {
        BALATRO_BLIND_BL_WALL, BALATRO_BLIND_BL_FINAL_VESSEL, BALATRO_BLIND_BL_NEEDLE, BALATRO_BLIND_BL_WATER, BALATRO_BLIND_BL_MANACLE,
    };
    BalatroConfig config;
    balatro_default_config(&config);
    for (size_t i = 0; i < sizeof(bosses) / sizeof(bosses[0]); ++i) {
        BalatroState state;
        assert(balatro_init_seed_string(&state, &config, "CHICOT_SETUP") == BALATRO_OK);
        state.blind_on_deck = 2;
        state.next_boss_id = bosses[i];
        state.joker_count = 1;
        state.jokers[0].center_id = BALATRO_CENTER_J_CHICOT;
        double ordinary_target = balatro_blind_target(state.ante, state.blind_on_deck, state.stake_scaling);
        if (bosses[i] == BALATRO_BLIND_BL_NEEDLE) ordinary_target /= 2;
        BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
        BalatroStepResult result;
        assert(balatro_step(&state, &select, &result) == BALATRO_OK);
        assert(state.blind_disabled && state.blind_chips == ordinary_target);
        assert(state.hands_left == state.hands_per_round);
        assert(state.discards_left == state.discards_per_round);
        assert(state.hand_size == state.base_hand_size && state.hand_count == state.base_hand_size);
    }
}

static void test_source_sell_consumable_during_blind(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.dollars = 2;
    state.consumable_count = 1;
    state.consumables[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_MOON, .cost = 3, .sell_cost = 1};
    state.joker_count = 1;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_CAMPFIRE, .state = {100, 0, 0, 0}};
    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    BalatroAction sell = {0};
    int found = 0;
    for (int i = 0; i < count; ++i)
        if (actions[i].type == BALATRO_ACTION_SELL_CONSUMABLE && actions[i].primary == 0) sell = actions[i], found = 1;
    assert(found);
    BalatroStepResult result;
    assert(balatro_step(&state, &sell, &result) == BALATRO_OK);
    assert(state.consumable_count == 0 && state.dollars == 3);
    assert(state.jokers[0].state[0] == 125);
}

static void test_source_blind_select_inventory_actions(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_BLIND_SELECT;
    state.joker_count = 2;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_JOKER};
    state.jokers[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_GREEDY_JOKER};
    state.consumable_count = 1;
    state.consumables[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_URANUS};

    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    int sell_joker = 0, sell_consumable = 0, swap_jokers = 0;
    for (int i = 0; i < count; ++i) {
        if (actions[i].type == BALATRO_ACTION_SELL_JOKER) sell_joker = 1;
        if (actions[i].type == BALATRO_ACTION_SELL_CONSUMABLE) sell_consumable = 1;
        if (actions[i].type == BALATRO_ACTION_SWAP_JOKERS_LEFT || actions[i].type == BALATRO_ACTION_SWAP_JOKERS_RIGHT) swap_jokers = 1;
    }
    assert(sell_joker && sell_consumable && swap_jokers);
}

static void test_source_hand_sort_mode_persists_across_draw(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.blind_id = BALATRO_BLIND_BL_SMALL;
    state.blind_chips = 1000000;
    state.hands_left = 4;
    state.hand_size = state.hand_count = 8;
    state.deck_count = 5;
    state.discard_count = 0;
    BalatroCard initial[8] = {
        card(BALATRO_HEARTS, 12), card(BALATRO_HEARTS, 11), card(BALATRO_HEARTS, 10),   card(BALATRO_HEARTS, 7),
        card(BALATRO_HEARTS, 5),  card(BALATRO_HEARTS, 4),  card(BALATRO_DIAMONDS, 13), card(BALATRO_DIAMONDS, 9),
    };
    memcpy(state.hand, initial, sizeof(initial));
    state.deck[0] = card(BALATRO_CLUBS, 6);
    state.deck[1] = card(BALATRO_CLUBS, 9);
    state.deck[2] = card(BALATRO_CLUBS, 14);
    state.deck[3] = card(BALATRO_SPADES, 7);
    state.deck[4] = card(BALATRO_SPADES, 13);

    BalatroStepResult result;
    BalatroAction sort_suit = {.type = BALATRO_ACTION_SORT_HAND_SUIT};
    assert(balatro_step(&state, &sort_suit, &result) == BALATRO_OK);
    assert(state.hand_sort_suit == 1);
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 5, .selection = {0, 1, 2, 6, 7}};
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);

    static const uint8_t expected_suits[8] = {
        BALATRO_SPADES, BALATRO_SPADES, BALATRO_HEARTS, BALATRO_HEARTS, BALATRO_HEARTS, BALATRO_CLUBS, BALATRO_CLUBS, BALATRO_CLUBS,
    };
    static const uint8_t expected_ranks[8] = {13, 7, 7, 5, 4, 14, 9, 6};
    assert(state.hand_count == 8);
    for (uint8_t i = 0; i < state.hand_count; ++i) {
        assert(state.hand[i].suit == expected_suits[i]);
        assert(state.hand[i].rank == expected_ranks[i]);
    }
}

static void test_source_pack_hand_returns_to_deck_bottom(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_PACK_OPENING;
    state.shop_return_phase = BALATRO_PHASE_SHOP;
    state.pack_kind = 1;
    state.pack_count = 1;
    state.pack_choices = 1;
    state.deck_count = 2;
    state.deck[0].sort_id = 1;
    state.deck[1].sort_id = 2;
    state.hand_count = 3;
    state.hand[0].sort_id = 10;
    state.hand[1].sort_id = 11;
    state.hand[2].sort_id = 12;
    assert(balatro_skip_pack(&state) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_SHOP && state.hand_count == 0);
    assert(state.deck_count == 5);
    assert(state.deck[0].sort_id == 12 && state.deck[1].sort_id == 11 && state.deck[2].sort_id == 10 && state.deck[3].sort_id == 1 &&
           state.deck[4].sort_id == 2);
}

static void test_source_copied_hiker_individual_hook(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_BLUEPRINT;
    state.jokers[1].center_id = BALATRO_CENTER_J_HIKER;
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS};
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    /* Hiker has no context.blueprint guard: copied hook + real hook = +10. */
    assert(card.perma_bonus == 10);
}

static void test_source_mr_bones_uses_total_chips(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 1;
    state.discards_left = 0;
    state.blind_id = BALATRO_BLIND_BL_SMALL;
    state.blind_chips = 100;
    state.chips = 30;
    state.blind_reward = 4;
    state.dollars = 25;
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_MR_BONES;
    state.jokers[1].center_id = BALATRO_CENTER_J_GOLDEN;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS, .sort_id = 1};
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_ROUND_EVAL && state.joker_count == 1);
    assert(state.jokers[0].center_id == BALATRO_CENTER_J_GOLDEN);
    /* A Mr. Bones save pays no blind reward. Golden Joker and five interest
       brackets are still evaluated normally: $4 + $5, not $4 + $4 + $5. */
    assert(state.round_earnings == 9);
    BalatroAction cash_out = {.type = BALATRO_ACTION_CASH_OUT};
    assert(balatro_step(&state, &cash_out, &result) == BALATRO_OK);
    assert(state.dollars == 34);
}

static void test_source_temperance_jokers_only(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_JOKER;
    state.jokers[0].sell_cost = 5;
    state.consumable_count = 2;
    state.consumables[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_TEMPERANCE, .sell_cost = 1};
    state.consumables[1] = (BalatroCard){.center_id = BALATRO_CENTER_C_HERMIT, .sell_cost = 10};
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    BalatroStepResult result;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.dollars == 9 && state.consumable_count == 1);
}

static void test_source_ramen_is_removed_at_one(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 1;
    state.discards_left = 1;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_RAMEN;
    state.jokers[0].state[0] = 101;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS, .sort_id = 1};
    BalatroAction discard = {.type = BALATRO_ACTION_DISCARD, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &discard, &result) == BALATRO_OK);
    assert(state.joker_count == 0);
}

static void test_source_faceless_pareidolia_discard(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 3;
    state.hand_size = 3;
    state.discards_left = 1;
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_FACELESS;
    state.jokers[1].center_id = BALATRO_CENTER_J_PAREIDOLIA;
    for (uint8_t i = 0; i < state.hand_count; ++i)
        state.hand[i] = (BalatroCard){
            .center_id = BALATRO_CENTER_C_BASE, .rank = (uint8_t)(2 + i), .suit = BALATRO_CLUBS, .sort_id = (uint16_t)(i + 1)};
    BalatroAction discard = {.type = BALATRO_ACTION_DISCARD, .selection_count = 3, .selection = {0, 1, 2}};
    BalatroStepResult result;
    assert(balatro_step(&state, &discard, &result) == BALATRO_OK);
    assert(state.dollars == 9);
}

static void test_source_purple_seal_discard_tarot(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand_size = 1;
    state.discards_left = 1;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 9, .suit = BALATRO_CLUBS, .seal = 4, .sort_id = 1};
    BalatroAction discard = {.type = BALATRO_ACTION_DISCARD, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &discard, &result) == BALATRO_OK);
    assert(state.consumable_count == 1 && state.consumables[0].center_id != BALATRO_CENTER_C_BASE);
}

static void test_source_dusk_last_hand_timing(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand_size = 1;
    state.hands_left = 1;
    state.blind_chips = 100000;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_DUSK;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_CLUBS, .sort_id = 1};
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    BalatroStepResult result;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    /* High Card base 5 + rank-2 chips twice for Dusk's retrigger. */
    assert(state.last_hand_score == 9 && state.hands_left == 0);
}

static void test_source_obelisk_state(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    BalatroCard card = {.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_HEARTS};
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_OBELISK;
    state.hand_plays[BALATRO_PAIR] = 1;
    BalatroScoreResult score;
    assert(balatro_score_hand(&state, &card, 1, NULL, 0, &score) == BALATRO_OK);
    assert(state.jokers[0].state[0] == 120);
    assert(score.mult > 1.0);

    /* Obelisk reaches 1.6 through three successive +0.2 operations. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 5;
    state.jokers[0].center_id = BALATRO_CENTER_J_SMILEY;
    state.jokers[1].center_id = BALATRO_CENTER_J_ABSTRACT;
    state.jokers[2].center_id = BALATRO_CENTER_J_OBELISK;
    state.jokers[2].state[0] = 140;
    state.jokers[3].center_id = BALATRO_CENTER_J_POPCORN;
    state.jokers[3].state[0] = 12;
    state.jokers[4].center_id = BALATRO_CENTER_J_RED_CARD;
    state.hand_plays[BALATRO_PAIR] = 1;
    state.hand_plays[BALATRO_HIGH_CARD] = 2;
    BalatroCard rollout_hand[] = {
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 11, .suit = BALATRO_SPADES},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 10, .suit = BALATRO_HEARTS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 10, .suit = BALATRO_DIAMONDS},
        {.center_id = BALATRO_CENTER_C_BASE, .rank = 3, .suit = BALATRO_HEARTS},
    };
    assert(balatro_score_hand(&state, rollout_hand, 4, NULL, 0, &score) == BALATRO_OK);
    assert(state.jokers[2].state[0] == 160);
    assert(score.total == 1176);

    /* Without Popcorn, the same Obelisk progression scores 672. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 4;
    state.jokers[0].center_id = BALATRO_CENTER_J_SMILEY;
    state.jokers[1].center_id = BALATRO_CENTER_J_ABSTRACT;
    state.jokers[2].center_id = BALATRO_CENTER_J_OBELISK;
    state.jokers[2].state[0] = 140;
    state.jokers[3].center_id = BALATRO_CENTER_J_RED_CARD;
    state.hand_plays[BALATRO_PAIR] = 1;
    state.hand_plays[BALATRO_HIGH_CARD] = 2;
    assert(balatro_score_hand(&state, rollout_hand, 4, NULL, 0, &score) == BALATRO_OK);
    assert(score.total == 672);

    /* Joker ordering remains significant when Popcorn precedes Obelisk. */
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 5;
    state.jokers[0].center_id = BALATRO_CENTER_J_SMILEY;
    state.jokers[1].center_id = BALATRO_CENTER_J_ABSTRACT;
    state.jokers[2].center_id = BALATRO_CENTER_J_POPCORN;
    state.jokers[2].state[0] = 12;
    state.jokers[3].center_id = BALATRO_CENTER_J_OBELISK;
    state.jokers[3].state[0] = 140;
    state.jokers[4].center_id = BALATRO_CENTER_J_RED_CARD;
    state.hand_plays[BALATRO_PAIR] = 1;
    state.hand_plays[BALATRO_HIGH_CARD] = 2;
    assert(balatro_score_hand(&state, rollout_hand, 4, NULL, 0, &score) == BALATRO_OK);
    assert(score.total == 1392);
}

static void test_source_madness_setting_blind(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init(&state, &config, 1) == BALATRO_OK);
    state.joker_count = 2;
    state.jokers[0].center_id = BALATRO_CENTER_J_MADNESS;
    state.jokers[1].center_id = BALATRO_CENTER_J_JOKER;
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.joker_count == 1);
    assert(state.jokers[0].center_id == BALATRO_CENTER_J_MADNESS);
    assert(state.jokers[0].state[0] == 150);

    /* Random card selection sorts candidates by creation sort_id, not their
       current Joker-area order. This fixture selects Seltzer in sort order,
       which occupies Splash's position in display order. */
    assert(balatro_init_seed_string(&state, &config, "MC14") == BALATRO_OK);
    state.joker_count = 5;
    state.jokers[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_SELZER, .sort_id = 66};
    state.jokers[1] = (BalatroCard){.center_id = BALATRO_CENTER_J_SPLASH, .sort_id = 65};
    state.jokers[2] = (BalatroCard){.center_id = BALATRO_CENTER_J_FORTUNE_TELLER, .sort_id = 71};
    state.jokers[3] = (BalatroCard){.center_id = BALATRO_CENTER_J_POPCORN, .sort_id = 88};
    state.jokers[4] = (BalatroCard){.center_id = BALATRO_CENTER_J_MADNESS, .sort_id = 80};
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.joker_count == 4);
    assert(state.jokers[0].center_id == BALATRO_CENTER_J_SPLASH);
    assert(state.jokers[1].center_id == BALATRO_CENTER_J_FORTUNE_TELLER);
    assert(state.jokers[2].center_id == BALATRO_CENTER_J_POPCORN);
    assert(state.jokers[3].center_id == BALATRO_CENTER_J_MADNESS);
}

static void test_source_death_requires_two_targets(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "DEATH_TARGETS") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.hand_count = 2;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 2, .suit = BALATRO_HEARTS, .sort_id = 1};
    state.hand[1] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_SPADES, .sort_id = 2, .perma_bonus = 20};
    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_DEATH;
    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    assert(count > 0);
    int one_target = 0, two_targets = 0;
    for (int i = 0; i < count; ++i) {
        if (actions[i].type != BALATRO_ACTION_USE_CONSUMABLE || actions[i].primary != 0) continue;
        if (actions[i].selection_count == 1) one_target = 1;
        if (actions[i].selection_count == 2) two_targets = 1;
    }
    assert(!one_target && two_targets);
    BalatroStepResult result;
    BalatroAction invalid = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0, .selection_count = 1, .selection = {0}};
    assert(balatro_step(&state, &invalid, &result) == BALATRO_ERR_ACTION);
    BalatroAction valid = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0, .selection_count = 2, .selection = {0, 1}};
    assert(balatro_step(&state, &valid, &result) == BALATRO_OK);
    assert(state.consumable_count == 0 && state.hand[0].rank == 14);
    assert(state.hand[0].perma_bonus == 20);
}

static void test_source_targeted_tarot_during_blind(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "BLIND_TAROT") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SELECTING_HAND;
    state.hand_count = 1;
    state.hand[0] = (BalatroCard){.center_id = BALATRO_CENTER_C_BASE, .rank = 14, .suit = BALATRO_SPADES, .sort_id = 1};
    state.consumable_count = 1;
    state.consumables[0].center_id = BALATRO_CENTER_C_HEIROPHANT;

    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    assert(count > 0);
    BalatroAction use = {0};
    int found = 0;
    for (int i = 0; i < count; ++i) {
        if (actions[i].type == BALATRO_ACTION_USE_CONSUMABLE && actions[i].primary == 0 && actions[i].selection_count == 1 &&
            actions[i].selection[0] == 0) {
            use = actions[i];
            found = 1;
            break;
        }
    }
    assert(found);

    BalatroStepResult result;
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_SELECTING_HAND);
    assert(state.consumable_count == 0);
    assert(state.hand[0].enhancement == BALATRO_ENHANCEMENT_BONUS);
}

static void test_source_full_consumable_area_routes_use(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "FULL_CONSUMABLES") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.consumable_count = state.consumable_slots;
    state.consumables[0].center_id = BALATRO_CENTER_C_EMPEROR;
    state.consumables[1].center_id = BALATRO_CENTER_C_HERMIT;
    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    int emperor_legal = 0;
    for (int i = 0; i < count; ++i)
        if (actions[i].type == BALATRO_ACTION_USE_CONSUMABLE && actions[i].primary == 0) emperor_legal = 1;
    assert(emperor_legal);
    BalatroStepResult result;
    BalatroAction use = {.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.consumable_count == state.consumable_slots);

    assert(balatro_init_seed_string(&state, &config, "FULL_FOOL") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.consumable_count = state.consumable_slots;
    state.consumables[0].center_id = BALATRO_CENTER_C_FOOL;
    state.consumables[1].center_id = BALATRO_CENTER_C_HERMIT;
    state.last_tarot_planet = BALATRO_CENTER_C_MERCURY;
    count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    int fool_legal = 0;
    for (int i = 0; i < count; ++i)
        if (actions[i].type == BALATRO_ACTION_USE_CONSUMABLE && actions[i].primary == 0) fool_legal = 1;
    assert(fool_legal);
    use = (BalatroAction){.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = 0};
    assert(balatro_step(&state, &use, &result) == BALATRO_OK);
    assert(state.consumable_count == state.consumable_slots);
}

static void test_source_round_and_persistent_shop_voucher(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "ROUND_VOUCHER") == BALATRO_OK);
    assert(state.round == 0 && state.next_voucher_id != 0);

    BalatroStepResult result;
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    assert(state.round == 1);

    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 100;
    uint16_t voucher = state.next_voucher_id;
    balatro_populate_shop(&state);
    assert(state.next_voucher_id == voucher);

    BalatroAction next = {.type = BALATRO_ACTION_NEXT_ROUND};
    assert(balatro_step(&state, &next, &result) == BALATRO_OK);
    assert(state.shop_count == 0 && state.round == 1);

    state.phase = BALATRO_PHASE_SHOP;
    balatro_populate_shop(&state);
    uint8_t voucher_index = UINT8_MAX;
    for (uint8_t i = 0; i < state.shop_count; ++i)
        if (state.shop_cards[i].center_id == voucher) voucher_index = i;
    assert(voucher_index != UINT8_MAX);
    BalatroAction redeem = {.type = BALATRO_ACTION_REDEEM_VOUCHER, .primary = voucher_index};
    assert(balatro_step(&state, &redeem, &result) == BALATRO_OK);
    assert(state.next_voucher_id == 0);
    balatro_populate_shop(&state);
    for (uint8_t i = 0; i < state.shop_count; ++i)
        assert(state.shop_cards[i].center_id < BALATRO_CENTER_V_ANTIMATTER || state.shop_cards[i].center_id > BALATRO_CENTER_V_WASTEFUL);
}

static void test_source_magic_trick_shop_playing_cards(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "SHOP_PLAYING") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.ante = 3;
    state.shop_count = 0;
    state.next_voucher_id = 0;
    state.shop_joker_max = 2;
    state.joker_rate = state.tarot_rate = state.planet_rate = state.spectral_rate = 0;
    state.playing_card_rate = 4;
    state.joker_count = 1;
    state.jokers[0].center_id = BALATRO_CENTER_J_HOLOGRAM;
    state.jokers[0].state[0] = 100;

    balatro_populate_shop(&state);
    assert(state.shop_count == 4);
    /* pseudorandom_element sorts G.P_CARDS by key.  frontsho3 therefore
       produces D_4 and S_7 for this seed, not table-literal order cards. */
    assert(state.shop_cards[0].center_id == BALATRO_CENTER_C_BASE);
    assert(state.shop_cards[0].suit == BALATRO_DIAMONDS && state.shop_cards[0].rank == 4);
    assert(state.shop_cards[1].center_id == BALATRO_CENTER_C_BASE);
    assert(state.shop_cards[1].suit == BALATRO_SPADES && state.shop_cards[1].rank == 7);

    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    BalatroAction buy = {0};
    int found = 0;
    for (int i = 0; i < count; ++i)
        if (actions[i].type == BALATRO_ACTION_BUY_CARD && actions[i].primary == 0) {
            buy = actions[i];
            found = 1;
            break;
        }
    assert(found);
    uint16_t deck_count = state.deck_count;
    BalatroStepResult result;
    assert(balatro_step(&state, &buy, &result) == BALATRO_OK);
    assert(state.deck_count == deck_count + 1);
    assert(state.deck[0].suit == BALATRO_DIAMONDS && state.deck[0].rank == 4);
    assert(state.jokers[0].state[0] == 125);
}

static void test_source_credit_card_shop_affordability(void) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "CREDIT_SHOP") == BALATRO_OK);
    state.phase = BALATRO_PHASE_SHOP;
    state.dollars = 0;
    state.joker_count = 3;
    state.jokers[0].center_id = BALATRO_CENTER_J_CREDIT_CARD;
    state.jokers[1].center_id = BALATRO_CENTER_J_CREDIT_CARD;
    state.jokers[2].center_id = BALATRO_CENTER_J_CREDIT_CARD;
    state.jokers[2].flags = BALATRO_CARD_DEBUFFED;
    state.reroll_cost = 30;
    state.shop_count = 3;
    state.shop_cards[0] = (BalatroCard){.center_id = BALATRO_CENTER_J_JOKER, .cost = 30};
    state.shop_cards[1] = (BalatroCard){.center_id = BALATRO_CENTER_V_OVERSTOCK_NORM, .cost = 30};
    state.shop_cards[2] = (BalatroCard){.center_id = BALATRO_CENTER_P_CELESTIAL_NORMAL_1, .cost = 30};

    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    int count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    assert(count > 0);
    int saw_buy = 0, saw_redeem = 0, saw_open = 0, saw_reroll = 0;
    for (int i = 0; i < count; ++i) {
        saw_buy |= actions[i].type == BALATRO_ACTION_BUY_CARD;
        saw_redeem |= actions[i].type == BALATRO_ACTION_REDEEM_VOUCHER;
        saw_open |= actions[i].type == BALATRO_ACTION_OPEN_BOOSTER;
        saw_reroll |= actions[i].type == BALATRO_ACTION_REROLL;
        BalatroState copy = state;
        BalatroStepResult result;
        assert(balatro_step(&copy, &actions[i], &result) == BALATRO_OK);
    }
    assert(saw_buy && saw_redeem && saw_open && saw_reroll);

    state.dollars = -15;
    state.joker_count = 1;
    state.reroll_cost = 6;
    for (uint8_t i = 0; i < state.shop_count; ++i) state.shop_cards[i].cost = 6;
    count = legal_actions(&state, actions, BALATRO_MAX_LEGAL_ACTIONS);
    assert(count > 0);
    for (int i = 0; i < count; ++i)
        assert(actions[i].type != BALATRO_ACTION_BUY_CARD && actions[i].type != BALATRO_ACTION_REDEEM_VOUCHER &&
               actions[i].type != BALATRO_ACTION_OPEN_BOOSTER && actions[i].type != BALATRO_ACTION_REROLL);
    assert(balatro_buy_shop_card(&state, 0) == BALATRO_ERR_ACTION);
    assert(balatro_redeem_voucher(&state, 1) == BALATRO_ERR_ACTION);
    assert(balatro_open_booster(&state, 2) == BALATRO_ERR_ACTION);
    assert(balatro_reroll_shop(&state) == BALATRO_ERR_ACTION);
}

static void test_source_large_and_nonfinite_scores(void) {
    /* get_blind_amount remains a Lua-number calculation beyond integer
       ranges. Endless scaling passes INT64_MAX long before binary64 itself
       overflows, after which the source modulo expression produces NaN. */
    assert(balatro_blind_amount(20, 1) > (double)INT64_MAX);
    assert(isnan(balatro_blind_amount(39, 1)));
    assert(isnan(balatro_blind_target(39, 2, 1)));

    BalatroConfig config;
    balatro_default_config(&config);
    BalatroState state;
    assert(balatro_init_seed_string(&state, &config, "LARGE_SCORE") == BALATRO_OK);
    state.chips = 1e100;
    state.blind_chips = INFINITY;
    state.last_hand_score = -INFINITY;

    BalatroObservation observation;
    assert(balatro_observe(&state, &observation) == BALATRO_OK);
    assert(observation.scalars.chips_number == BALATRO_NUMBER_FINITE);
    assert(observation.scalars.blind_chips_number == BALATRO_NUMBER_POSITIVE_INFINITY);
    assert(observation.scalars.last_hand_score_number == BALATRO_NUMBER_NEGATIVE_INFINITY);
    assert(isfinite(observation.scalars.chips));
    assert(isfinite(observation.scalars.blind_chips));
    assert(isfinite(observation.scalars.last_hand_score));

    state.chips = nan("0x42");
    state.blind_chips = 1.0;
    state.last_hand_score = nan("0x99");
    assert(balatro_observe(&state, &observation) == BALATRO_OK);
    assert(observation.scalars.chips_number == BALATRO_NUMBER_NAN);
    assert(observation.scalars.last_hand_score_number == BALATRO_NUMBER_NAN);
    assert(observation.scalars.chips_over_blind_number == BALATRO_NUMBER_NAN);
    assert(observation.scalars.chips == 0.0f);
    assert(observation.scalars.last_hand_score == 0.0f);
    assert(observation.scalars.chips_over_blind == 0.0f);

    size_t snapshot_size = balatro_serialize(&state, NULL, 0);
    assert(snapshot_size > sizeof(state));
    void *snapshot = malloc(snapshot_size);
    assert(snapshot);
    assert(balatro_serialize(&state, snapshot, snapshot_size) == snapshot_size);
    BalatroState restored;
    assert(balatro_deserialize(&restored, snapshot, snapshot_size) == BALATRO_OK);
    free(snapshot);
    assert(isnan(restored.chips) && restored.blind_chips == 1.0 && isnan(restored.last_hand_score));

    /* IEEE comparisons reproduce the source transition behavior: +infinity
       clears a finite blind, while NaN never satisfies the clear check. */
    assert(balatro_init_seed_string(&state, &config, "INFINITE_CLEAR") == BALATRO_OK);
    BalatroStepResult result;
    BalatroAction select = {.type = BALATRO_ACTION_SELECT_BLIND};
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    state.chips = INFINITY;
    BalatroAction play = {.type = BALATRO_ACTION_PLAY_HAND, .selection_count = 1, .selection = {0}};
    double predicted_score;
    assert(balatro_score_play_actions_trusted(&state, &play, 1, &predicted_score) == BALATRO_OK);
    assert(isfinite(predicted_score) && predicted_score > 0.0);
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.last_hand_score == predicted_score);
    assert(state.phase == BALATRO_PHASE_ROUND_EVAL && isinf(state.chips));

    assert(balatro_init_seed_string(&state, &config, "INFINITE_BLIND") == BALATRO_OK);
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    state.chips = INFINITY;
    state.blind_chips = INFINITY;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_SELECTING_HAND && isinf(state.chips));

    assert(balatro_init_seed_string(&state, &config, "NAN_NO_CLEAR") == BALATRO_OK);
    assert(balatro_step(&state, &select, &result) == BALATRO_OK);
    state.chips = NAN;
    assert(balatro_step(&state, &play, &result) == BALATRO_OK);
    assert(state.phase == BALATRO_PHASE_SELECTING_HAND && isnan(state.chips));
    assert(isfinite(result.reward));
}

int main(void) {
    assert(fabs(balatro_pseudohash("TESTSEED") - 0.3192720782223546) < 1e-12);
    test_poker();
    test_rng();
    test_exact_rng_fast_paths();
    test_blind_scaling_and_economy();
    test_shop_lifecycle();
    test_source_boss_reroll_action();
    test_spectral_lifecycle();
    test_juggle_tag_next_round();
    test_source_orbital_choices_are_preselected();
    test_handy_tag_uses_run_hand_total();
    test_source_top_up_tag_uses_joker_creation_pipeline();
    test_source_immolate_preserves_survivor_visual_order();
    test_source_end_of_round_seals();
    test_source_dollar_jokers();
    test_source_perishable_lifecycle();
    test_stake_round_modifiers();
    test_source_starting_vouchers();
    test_anaglyph_boss_tag();
    test_telescope_planet_pack();
    test_source_tag_shop_hooks();
    test_source_joker_lifecycle_hooks();
    test_source_verdant_leaf_sale_disables_blind();
    test_source_free_pack_tags();
    test_source_midas_vampire_mutation();
    test_source_glass_joker_destruction_hooks();
    test_source_glass_hanged_man_hooks();
    test_source_matador_debuff_bonus();
    test_source_hologram_playing_card_added();
    test_source_gold_seal_only_pays_when_played();
    test_source_seltzer_expires_after_hand();
    test_source_mime_repeats_round_eval_cards();
    test_source_ice_cream_melts();
    test_source_to_do_list_rollover();
    test_source_flint_modifies_base_hand_before_cards();
    test_source_stone_tally_avoids_played_duplicate();
    test_source_idol_target_reset();
    test_source_mail_castle_target_reset();
    test_source_loyalty_card_cycle();
    test_source_hand_counter_timing();
    test_source_brainstorm_first_slot();
    test_source_flower_pot_wildcard_order();
    test_source_debuffed_discard_hooks();
    test_source_matador_ordinary_debuff();
    test_source_erosion_abandoned_deck_size();
    test_source_plasma_final_scoring();
    test_source_retrigger_stateful_jokers();
    test_source_retrigger_interaction_matrix();
    test_source_retrigger_card_effect_order();
    test_source_retrigger_eligibility_boundaries();
    test_source_held_retrigger_interactions();
    test_source_end_round_copied_mime();
    test_source_retrigger_lifecycle_persistence();
    test_source_joker_activation_scoring_gaps();
    test_source_copied_held_joker_hook();
    test_source_raised_fist_selects_before_debuff();
    test_source_copied_joker_self_exclusion();
    test_source_copied_before_state_suppression();
    test_source_joker_on_joker_order();
    test_source_joker_edition_order();
    test_source_dna_before_scoring();
    test_source_eight_ball_retriggers();
    test_source_pack_consumable_use();
    test_source_emperor_reuses_named_stream();
    test_source_pack_generators_respect_consumable_capacity();
    test_source_hallucination_open_booster_context();
    test_source_blind_random_card_ordering();
    test_source_manacle_restores_before_shop_pack();
    test_source_all_boss_temporary_state_clears_on_defeat();
    test_source_chicot_disables_setup_penalties();
    test_source_sell_consumable_during_blind();
    test_source_blind_select_inventory_actions();
    test_source_hand_sort_mode_persists_across_draw();
    test_source_pack_hand_returns_to_deck_bottom();
    test_source_copied_hiker_individual_hook();
    test_source_mr_bones_uses_total_chips();
    test_source_temperance_jokers_only();
    test_source_ramen_is_removed_at_one();
    test_source_faceless_pareidolia_discard();
    test_source_purple_seal_discard_tarot();
    test_source_dusk_last_hand_timing();
    test_source_obelisk_state();
    test_source_madness_setting_blind();
    test_source_death_requires_two_targets();
    test_source_targeted_tarot_during_blind();
    test_source_full_consumable_area_routes_use();
    test_source_round_and_persistent_shop_voucher();
    test_source_magic_trick_shop_playing_cards();
    test_source_credit_card_shop_affordability();
    test_source_large_and_nonfinite_scores();
    test_full_joker_slots_accept_negative_shop_joker();
    test_state();
    test_legal_view();
    test_clone_hash_and_snapshots();
    test_validation_card_injection();
    test_source_wraith_factory_streams();
    test_source_riff_raff_factory_resample();
    test_source_soul_legendary_stream();
    test_source_soul_pack_type_gates();
    test_source_pack_overlay_joker_sale_and_cryptid();
    test_source_buy_and_use_bypasses_consumable_capacity();
    test_source_suit_boss_stone_and_wild_debuffs();
    test_source_grim_rolls_suit_without_fixed_rank_poll();
    test_source_hook_named_stream_targets();
    test_source_aura_target_and_edition();
    test_shaped_reward_progress();
    test_observation_layout();
    test_observation_policy_actions();
    puts("balatro core tests passed");
    return 0;
}
