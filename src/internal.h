#ifndef BALATRO_INTERNAL_H
#define BALATRO_INTERNAL_H

#include "balatro.h"

enum BalatroCenterSet {
    SET_DEFAULT = 1,
    SET_PLAYING = 1,
    SET_ENHANCED = 2,
    SET_JOKER = 3,
    SET_TAROT = 4,
    SET_PLANET = 5,
    SET_SPECTRAL = 6,
    SET_VOUCHER = 7,
    SET_BOOSTER = 8
};

static inline uint16_t balatro_playing_card_count(const BalatroState *state) {
    return (uint16_t)(state->deck_count + state->hand_count + state->discard_count);
}

static inline int balatro_can_add_playing_cards(const BalatroState *state, uint16_t count) {
    return (uint32_t)balatro_playing_card_count(state) + count <= BALATRO_OBS_MAX_PLAYING_CARDS;
}

typedef struct BalatroKeyBuilder {
    char *data;
    size_t capacity;
    size_t length;
} BalatroKeyBuilder;

void balatro_key_begin(BalatroKeyBuilder *builder, char *data, size_t capacity);
void balatro_key_append(BalatroKeyBuilder *builder, const char *text);
void balatro_key_append_u64(BalatroKeyBuilder *builder, uint64_t value);
void balatro_key_with_u64(char *data, size_t capacity, const char *prefix, uint64_t value);

void balatro_rng_reset(BalatroState *state);
double balatro_round_decimal13(double value);
void balatro_shuffle(BalatroState *state, BalatroCard *cards, size_t count, const char *stream);
void balatro_draw_to_hand(BalatroState *state);
void balatro_populate_shop(BalatroState *state);
int balatro_can_afford(const BalatroState *state, int32_t cost);
uint8_t balatro_card_set(const BalatroCard *card);
uint16_t balatro_planet_center(uint8_t hand);
BalatroHandType balatro_planet_hand(uint16_t center_id);
BalatroCard balatro_create_pooled_card(BalatroState *state, uint8_t set, const char *append, int pack_area);
void balatro_initialize_joker_card(BalatroState *state, BalatroCard *card);
void balatro_price_card(const BalatroState *state, BalatroCard *card);
BalatroHandType balatro_classify_hand_rules(const BalatroCard *cards, size_t count, uint8_t *scoring_mask, int four_fingers, int shortcut,
                                            int smeared);
int balatro_center_used(const BalatroState *state, uint16_t id);
void balatro_mark_center_used(BalatroState *state, uint16_t id);
void balatro_unmark_center_used(BalatroState *state, uint16_t id);
uint16_t balatro_pick_voucher(BalatroState *state);
uint16_t balatro_pick_voucher_from_tag(BalatroState *state);
int balatro_open_free_pack(BalatroState *state, uint16_t center_id);
void balatro_complete_pack_pick(BalatroState *state, uint8_t index);
uint8_t balatro_most_played_hand(const BalatroState *state);
void balatro_joker_added(BalatroState *state, const BalatroCard *joker);
void balatro_joker_removed(BalatroState *state, const BalatroCard *joker);
void balatro_consumable_added(BalatroState *state, const BalatroCard *card);
void balatro_consumable_removed(BalatroState *state, const BalatroCard *card);
void balatro_playing_card_added(BalatroState *state, uint8_t count);
void balatro_clear_card_debuffs(BalatroState *state);
void balatro_refresh_joker_cache(BalatroState *state);
int balatro_joker_active(const BalatroState *state, uint16_t center_id);
int balatro_action_is_legal(const BalatroState *state, const BalatroAction *action);
int balatro_state_layout_valid(const BalatroState *state);
int add_pooled_consumable(BalatroState *state, uint8_t set, const char *append, uint8_t edition);
int add_specific_consumable(BalatroState *state, uint16_t center_id);
int add_joker_rarity(BalatroState *state, uint8_t rarity, const char *append, int legendary);
void remove_joker_at(BalatroState *state, uint8_t index);
void remove_hand_index(BalatroState *state, uint8_t index);
uint8_t random_sorted_hand_index(BalatroState *state, const char *stream);
void add_spectral_cards(BalatroState *state, const uint8_t *ranks, size_t rank_count, uint8_t count, const char *stream);
double balatro_probability_normal(const BalatroState *state, double base);
void sort_hand_desc(BalatroState *state);
void refresh_card_debuff(const BalatroState *state, BalatroCard *card);
void apply_consumable(BalatroState *state, const BalatroAction *action, BalatroCard card);
void use_consumable(BalatroState *state, const BalatroAction *action);
void sell_consumable(BalatroState *state, uint8_t index);
void reset_round_rerolls(BalatroState *state);
void choose_orbital_hands(BalatroState *state);
void assign_blind_tags(BalatroState *state);
void apply_skip_tag(BalatroState *state, uint8_t tag);
void reset_round_targets(BalatroState *state);
void draw_after_play(BalatroState *state);
void apply_discard_effects(BalatroState *state, const BalatroCard *cards, uint8_t count, int first_discard, int hook);
void apply_hook_discard(BalatroState *state);
int find_discard_card(const BalatroState *state, uint16_t sort_id);
void apply_drawn_to_hand_boss(BalatroState *state, int crimson_prepped);
void sort_hand_mode(BalatroState *state, int by_suit);
void apply_card_debuffs(BalatroState *state);
void swap_jokers(BalatroState *state, uint8_t left, uint8_t right);
int8_t blind_reward_for(uint16_t blind_id, uint8_t blind_on_deck);
uint16_t choose_boss(BalatroState *state);
uint8_t choose_to_do_hand(BalatroState *state, int excluded);
void notify_removed_playing_cards(BalatroState *state, const BalatroCard *cards, uint8_t count, int shattered);
BalatroCard random_playing_card(BalatroState *state, const char *stream);
int blind_debuffs_card(const BalatroState *state, const BalatroCard *card);
#endif
