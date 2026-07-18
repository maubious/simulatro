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
#endif
