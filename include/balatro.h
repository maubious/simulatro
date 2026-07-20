#ifndef BALATRO_H
#define BALATRO_H

#include <stddef.h>
#include <stdint.h>
#include "balatro_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed storage for the standard Ante-8 public-observation profile.  A run
   may select any smaller capacity in BalatroConfig.  These are training
   environment limits, not semantic maxima of endless Balatro. */
#define BALATRO_OBS_MAX_PLAYING_CARDS 256
#define BALATRO_OBS_MAX_PLAYING_VARIANTS 256
#define BALATRO_OBS_MAX_HAND 64
#define BALATRO_OBS_MAX_JOKERS 32
#define BALATRO_OBS_MAX_CONSUMABLES 32
#define BALATRO_OBS_MAX_TAGS 64
#define BALATRO_OBS_MAX_SHOP_MAIN 4
#define BALATRO_OBS_MAX_SHOP_VOUCHERS 64
#define BALATRO_OBS_MAX_SHOP_BOOSTERS 2
#define BALATRO_OBS_MAX_PACK_CARDS 5

/* Live state uses the same capacity contract as the public ABI. Draw and
   discard each need room for the full playing-card set, while additions are
   bounded by BALATRO_OBS_MAX_PLAYING_CARDS globally. */
#define BALATRO_MAX_DECK BALATRO_OBS_MAX_PLAYING_CARDS
#define BALATRO_MAX_HAND BALATRO_OBS_MAX_HAND
#define BALATRO_MAX_JOKERS BALATRO_OBS_MAX_JOKERS
#define BALATRO_MAX_CONSUMABLES BALATRO_OBS_MAX_CONSUMABLES
#define BALATRO_MAX_SHOP_CARDS \
    (BALATRO_OBS_MAX_SHOP_MAIN + BALATRO_OBS_MAX_SHOP_VOUCHERS + BALATRO_OBS_MAX_SHOP_BOOSTERS)
#define BALATRO_MAX_PACK_CARDS BALATRO_OBS_MAX_PACK_CARDS
#define BALATRO_MAX_SELECTION 5
#define BALATRO_MAX_LEGAL_ACTIONS 20000
#define BALATRO_MAX_LEGAL_GROUPS 512
#define BALATRO_MAX_RNG_STREAMS 256
#define BALATRO_ACTION_TYPE_COUNT 23

typedef enum BalatroError {
    BALATRO_OK = 0,
    BALATRO_ERR_ARGUMENT = -1,
    BALATRO_ERR_ACTION = -2,
    BALATRO_ERR_CAPACITY = -3,
    BALATRO_ERR_SNAPSHOT = -4,
    BALATRO_ERR_INVARIANT = -6,
    BALATRO_ERR_OBSERVATION_CAPACITY = -7
} BalatroError;

typedef enum BalatroTruncationReason {
    BALATRO_TRUNCATED_NONE = 0,
    BALATRO_TRUNCATED_OBSERVATION_CAPACITY = 1
} BalatroTruncationReason;

typedef enum BalatroNumberClass {
    BALATRO_NUMBER_FINITE = 0,
    BALATRO_NUMBER_POSITIVE_INFINITY = 1,
    BALATRO_NUMBER_NEGATIVE_INFINITY = 2,
    BALATRO_NUMBER_NAN = 3
} BalatroNumberClass;

typedef enum BalatroObservationSection {
    BALATRO_OBS_SECTION_NONE = 0,
    BALATRO_OBS_SECTION_PLAYING_CARDS = 1,
    BALATRO_OBS_SECTION_PLAYING_VARIANTS = 2,
    BALATRO_OBS_SECTION_HAND = 3,
    BALATRO_OBS_SECTION_JOKERS = 4,
    BALATRO_OBS_SECTION_CONSUMABLES = 5,
    BALATRO_OBS_SECTION_TAGS = 6,
    BALATRO_OBS_SECTION_SHOP_MAIN = 7,
    BALATRO_OBS_SECTION_SHOP_VOUCHERS = 8,
    BALATRO_OBS_SECTION_SHOP_BOOSTERS = 9,
    BALATRO_OBS_SECTION_PACK_CARDS = 10
} BalatroObservationSection;

typedef enum BalatroPhase {
    BALATRO_PHASE_BLIND_SELECT = 0,
    BALATRO_PHASE_SELECTING_HAND = 1,
    BALATRO_PHASE_ROUND_EVAL = 2,
    BALATRO_PHASE_SHOP = 3,
    BALATRO_PHASE_PACK_OPENING = 4,
    BALATRO_PHASE_GAME_OVER = 5
} BalatroPhase;

typedef enum BalatroActionType {
    BALATRO_ACTION_PLAY_HAND = 0,
    BALATRO_ACTION_DISCARD = 1,
    BALATRO_ACTION_SELECT_BLIND = 2,
    BALATRO_ACTION_SKIP_BLIND = 3,
    BALATRO_ACTION_CASH_OUT = 4,
    BALATRO_ACTION_REROLL = 5,
    BALATRO_ACTION_NEXT_ROUND = 6,
    BALATRO_ACTION_SKIP_PACK = 7,
    BALATRO_ACTION_BUY_CARD = 8,
    BALATRO_ACTION_SELL_JOKER = 9,
    BALATRO_ACTION_SELL_CONSUMABLE = 10,
    BALATRO_ACTION_USE_CONSUMABLE = 11,
    BALATRO_ACTION_REDEEM_VOUCHER = 12,
    BALATRO_ACTION_OPEN_BOOSTER = 13,
    BALATRO_ACTION_PICK_PACK_CARD = 14,
    BALATRO_ACTION_SWAP_JOKERS_LEFT = 15,
    BALATRO_ACTION_SWAP_JOKERS_RIGHT = 16,
    BALATRO_ACTION_SWAP_HAND_LEFT = 17,
    BALATRO_ACTION_SWAP_HAND_RIGHT = 18,
    BALATRO_ACTION_SORT_HAND_RANK = 19,
    BALATRO_ACTION_SORT_HAND_SUIT = 20,
    BALATRO_ACTION_BUY_AND_USE = 21,
    BALATRO_ACTION_REROLL_BOSS = 22
} BalatroActionType;

typedef enum BalatroSuit { BALATRO_HEARTS = 0, BALATRO_DIAMONDS = 1, BALATRO_CLUBS = 2, BALATRO_SPADES = 3 } BalatroSuit;

typedef enum BalatroEnhancement {
    BALATRO_ENHANCEMENT_NONE = 0,
    BALATRO_ENHANCEMENT_BONUS = 1,
    BALATRO_ENHANCEMENT_MULT = 2,
    BALATRO_ENHANCEMENT_WILD = 3,
    BALATRO_ENHANCEMENT_GLASS = 4,
    BALATRO_ENHANCEMENT_STEEL = 5,
    BALATRO_ENHANCEMENT_STONE = 6,
    BALATRO_ENHANCEMENT_GOLD = 7,
    BALATRO_ENHANCEMENT_LUCKY = 8
} BalatroEnhancement;

typedef enum BalatroEdition {
    BALATRO_EDITION_NONE = 0,
    BALATRO_EDITION_FOIL = 1,
    BALATRO_EDITION_HOLO = 2,
    BALATRO_EDITION_POLYCHROME = 3,
    BALATRO_EDITION_NEGATIVE = 4
} BalatroEdition;

typedef enum BalatroSeal {
    BALATRO_SEAL_NONE = 0,
    BALATRO_SEAL_GOLD = 1,
    BALATRO_SEAL_RED = 2,
    BALATRO_SEAL_BLUE = 3,
    BALATRO_SEAL_PURPLE = 4
} BalatroSeal;

typedef enum BalatroCardFlag {
    BALATRO_CARD_DEBUFFED = 1u << 0,
    BALATRO_CARD_ETERNAL = 1u << 1,
    BALATRO_CARD_PERISHABLE = 1u << 2,
    BALATRO_CARD_RENTAL = 1u << 3,
    BALATRO_CARD_FORCED = 1u << 4
} BalatroCardFlag;

/* Ask the validation card factory to perform the normal edition poll. */
#define BALATRO_EDITION_POLL UINT8_MAX

typedef enum BalatroHandType {
    BALATRO_FLUSH_FIVE = 0,
    BALATRO_FLUSH_HOUSE,
    BALATRO_FIVE_OF_A_KIND,
    BALATRO_STRAIGHT_FLUSH,
    BALATRO_FOUR_OF_A_KIND,
    BALATRO_FULL_HOUSE,
    BALATRO_FLUSH,
    BALATRO_STRAIGHT,
    BALATRO_THREE_OF_A_KIND,
    BALATRO_TWO_PAIR,
    BALATRO_PAIR,
    BALATRO_HIGH_CARD,
    BALATRO_HAND_COUNT
} BalatroHandType;

typedef struct BalatroCard {
    uint16_t center_id;
    uint16_t sort_id;
    uint8_t suit;
    uint8_t rank;
    uint8_t enhancement;
    uint8_t edition;
    uint8_t seal;
    uint8_t flags;
    int16_t perma_bonus;
    int16_t cost;
    int16_t sell_cost;
    int32_t state[4];
} BalatroCard;

typedef struct BalatroAction {
    uint8_t type;
    uint8_t primary;
    uint8_t selection_count;
    uint8_t selection[BALATRO_MAX_SELECTION];
} BalatroAction;

typedef enum BalatroLegalGroupKind { BALATRO_LEGAL_DISCRETE = 0, BALATRO_LEGAL_SELECTION = 1 } BalatroLegalGroupKind;

typedef struct BalatroSelectionFamily {
    uint8_t type;
    uint8_t primary;
    uint8_t minimum;
    uint8_t maximum;
    uint64_t allowed_mask;
    uint64_t required_mask;
} BalatroSelectionFamily;

typedef struct BalatroLegalGroup {
    uint8_t kind;
    uint8_t reserved[3];
    BalatroAction action;
    BalatroSelectionFamily selection;
} BalatroLegalGroup;

typedef struct BalatroLegalView {
    uint16_t group_count;
    uint16_t reserved;
    uint32_t action_count;
    BalatroLegalGroup groups[BALATRO_MAX_LEGAL_GROUPS];
} BalatroLegalView;

typedef struct BalatroObservationProfile {
    uint16_t playing_cards;
    uint16_t playing_variants;
    uint16_t hand;
    uint16_t jokers;
    uint16_t consumables;
    uint16_t tags;
    uint16_t shop_vouchers;
} BalatroObservationProfile;

typedef struct BalatroConfig {
    uint8_t deck;
    uint8_t stake;
    uint8_t win_ante;
    uint8_t shaped_reward;
    uint8_t validation;
    uint8_t reserved;
    BalatroObservationProfile observation;
} BalatroConfig;

typedef struct BalatroRngStream {
    uint64_t key_hash;
    double value;
} BalatroRngStream;

typedef struct BalatroState {
    BalatroConfig config;
    uint64_t numeric_seed;
    char seed[32];
    double hashed_seed;
    BalatroRngStream rng[BALATRO_MAX_RNG_STREAMS];
    uint16_t rng_count;
    uint16_t next_sort_id;

    uint8_t phase;
    uint8_t blind_on_deck;
    uint8_t ante;
    uint8_t round;
    uint8_t won;
    uint8_t terminal;
    uint8_t hands_left;
    uint8_t discards_left;
    uint8_t hands_played;
    uint8_t discards_used;
    uint8_t hand_size;
    uint8_t joker_slots;
    uint8_t consumable_slots;
    uint8_t skips;
    uint8_t blind_skipped_mask;
    /* G.hand.config.sort: 0 = rank, 1 = suit. The choice persists when new
       cards are dealt rather than applying only to the current hand. */
    uint8_t hand_sort_suit;

    int32_t dollars;
    /* Balatro stores score-domain values as Lua numbers (binary64), including
       values beyond exact-integer range and the NaN/infinities reached by
       extreme endless runs. */
    double chips;
    double blind_chips;
    double last_hand_score;
    int32_t reroll_cost;
    int32_t round_earnings;
    int16_t interest_cap;
    int8_t interest_amount;
    int8_t rental_rate;
    int8_t blind_reward;
    uint8_t stake_scaling;
    uint32_t actions_taken;
    /* G.GAME.hands_played: persistent across rounds, unlike hands_played
       above which mirrors G.GAME.current_round.hands_played. */
    uint32_t run_hands_played;

    BalatroCard deck[BALATRO_MAX_DECK];
    BalatroCard hand[BALATRO_MAX_HAND];
    BalatroCard discard[BALATRO_MAX_DECK];
    BalatroCard jokers[BALATRO_MAX_JOKERS];
    BalatroCard consumables[BALATRO_MAX_CONSUMABLES];
    BalatroCard shop_main[BALATRO_OBS_MAX_SHOP_MAIN];
    BalatroCard shop_vouchers[BALATRO_OBS_MAX_SHOP_VOUCHERS];
    BalatroCard shop_boosters[BALATRO_OBS_MAX_SHOP_BOOSTERS];
    BalatroCard pack_cards[BALATRO_MAX_PACK_CARDS];
    uint16_t deck_count;
    uint8_t hand_count;
    uint16_t discard_count;
    uint8_t joker_count;
    uint8_t consumable_count;
    uint8_t shop_main_count;
    uint8_t shop_voucher_count;
    uint8_t shop_booster_count;
    uint8_t pack_count;

    uint8_t hand_levels[BALATRO_HAND_COUNT];
    uint8_t used_centers[(BALATRO_CENTER_COUNT + 7) / 8];
    uint8_t reroll_base;
    uint8_t reroll_increase;
    uint8_t free_rerolls;
    uint8_t first_shop_buffoon;
    uint8_t shop_joker_max;
    uint8_t hands_per_round;
    uint8_t discards_per_round;
    uint8_t discount_percent;
    float joker_rate;
    float tarot_rate;
    float planet_rate;
    float spectral_rate;
    float playing_card_rate;
    float edition_rate;
    uint8_t pack_choices;
    uint8_t pack_kind;
    uint8_t shop_return_phase;
    /* A duplicated free-pack tag waits until the current pack is closed. */
    uint16_t pending_free_pack_id;
    uint16_t hand_plays[BALATRO_HAND_COUNT];
    uint8_t hand_plays_round[BALATRO_HAND_COUNT];
    uint16_t tarots_used;
    uint16_t planet_usage_mask;
    uint8_t ancient_suit;
    /* The game keeps these random round targets in current-round state even
       when their Joker is not owned. Cards injected mid-round must inherit
       the already-selected target without advancing the RNG stream. */
    uint8_t idol_rank;
    uint8_t idol_suit;
    uint8_t mail_rank;
    uint8_t castle_suit;
    uint16_t blind_id;
    uint8_t blind_disabled;
    uint8_t blind_only_hand;
    uint16_t blind_hands_mask;
    /* Most-played poker hand. The game updates this at
       boss cash-out and The Ox compares against the stored value on the
       following boss, rather than recomputing it mid-round. */
    uint8_t most_played_hand;
    uint8_t last_hand_type;
    uint8_t base_hand_size;
    uint16_t next_boss_id;
    uint8_t boss_rerolled;
    uint8_t boss_usage[BALATRO_BLIND_COUNT];
    uint8_t double_tag;
    uint8_t blind_tags[2];
    uint8_t orbital_hands[3];
    uint8_t active_tag;
    uint16_t unused_discards;
    uint8_t tag_hand_bonus;
    uint8_t tag_force_rarity;
    uint8_t tag_force_rarity_count;
    uint8_t tag_force_edition;
    uint8_t tag_force_edition_count;
    uint8_t tag_voucher_pending;
    uint8_t tag_coupon_pending;
    uint8_t tag_coupon_active;
    uint8_t tag_saved_discount;
    uint8_t tag_investment_pending;
    uint8_t tag_d_six_pending;
    uint8_t tag_d_six_active;
    uint8_t ecto_penalty;
    uint16_t next_voucher_id;
    uint8_t gros_michel_extinct;
    uint16_t last_tarot_planet;
    uint64_t joker_flags;
    uint64_t joker_active_flags;
} BalatroState;

typedef enum BalatroPublicCardFlag {
    BALATRO_PUBLIC_CARD_DEBUFFED = 1u << 0,
    BALATRO_PUBLIC_CARD_ETERNAL = 1u << 1,
    BALATRO_PUBLIC_CARD_PERISHABLE = 1u << 2,
    BALATRO_PUBLIC_CARD_RENTAL = 1u << 3,
    BALATRO_PUBLIC_CARD_FORCED = 1u << 4,
    BALATRO_PUBLIC_CARD_PLAYED_THIS_ANTE = 1u << 5,
    BALATRO_PUBLIC_CARD_FACE_DOWN = 1u << 6
} BalatroPublicCardFlag;

typedef struct BalatroPlayingCardVariants {
    uint16_t count;
    uint8_t rank[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint8_t suit[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint8_t enhancement[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint8_t edition[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint8_t seal[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint8_t flags[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    float perma_bonus[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint16_t owned_count[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint16_t draw_count[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint16_t hand_count[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint16_t discard_count[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint8_t valid[BALATRO_OBS_MAX_PLAYING_VARIANTS];
} BalatroPlayingCardVariants;

typedef struct BalatroObservedHand {
    uint16_t count;
    uint16_t variant[BALATRO_OBS_MAX_HAND];
    uint8_t flags[BALATRO_OBS_MAX_HAND];
    uint8_t valid[BALATRO_OBS_MAX_HAND];
} BalatroObservedHand;

typedef struct BalatroDeckSummary {
    uint16_t rank[13];
    uint16_t suit[4];
    uint16_t rank_suit[4][13];
    uint16_t enhancement[9];
    uint16_t edition[5];
    uint16_t seal[5];
    uint16_t face;
    uint16_t numbered;
    uint16_t ace;
    uint16_t stone;
    uint16_t wild;
    uint16_t steel;
    uint16_t gold;
    uint16_t glass;
    uint16_t enhanced;
    uint16_t unmodified;
    uint16_t total;
} BalatroDeckSummary;

#define BALATRO_DEFINE_PUBLIC_CARD_TABLE(name, capacity)                                                                              \
    typedef struct name {                                                                                                             \
        uint16_t count;                                                                                                               \
        uint16_t center_id[capacity];                                                                                                 \
        uint8_t rank[capacity];                                                                                                       \
        uint8_t suit[capacity];                                                                                                       \
        uint8_t enhancement[capacity];                                                                                                \
        uint8_t edition[capacity];                                                                                                    \
        uint8_t seal[capacity];                                                                                                       \
        uint8_t flags[capacity];                                                                                                      \
        float perma_bonus[capacity];                                                                                                  \
        float cost[capacity];                                                                                                         \
        float sell_cost[capacity];                                                                                                    \
        int32_t mutable_raw[4][capacity];                                                                                             \
        float mutable_value[4][capacity];                                                                                             \
        uint8_t valid[capacity];                                                                                                      \
    } name

BALATRO_DEFINE_PUBLIC_CARD_TABLE(BalatroObservedJokers, BALATRO_OBS_MAX_JOKERS);
BALATRO_DEFINE_PUBLIC_CARD_TABLE(BalatroObservedConsumables, BALATRO_OBS_MAX_CONSUMABLES);
BALATRO_DEFINE_PUBLIC_CARD_TABLE(BalatroObservedShopMain, BALATRO_OBS_MAX_SHOP_MAIN);
BALATRO_DEFINE_PUBLIC_CARD_TABLE(BalatroObservedShopVouchers, BALATRO_OBS_MAX_SHOP_VOUCHERS);
BALATRO_DEFINE_PUBLIC_CARD_TABLE(BalatroObservedShopBoosters, BALATRO_OBS_MAX_SHOP_BOOSTERS);
BALATRO_DEFINE_PUBLIC_CARD_TABLE(BalatroObservedPackCards, BALATRO_OBS_MAX_PACK_CARDS);
#undef BALATRO_DEFINE_PUBLIC_CARD_TABLE

typedef struct BalatroObservedTags {
    uint16_t count;
    uint8_t tag_id[BALATRO_OBS_MAX_TAGS];
    uint8_t orbital_hand[BALATRO_OBS_MAX_TAGS];
    /* bit 0: held/pending, bit 1: Small Blind offer, bit 2: Big Blind offer */
    uint8_t flags[BALATRO_OBS_MAX_TAGS];
    uint8_t valid[BALATRO_OBS_MAX_TAGS];
} BalatroObservedTags;

typedef struct BalatroObservedPokerHands {
    uint8_t visible[BALATRO_HAND_COUNT];
    uint32_t level[BALATRO_HAND_COUNT];
    float chips[BALATRO_HAND_COUNT];
    float mult[BALATRO_HAND_COUNT];
    uint32_t total_plays[BALATRO_HAND_COUNT];
    uint32_t round_plays[BALATRO_HAND_COUNT];
} BalatroObservedPokerHands;

typedef struct BalatroObservationScalars {
    uint16_t deck_id;
    uint16_t blind_id;
    uint16_t next_boss_id;
    uint16_t next_voucher_id;
    uint16_t last_tarot_planet;
    uint8_t stake;
    uint8_t phase;
    uint8_t blind_on_deck;
    uint8_t won;
    uint8_t terminal;
    uint8_t blind_disabled;
    uint8_t hand_sort_suit;
    uint8_t most_played_hand;
    uint8_t last_hand_type;
    uint8_t blind_skipped_mask;
    uint8_t blind_only_hand;
    uint8_t boss_rerolled;
    uint8_t free_rerolls;
    uint8_t reroll_base;
    uint8_t reroll_increase;
    uint8_t discount_percent;
    uint8_t hands_per_round;
    uint8_t discards_per_round;
    uint8_t base_hand_size;
    uint8_t pack_kind;
    uint8_t double_tag;
    uint8_t active_tag;
    uint8_t tag_hand_bonus;
    uint8_t tag_force_rarity;
    uint8_t tag_force_rarity_count;
    uint8_t tag_force_edition;
    uint8_t tag_force_edition_count;
    uint8_t tag_voucher_pending;
    uint8_t tag_coupon_pending;
    uint8_t tag_coupon_active;
    uint8_t tag_investment_pending;
    uint8_t tag_d_six_pending;
    uint8_t tag_d_six_active;
    uint8_t ecto_penalty;
    uint8_t gros_michel_extinct;
    uint32_t ante;
    uint32_t round;
    uint32_t actions_taken;
    uint32_t run_hands_played;
    uint16_t hands_left;
    uint16_t discards_left;
    uint16_t hands_played;
    uint16_t discards_used;
    uint16_t hand_size;
    uint16_t joker_slots;
    uint16_t consumable_slots;
    uint16_t skips;
    uint16_t pack_choices;
    uint16_t unused_discards;
    uint16_t blind_hands_mask;
    uint16_t tarots_used;
    uint16_t planet_usage_mask;
    /* BALATRO_NUMBER_* classification for the four transformed score fields
       below. Their float payloads are always finite for RL consumers. */
    uint8_t chips_number;
    uint8_t blind_chips_number;
    uint8_t last_hand_score_number;
    uint8_t chips_over_blind_number;
    float dollars;
    float chips;
    float blind_chips;
    float last_hand_score;
    float chips_over_blind;
    float reroll_cost;
    float round_earnings;
    float interest_cap;
    float interest_amount;
    float blind_reward;
    float joker_rate;
    float tarot_rate;
    float planet_rate;
    float spectral_rate;
    float playing_card_rate;
    float edition_rate;
    uint8_t redeemed_vouchers[BALATRO_CENTER_COUNT];
} BalatroObservationScalars;

typedef struct BalatroObservedSelection {
    uint64_t allowed_hand;
    uint64_t required_hand;
    uint8_t minimum;
    uint8_t maximum;
    uint8_t valid;
    uint8_t reserved;
} BalatroObservedSelection;

typedef struct BalatroObservedLegality {
    uint8_t action_type[BALATRO_ACTION_TYPE_COUNT];
    uint64_t primary[BALATRO_ACTION_TYPE_COUNT];
    BalatroObservedSelection play;
    BalatroObservedSelection discard;
    BalatroObservedSelection consumable[BALATRO_OBS_MAX_CONSUMABLES];
    BalatroObservedSelection shop[BALATRO_OBS_MAX_SHOP_MAIN];
    BalatroObservedSelection pack[BALATRO_OBS_MAX_PACK_CARDS];
    uint64_t hand_reorder_destination[BALATRO_OBS_MAX_HAND];
    uint64_t joker_reorder_destination[BALATRO_OBS_MAX_JOKERS];
} BalatroObservedLegality;

/* Dense, allocation-free legality representation used by the training API.
   The legacy observation embeds the same layout for ABI compatibility. */
typedef BalatroObservedLegality BalatroLegalMasks;

typedef struct BalatroObservation {
    BalatroObservationProfile profile;
    uint32_t encoded_bytes;
    uint16_t required_capacity;
    uint8_t truncation_reason;
    uint8_t overflow_section;
    BalatroObservationScalars scalars;
    BalatroPlayingCardVariants variants;
    BalatroObservedHand hand;
    BalatroDeckSummary owned_deck;
    BalatroDeckSummary draw_pile;
    BalatroObservedJokers jokers;
    BalatroObservedConsumables consumables;
    BalatroObservedShopMain shop;
    BalatroObservedShopVouchers shop_vouchers;
    BalatroObservedShopBoosters shop_boosters;
    BalatroObservedPackCards pack;
    BalatroObservedTags tags;
    BalatroObservedPokerHands poker_hands;
    BalatroObservedLegality legal;
} BalatroObservation;

/* Packed semantic observation for high-throughput RL consumers. Unlike
   BalatroObservation, live entries are count-prefixed AoS values, continuous
   fields are quantized signed-log2 Q8.8, and redeemed vouchers are bit-packed.
   The version field is the wire-format compatibility boundary. */
#define BALATRO_COMPACT_OBSERVATION_VERSION 1
#define BALATRO_COMPACT_ID_SCALARS 5
#define BALATRO_COMPACT_U8_SCALARS 39
#define BALATRO_COMPACT_U32_SCALARS 4
#define BALATRO_COMPACT_U16_SCALARS 13
#define BALATRO_COMPACT_Q_SCALARS 16
#define BALATRO_COMPACT_VOUCHER_BYTES ((BALATRO_CENTER_COUNT + 7) / 8)

#pragma pack(push, 1)
typedef struct BalatroCompactGlobals {
    uint16_t ids[BALATRO_COMPACT_ID_SCALARS];
    uint8_t u8[BALATRO_COMPACT_U8_SCALARS];
    uint32_t u32[BALATRO_COMPACT_U32_SCALARS];
    uint16_t u16[BALATRO_COMPACT_U16_SCALARS];
    int16_t q8_8[BALATRO_COMPACT_Q_SCALARS];
    uint8_t redeemed_vouchers[BALATRO_COMPACT_VOUCHER_BYTES];
} BalatroCompactGlobals;

typedef struct BalatroCompactVariant {
    uint8_t rank;
    uint8_t suit;
    uint8_t enhancement;
    uint8_t edition;
    uint8_t seal;
    uint8_t flags;
    int16_t perma_bonus_q8_8;
    uint16_t owned_count;
    uint16_t draw_count;
    uint16_t hand_count;
    uint16_t discard_count;
} BalatroCompactVariant;

typedef struct BalatroCompactVariants {
    uint16_t count;
    BalatroCompactVariant values[BALATRO_OBS_MAX_PLAYING_VARIANTS];
} BalatroCompactVariants;

typedef struct BalatroCompactHandCard {
    uint16_t variant;
    uint8_t flags;
} BalatroCompactHandCard;

typedef struct BalatroCompactHand {
    uint16_t count;
    BalatroCompactHandCard values[BALATRO_OBS_MAX_HAND];
} BalatroCompactHand;

typedef struct BalatroCompactCard {
    uint16_t center_id;
    uint8_t rank;
    uint8_t suit;
    uint8_t enhancement;
    uint8_t edition;
    uint8_t seal;
    uint8_t flags;
    int16_t numeric_q8_8[7];
} BalatroCompactCard;

#define BALATRO_DEFINE_COMPACT_CARD_ZONE(name, capacity) \
    typedef struct name { uint16_t count; BalatroCompactCard values[capacity]; } name
BALATRO_DEFINE_COMPACT_CARD_ZONE(BalatroCompactJokers, BALATRO_OBS_MAX_JOKERS);
BALATRO_DEFINE_COMPACT_CARD_ZONE(BalatroCompactConsumables, BALATRO_OBS_MAX_CONSUMABLES);
BALATRO_DEFINE_COMPACT_CARD_ZONE(BalatroCompactShopMain, BALATRO_OBS_MAX_SHOP_MAIN);
BALATRO_DEFINE_COMPACT_CARD_ZONE(BalatroCompactShopVouchers, BALATRO_OBS_MAX_SHOP_VOUCHERS);
BALATRO_DEFINE_COMPACT_CARD_ZONE(BalatroCompactShopBoosters, BALATRO_OBS_MAX_SHOP_BOOSTERS);
BALATRO_DEFINE_COMPACT_CARD_ZONE(BalatroCompactPack, BALATRO_OBS_MAX_PACK_CARDS);
#undef BALATRO_DEFINE_COMPACT_CARD_ZONE

typedef struct BalatroCompactDeckSummary {
    uint16_t rank[13];
    uint16_t suit[4];
    uint16_t rank_suit[4][13];
    uint16_t enhancement[9];
    uint16_t edition[5];
    uint16_t seal[5];
    uint16_t derived[14];
} BalatroCompactDeckSummary;

typedef struct BalatroCompactTags {
    uint16_t count;
    uint8_t tag_id[BALATRO_OBS_MAX_TAGS];
    uint8_t orbital_hand[BALATRO_OBS_MAX_TAGS];
    uint8_t flags[BALATRO_OBS_MAX_TAGS];
} BalatroCompactTags;

typedef struct BalatroCompactPokerHand {
    uint8_t visible;
    uint32_t level;
    int16_t chips_q8_8;
    int16_t mult_q8_8;
    uint32_t total_plays;
    uint32_t round_plays;
} BalatroCompactPokerHand;

typedef struct BalatroCompactObservation {
    uint16_t version;
    BalatroCompactGlobals globals;
    BalatroCompactVariants variants;
    BalatroCompactHand hand;
    BalatroCompactDeckSummary owned_deck;
    BalatroCompactDeckSummary draw_pile;
    BalatroCompactJokers jokers;
    BalatroCompactConsumables consumables;
    BalatroCompactShopMain shop;
    BalatroCompactShopVouchers shop_vouchers;
    BalatroCompactShopBoosters shop_boosters;
    BalatroCompactPack pack;
    BalatroCompactTags tags;
    BalatroCompactPokerHand poker_hands[BALATRO_HAND_COUNT];
} BalatroCompactObservation;
#pragma pack(pop)

typedef struct BalatroPolicyAction {
    uint8_t type;
    uint8_t selection_count;
    uint16_t primary;
    uint16_t selection[BALATRO_MAX_SELECTION];
    uint16_t reorder_destination;
} BalatroPolicyAction;

typedef struct BalatroStepResult {
    float reward;
    float sparse_reward;
    uint8_t terminal;
    uint8_t won;
    uint8_t ante;
    uint8_t truncated;
    uint8_t truncation_reason;
    uint8_t reserved[3];
} BalatroStepResult;

typedef struct BalatroScoreResult {
    uint8_t hand_type;
    uint8_t scoring_mask;
    uint8_t destroyed_mask;
    uint8_t reserved;
    double chips;
    double mult;
    double total;
    int32_t dollars;
} BalatroScoreResult;

size_t balatro_state_size(void);
size_t balatro_observation_size(void);
size_t balatro_compact_observation_size(void);
size_t balatro_legal_masks_size(void);
void balatro_default_config(BalatroConfig *config);
void balatro_default_observation_profile(BalatroObservationProfile *profile);
int balatro_init(BalatroState *state, const BalatroConfig *config, uint64_t seed);
int balatro_init_seed_string(BalatroState *state, const BalatroConfig *config, const char *seed);
int balatro_reset(BalatroState *state, uint64_t seed);
int balatro_reset_seed_string(BalatroState *state, const char *seed);
void balatro_clone_state(BalatroState *destination, const BalatroState *source);
int balatro_step(BalatroState *state, const BalatroAction *action, BalatroStepResult *out);
/* Fast planner paths. Actions must be decoded from this state's legal view. */
int balatro_step_trusted(BalatroState *state, const BalatroAction *action, BalatroStepResult *out);
int balatro_score_play_actions_trusted(const BalatroState *state, const BalatroAction *actions, size_t count, double *scores);
/* Score-only planner path. It runs scoring-time effects on an isolated state
   but deliberately skips draw, cash-out, and other post-hand transitions. */
int balatro_score_plays(const BalatroState *state, const BalatroAction *plays, size_t count, double *scores);
int balatro_observe(const BalatroState *state, BalatroObservation *out);
int balatro_observe_rl(const BalatroState *state, BalatroCompactObservation *out, BalatroLegalMasks *legal);
/* Compatibility helper: emits the packed observation, builds matching masks,
   then expands those masks into the optional group view. */
int balatro_observe_compact_legal_view(const BalatroState *state, BalatroCompactObservation *out,
                                       BalatroLegalView *legal_view);
int balatro_observe_batch(const BalatroState *states, size_t count, BalatroObservation *observations, int8_t *status);
int balatro_action_to_observation(const BalatroState *state, const BalatroAction *action, BalatroPolicyAction *out);
int balatro_action_from_observation(const BalatroState *state, const BalatroPolicyAction *policy, BalatroAction *out);
int balatro_step_policy(BalatroState *state, const BalatroPolicyAction *action, BalatroStepResult *result);
int balatro_step_observe(BalatroState *state, const BalatroPolicyAction *action, BalatroStepResult *result, BalatroObservation *observation);
/* Skips redundant legality validation when the action was decoded from the
   immediately preceding observation's legal masks. */
int balatro_step_observe_trusted(BalatroState *state, const BalatroPolicyAction *action, BalatroStepResult *result,
                                 BalatroObservation *observation);
int balatro_step_observe_batch(BalatroState *states, const BalatroPolicyAction *actions, size_t count, BalatroStepResult *results,
                               BalatroObservation *observations, int8_t *status);
int balatro_step_observe_batch_trusted(BalatroState *states, const BalatroPolicyAction *actions, size_t count, BalatroStepResult *results,
                                       BalatroObservation *observations, int8_t *status);
int balatro_step_observe_rl(BalatroState *state, const BalatroPolicyAction *action, BalatroStepResult *result,
                            BalatroCompactObservation *observation, BalatroLegalMasks *legal);
int balatro_step_observe_rl_trusted(BalatroState *state, const BalatroPolicyAction *action, BalatroStepResult *result,
                                    BalatroCompactObservation *observation, BalatroLegalMasks *legal);
int balatro_step_observe_rl_batch(BalatroState *states, const BalatroPolicyAction *actions, size_t count,
                                  BalatroStepResult *results, BalatroCompactObservation *observations,
                                  BalatroLegalMasks *legal, int8_t *status, int trusted);
int balatro_legal_masks(const BalatroState *state, BalatroLegalMasks *out);
int balatro_legal_expand(const BalatroLegalMasks *masks, BalatroLegalView *out);
int balatro_legal_view(const BalatroState *state, BalatroLegalView *out);
uint32_t balatro_legal_group_count(const BalatroLegalGroup *group);
int balatro_legal_group_action(const BalatroLegalGroup *group, uint32_t ordinal, BalatroAction *out);
int balatro_legal_group_actions(const BalatroLegalGroup *group, BalatroAction *out, size_t capacity);
uint64_t balatro_state_hash(const BalatroState *state);
size_t balatro_serialize(const BalatroState *state, void *out, size_t capacity);
int balatro_deserialize(BalatroState *state, const void *data, size_t length);
float balatro_signed_log2(double value);

double balatro_pseudohash(const char *text);
double balatro_pseudorandom(BalatroState *state, const char *stream);
BalatroHandType balatro_classify_hand(const BalatroCard *cards, size_t count, uint8_t *scoring_mask);
int64_t balatro_base_hand_score(BalatroHandType hand, uint8_t level, const BalatroCard *cards, uint8_t scoring_mask);
int balatro_score_hand(BalatroState *state, const BalatroCard *played, size_t played_count, const BalatroCard *held, size_t held_count,
                       BalatroScoreResult *out);
double balatro_blind_amount(uint8_t ante, uint8_t scaling);
double balatro_blind_target(uint8_t ante, uint8_t blind, uint8_t scaling);
int32_t balatro_calculate_round_earnings(const BalatroState *state);
int balatro_buy_shop_card(BalatroState *state, uint8_t index);
int balatro_sell_joker(BalatroState *state, uint8_t index);
int balatro_reroll_shop(BalatroState *state);
void balatro_populate_shop(BalatroState *state);
int balatro_redeem_voucher(BalatroState *state, uint8_t index);
int balatro_open_booster(BalatroState *state, uint8_t index);
int balatro_pick_pack_card(BalatroState *state, uint8_t index);
int balatro_skip_pack(BalatroState *state);

/* Debug injection used by source/live differential scenarios. These entry
   points are deliberately gated by BalatroConfig.validation so production
   agents cannot bypass the normal game lifecycle. */
int balatro_debug_add_center(BalatroState *state, uint16_t center_id, uint8_t edition, uint8_t flags);
int balatro_debug_add_playing_card(BalatroState *state, uint8_t suit, uint8_t rank, uint8_t enhancement, uint8_t edition, uint8_t seal);

#ifdef __cplusplus
}
#endif
#endif
