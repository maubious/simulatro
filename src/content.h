/* Generated content metadata. Do not hand-edit. */
#ifndef BALATRO_CONTENT_H
#define BALATRO_CONTENT_H

#include <stddef.h>
#include <stdint.h>

typedef enum BalatroTargetEffect { TARGET_CUSTOM, TARGET_ENHANCEMENT, TARGET_SUIT, TARGET_RANK_UP, TARGET_SEAL } BalatroTargetEffect;
typedef enum BalatroVoucherEffect {
    VOUCHER_NONE,
    VOUCHER_SHOP_SIZE,
    VOUCHER_TAROT_RATE,
    VOUCHER_PLANET_RATE,
    VOUCHER_EDITION_RATE,
    VOUCHER_PLAYING_RATE,
    VOUCHER_CONSUMABLE_SLOT,
    VOUCHER_DISCOUNT,
    VOUCHER_REROLL,
    VOUCHER_INTEREST,
    VOUCHER_HANDS,
    VOUCHER_HAND_SIZE,
    VOUCHER_DISCARDS,
    VOUCHER_JOKER_SLOT,
    VOUCHER_ANTE_HANDS,
    VOUCHER_ANTE_DISCARDS
} BalatroVoucherEffect;
typedef struct BalatroCenterDefinition {
    uint16_t id;
    uint8_t set;
    uint8_t rarity;
    int16_t cost;
    float weight;
    uint8_t base_available;
    uint8_t kind;
    uint8_t pack_extra;
    uint8_t pack_choose;
    uint16_t
        requires;
    float extra;
    uint8_t target_effect;
    uint8_t target_value;
    uint8_t target_max;
    uint8_t target_min;
    uint8_t voucher_effect;
} BalatroCenterDefinition;

typedef struct BalatroPlayingCardDefinition {
    uint8_t suit;
    uint8_t rank;
} BalatroPlayingCardDefinition;
extern const BalatroCenterDefinition balatro_centers[BALATRO_CENTER_COUNT];
BalatroPlayingCardDefinition balatro_playing_card(uint8_t index);
const uint16_t *balatro_center_pool(uint8_t set, size_t *count);
const uint16_t *balatro_joker_pool(uint8_t rarity, size_t *count);
#endif
