#include "internal.h"
#include "content.h"

#include <math.h>
#include <string.h>

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

typedef struct PublicSnapshot {
    ObservationVariantBuild variants[BALATRO_OBS_MAX_PLAYING_VARIANTS];
    uint16_t hand_variant[BALATRO_OBS_MAX_HAND];
    BalatroDeckSummary owned_deck;
    BalatroDeckSummary draw_pile;
    uint16_t variant_count;
} PublicSnapshot;

typedef struct ObservationZoneCounts {
    uint16_t owned;
    uint16_t draw;
    uint16_t hand;
    uint16_t discard;
} ObservationZoneCounts;

static inline int simple_variant_slot(const BalatroCard *card) {
    uint8_t flags = public_playing_flags(card);
    if (card->rank < 2 || card->rank > 14 || card->suit >= 4 ||
        card->enhancement != BALATRO_ENHANCEMENT_NONE || card->edition != BALATRO_EDITION_NONE ||
        card->seal != BALATRO_SEAL_NONE || card->perma_bonus != 0 ||
        (flags != 0 && flags != BALATRO_PUBLIC_CARD_PLAYED_THIS_ANTE))
        return -1;
    return (((card->rank - 2) * 4 + card->suit) * 2) +
           (flags == BALATRO_PUBLIC_CARD_PLAYED_THIS_ANTE);
}

static inline void deck_summary_add_simple(BalatroDeckSummary *summary, const BalatroCard *card) {
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
}

static inline void snapshot_card_add(BalatroDeckSummary *summary,
                                     ObservationZoneCounts counts[13 * 4 * 2],
                                     const BalatroCard *card, uint8_t zone, int *simple) {
    if (!*simple) {
        deck_summary_add(summary, card);
        return;
    }
    int slot = simple_variant_slot(card);
    if (slot < 0) {
        *simple = 0;
        deck_summary_add(summary, card);
        return;
    }
    deck_summary_add_simple(summary, card);
    ObservationZoneCounts *entry = &counts[slot];
    entry->owned++;
    if (zone == 0)
        entry->draw++;
    else if (zone == 1)
        entry->hand++;
    else
        entry->discard++;
}

static void public_snapshot_from_state(const BalatroState *state, PublicSnapshot *snapshot) {
    snapshot->owned_deck = (BalatroDeckSummary){0};
    snapshot->draw_pile = (BalatroDeckSummary){0};
    ObservationZoneCounts simple_counts[13 * 4 * 2] = {0};
    int simple = 1;
    for (uint16_t i = 0; i < state->deck_count; ++i) {
        snapshot_card_add(&snapshot->draw_pile, simple_counts, &state->deck[i], 0, &simple);
    }
    snapshot->owned_deck = snapshot->draw_pile;
    for (uint8_t i = 0; i < state->hand_count; ++i) {
        snapshot_card_add(&snapshot->owned_deck, simple_counts, &state->hand[i], 1, &simple);
    }
    for (uint16_t i = 0; i < state->discard_count; ++i) {
        snapshot_card_add(&snapshot->owned_deck, simple_counts, &state->discard[i], 2, &simple);
    }
    if (simple) {
        uint16_t slot_to_variant[13 * 4 * 2];
        uint16_t count = 0;
        for (uint16_t slot = 0; slot < 13 * 4 * 2; ++slot) {
            const ObservationZoneCounts *counts = &simple_counts[slot];
            if (!counts->owned) continue;
            uint16_t card = (uint16_t)(slot / 2);
            ObservationVariantBuild *variant = &snapshot->variants[count];
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
            slot_to_variant[slot] = count++;
        }
        snapshot->variant_count = count;
        for (uint8_t i = 0; i < state->hand_count; ++i)
            snapshot->hand_variant[i] = slot_to_variant[simple_variant_slot(&state->hand[i])];
        return;
    }

    uint16_t count = 0;
    for (uint16_t i = 0; i < state->deck_count; ++i)
        append_observation_variant(snapshot->variants, &count, &state->deck[i], 0);
    for (uint8_t i = 0; i < state->hand_count; ++i)
        append_observation_variant(snapshot->variants, &count, &state->hand[i], 1);
    for (uint16_t i = 0; i < state->discard_count; ++i)
        append_observation_variant(snapshot->variants, &count, &state->discard[i], 2);
    snapshot->variant_count = merge_observation_variants(snapshot->variants, count);
    for (uint8_t i = 0; i < state->hand_count; ++i)
        snapshot->hand_variant[i] = find_observation_variant(snapshot->variants, snapshot->variant_count, &state->hand[i]);
}

static int observation_overflow(BalatroObservation *out, uint8_t section, uint16_t required) {
    out->truncation_reason = BALATRO_TRUNCATED_OBSERVATION_CAPACITY;
    out->overflow_section = section;
    out->required_capacity = required;
    return BALATRO_ERR_OBSERVATION_CAPACITY;
}

static int observe_legality(const BalatroState *state, BalatroObservation *out) {
    return balatro_legal_masks(state, &out->legal);
}

typedef struct ObservationPokerHandBuild {
    uint8_t visible;
    uint32_t level;
    float chips;
    float mult;
    uint32_t total_plays;
    uint32_t round_plays;
} ObservationPokerHandBuild;

static ObservationPokerHandBuild observation_poker_hand(const BalatroState *state, uint8_t hand) {
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
    uint32_t level = state->hand_levels[hand] ? state->hand_levels[hand] : 1;
    return (ObservationPokerHandBuild){
        .visible = (uint8_t)(hand >= BALATRO_STRAIGHT_FLUSH || state->hand_plays[hand] != 0),
        .level = level,
        .chips =
            level == 1 ? base_chips_log[hand] : balatro_signed_log2(base_chips[hand] + (double)level_chips[hand] * (level - 1)),
        .mult = level == 1 ? base_mult_log[hand]
                           : balatro_signed_log2(base_mult[hand] + (double)level_mult[hand] * (level - 1)),
        .total_plays = state->hand_plays[hand],
        .round_plays = state->hand_plays_round[hand],
    };
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
    if (!balatro_state_layout_valid(state)) return BALATRO_ERR_INVARIANT;
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

    if (state->shop_main_count > BALATRO_OBS_MAX_SHOP_MAIN)
        return observation_overflow(out, BALATRO_OBS_SECTION_SHOP_MAIN, state->shop_main_count);
    if (state->shop_voucher_count > profile->shop_vouchers)
        return observation_overflow(out, BALATRO_OBS_SECTION_SHOP_VOUCHERS, state->shop_voucher_count);
    if (state->shop_booster_count > BALATRO_OBS_MAX_SHOP_BOOSTERS)
        return observation_overflow(out, BALATRO_OBS_SECTION_SHOP_BOOSTERS, state->shop_booster_count);

    PublicSnapshot snapshot;
    public_snapshot_from_state(state, &snapshot);
    if (snapshot.variant_count > profile->playing_variants)
        return observation_overflow(out, BALATRO_OBS_SECTION_PLAYING_VARIANTS, snapshot.variant_count);
    out->variants.count = snapshot.variant_count;
    for (uint16_t i = 0; i < snapshot.variant_count; ++i) {
        out->variants.rank[i] = snapshot.variants[i].rank;
        out->variants.suit[i] = snapshot.variants[i].suit;
        out->variants.enhancement[i] = snapshot.variants[i].enhancement;
        out->variants.edition[i] = snapshot.variants[i].edition;
        out->variants.seal[i] = snapshot.variants[i].seal;
        out->variants.flags[i] = snapshot.variants[i].flags;
        out->variants.perma_bonus[i] = balatro_signed_log2(snapshot.variants[i].perma_bonus);
        out->variants.owned_count[i] = snapshot.variants[i].owned_count;
        out->variants.draw_count[i] = snapshot.variants[i].draw_count;
        out->variants.hand_count[i] = snapshot.variants[i].hand_count;
        out->variants.discard_count[i] = snapshot.variants[i].discard_count;
        out->variants.valid[i] = 1;
    }
    out->owned_deck = snapshot.owned_deck;
    out->draw_pile = snapshot.draw_pile;
    out->hand.count = state->hand_count;
    for (uint8_t i = 0; i < state->hand_count; ++i) {
        out->hand.variant[i] = snapshot.hand_variant[i];
        out->hand.flags[i] = public_playing_flags(&state->hand[i]);
        out->hand.valid[i] = 1;
    }
    out->jokers.count = state->joker_count;
    for (uint8_t i = 0; i < state->joker_count; ++i) OBSERVE_TABLE_CARD(out->jokers, i, &state->jokers[i], 0);
    out->consumables.count = state->consumable_count;
    for (uint8_t i = 0; i < state->consumable_count; ++i) OBSERVE_TABLE_CARD(out->consumables, i, &state->consumables[i], 0);
    out->shop.count = state->shop_main_count;
    for (uint8_t i = 0; i < state->shop_main_count; ++i) {
        uint8_t set = balatro_card_set(&state->shop_main[i]);
        OBSERVE_TABLE_CARD(out->shop, i, &state->shop_main[i], set == SET_DEFAULT || set == SET_ENHANCED);
    }
    out->shop_vouchers.count = state->shop_voucher_count;
    for (uint8_t i = 0; i < state->shop_voucher_count; ++i)
        OBSERVE_TABLE_CARD(out->shop_vouchers, i, &state->shop_vouchers[i], 0);
    out->shop_boosters.count = state->shop_booster_count;
    for (uint8_t i = 0; i < state->shop_booster_count; ++i)
        OBSERVE_TABLE_CARD(out->shop_boosters, i, &state->shop_boosters[i], 0);
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

    for (uint8_t hand = 0; hand < BALATRO_HAND_COUNT; ++hand) {
        ObservationPokerHandBuild poker_hand = observation_poker_hand(state, hand);
        out->poker_hands.visible[hand] = poker_hand.visible;
        out->poker_hands.level[hand] = poker_hand.level;
        out->poker_hands.chips[hand] = poker_hand.chips;
        out->poker_hands.mult[hand] = poker_hand.mult;
        out->poker_hands.total_plays[hand] = poker_hand.total_plays;
        out->poker_hands.round_plays[hand] = poker_hand.round_plays;
    }

    BalatroObservationScalars *scalars = &out->scalars;
    scalars->deck_id = state->config.deck;
    scalars->blind_id = state->blind_id;
    scalars->next_boss_id = state->next_boss_id;
    /* The next voucher is sampled before its shop is entered, but that RNG
       result is not public until the voucher card is actually visible. */
    for (uint8_t i = 0; i < state->shop_voucher_count; ++i)
        if (state->shop_vouchers[i].center_id == state->next_voucher_id) {
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

static int16_t compact_q8_8(float value) {
    if (!(value == value)) return 0;
    if (value > 127.99609375f) value = 127.99609375f;
    if (value < -128.0f) value = -128.0f;
    float scaled = value * 256.0f;
    return (int16_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static BalatroCompactDeckSummary compact_deck_summary(const BalatroDeckSummary *src) {
    BalatroCompactDeckSummary out = {0};
    memcpy(out.rank, src->rank, sizeof(out.rank));
    memcpy(out.suit, src->suit, sizeof(out.suit));
    memcpy(out.rank_suit, src->rank_suit, sizeof(out.rank_suit));
    memcpy(out.enhancement, src->enhancement, sizeof(out.enhancement));
    memcpy(out.edition, src->edition, sizeof(out.edition));
    memcpy(out.seal, src->seal, sizeof(out.seal));
    const uint16_t derived[] = {
        src->face, src->numbered, src->ace, src->stone, src->wild, src->steel, src->gold,
        src->glass, src->enhanced, src->unmodified, src->total, 0, 0, 0,
    };
    memcpy(out.derived, derived, sizeof(out.derived));
    return out;
}

static BalatroCompactCard compact_card(const BalatroCard *card, int playing) {
    BalatroCompactCard out = {
        .center_id = card->center_id,
        .rank = card->rank,
        .suit = card->suit,
        .enhancement = card->enhancement,
        .edition = card->edition,
        .seal = card->seal,
        .flags = playing ? public_playing_flags(card) : card->flags,
    };
    out.numeric_q8_8[0] = compact_q8_8(balatro_signed_log2(card->perma_bonus));
    out.numeric_q8_8[1] = compact_q8_8(balatro_signed_log2(card->cost));
    out.numeric_q8_8[2] = compact_q8_8(balatro_signed_log2(card->sell_cost));
    for (int i = 0; i < 4; ++i) out.numeric_q8_8[3 + i] = compact_q8_8(balatro_signed_log2(card->state[i]));
    return out;
}

int balatro_observe_rl(const BalatroState *state, BalatroCompactObservation *out, BalatroLegalMasks *legal) {
    if (!state || !out || !legal) return BALATRO_ERR_ARGUMENT;
    if (!balatro_state_layout_valid(state)) return BALATRO_ERR_INVARIANT;
    out->version = BALATRO_COMPACT_OBSERVATION_VERSION;
    out->tags.count = 0;

    const BalatroObservationProfile *profile = &state->config.observation;
    uint16_t playing_count = (uint16_t)(state->deck_count + state->hand_count + state->discard_count);
    if (playing_count > profile->playing_cards || state->hand_count > profile->hand ||
        state->joker_count > profile->jokers || state->consumable_count > profile->consumables ||
        state->pack_count > BALATRO_OBS_MAX_PACK_CARDS)
        return BALATRO_ERR_OBSERVATION_CAPACITY;

    if (state->shop_main_count > BALATRO_OBS_MAX_SHOP_MAIN ||
        state->shop_voucher_count > profile->shop_vouchers ||
        state->shop_booster_count > BALATRO_OBS_MAX_SHOP_BOOSTERS)
        return BALATRO_ERR_OBSERVATION_CAPACITY;

    PublicSnapshot snapshot;
    public_snapshot_from_state(state, &snapshot);
    if (snapshot.variant_count > profile->playing_variants) return BALATRO_ERR_OBSERVATION_CAPACITY;
    for (uint16_t i = 0; i < snapshot.variant_count; ++i) {
        const ObservationVariantBuild *variant = &snapshot.variants[i];
        out->variants.values[i] = (BalatroCompactVariant){
            .rank = variant->rank,
            .suit = variant->suit,
            .enhancement = variant->enhancement,
            .edition = variant->edition,
            .seal = variant->seal,
            .flags = variant->flags,
            .perma_bonus_q8_8 = variant->perma_bonus ? compact_q8_8(balatro_signed_log2(variant->perma_bonus)) : 0,
            .owned_count = variant->owned_count,
            .draw_count = variant->draw_count,
            .hand_count = variant->hand_count,
            .discard_count = variant->discard_count,
        };
    }
    out->variants.count = snapshot.variant_count;
    out->hand.count = state->hand_count;
    for (uint8_t i = 0; i < state->hand_count; ++i) {
        out->hand.values[i] = (BalatroCompactHandCard){
            .variant = snapshot.hand_variant[i],
            .flags = public_playing_flags(&state->hand[i]),
        };
    }
    out->owned_deck = compact_deck_summary(&snapshot.owned_deck);
    out->draw_pile = compact_deck_summary(&snapshot.draw_pile);

    out->jokers.count = state->joker_count;
    for (uint8_t i = 0; i < state->joker_count; ++i) out->jokers.values[i] = compact_card(&state->jokers[i], 0);
    out->consumables.count = state->consumable_count;
    for (uint8_t i = 0; i < state->consumable_count; ++i)
        out->consumables.values[i] = compact_card(&state->consumables[i], 0);
    out->shop.count = state->shop_main_count;
    for (uint8_t i = 0; i < state->shop_main_count; ++i) {
        uint8_t set = balatro_card_set(&state->shop_main[i]);
        out->shop.values[i] = compact_card(&state->shop_main[i], set == SET_DEFAULT || set == SET_ENHANCED);
    }
    out->shop_vouchers.count = state->shop_voucher_count;
    for (uint8_t i = 0; i < state->shop_voucher_count; ++i)
        out->shop_vouchers.values[i] = compact_card(&state->shop_vouchers[i], 0);
    out->shop_boosters.count = state->shop_booster_count;
    for (uint8_t i = 0; i < state->shop_booster_count; ++i)
        out->shop_boosters.values[i] = compact_card(&state->shop_boosters[i], 0);
    out->pack.count = state->pack_count;
    for (uint8_t i = 0; i < state->pack_count; ++i) {
        uint8_t set = balatro_card_set(&state->pack_cards[i]);
        out->pack.values[i] = compact_card(&state->pack_cards[i], set == SET_DEFAULT || set == SET_ENHANCED);
    }

    uint16_t tag_count = (uint16_t)(state->double_tag != 0) + (state->blind_tags[0] != BALATRO_TAG_NONE) +
                         (state->blind_tags[1] != BALATRO_TAG_NONE);
    if (tag_count > profile->tags) return BALATRO_ERR_OBSERVATION_CAPACITY;
    if (state->double_tag) {
        uint16_t slot = out->tags.count++;
        out->tags.tag_id[slot] = BALATRO_TAG_TAG_DOUBLE;
        out->tags.flags[slot] = 1;
    }
    for (uint8_t blind = 0; blind < 2; ++blind)
        if (state->blind_tags[blind] != BALATRO_TAG_NONE) {
            uint16_t slot = out->tags.count++;
            out->tags.tag_id[slot] = state->blind_tags[blind];
            out->tags.orbital_hand[slot] = state->orbital_hands[blind];
            out->tags.flags[slot] = (uint8_t)(1u << (blind + 1));
        }

    for (uint8_t hand = 0; hand < BALATRO_HAND_COUNT; ++hand) {
        ObservationPokerHandBuild poker_hand = observation_poker_hand(state, hand);
        out->poker_hands[hand] = (BalatroCompactPokerHand){
            .visible = poker_hand.visible,
            .level = poker_hand.level,
            .chips_q8_8 = compact_q8_8(poker_hand.chips),
            .mult_q8_8 = compact_q8_8(poker_hand.mult),
            .total_plays = poker_hand.total_plays,
            .round_plays = poker_hand.round_plays,
        };
    }

    uint16_t next_voucher_id = 0;
    for (uint8_t i = 0; i < state->shop_voucher_count; ++i)
        if (state->shop_vouchers[i].center_id == state->next_voucher_id) {
            next_voucher_id = state->next_voucher_id;
            break;
        }
    double chips_over_blind = state->blind_chips ? state->chips / state->blind_chips : 0.0;
    BalatroCompactGlobals globals = {0};
    const uint16_t ids[BALATRO_COMPACT_ID_SCALARS] = {
        state->config.deck, state->blind_id, state->next_boss_id, next_voucher_id, state->last_tarot_planet,
    };
    memcpy(globals.ids, ids, sizeof(ids));
    const uint8_t u8[BALATRO_COMPACT_U8_SCALARS] = {
        state->config.stake, state->phase, state->blind_on_deck, state->won, state->terminal,
        state->blind_disabled, state->hand_sort_suit, state->most_played_hand, state->last_hand_type,
        state->blind_skipped_mask, state->blind_only_hand, state->boss_rerolled, state->free_rerolls,
        state->reroll_base, state->reroll_increase, state->discount_percent, state->hands_per_round,
        state->discards_per_round, state->base_hand_size, state->pack_kind, state->double_tag, state->active_tag,
        state->tag_hand_bonus, state->tag_force_rarity, state->tag_force_rarity_count, state->tag_force_edition,
        state->tag_force_edition_count, state->tag_voucher_pending, state->tag_coupon_pending, state->tag_coupon_active,
        state->tag_investment_pending, state->tag_d_six_pending, state->tag_d_six_active, state->ecto_penalty,
        state->gros_michel_extinct, number_class(state->chips), number_class(state->blind_chips),
        number_class(state->last_hand_score), number_class(chips_over_blind),
    };
    memcpy(globals.u8, u8, sizeof(u8));
    const uint32_t u32[BALATRO_COMPACT_U32_SCALARS] = {
        state->ante, state->round, state->actions_taken, state->run_hands_played,
    };
    memcpy(globals.u32, u32, sizeof(u32));
    const uint16_t u16[BALATRO_COMPACT_U16_SCALARS] = {
        state->hands_left, state->discards_left, state->hands_played, state->discards_used, state->hand_size,
        state->joker_slots, state->consumable_slots, state->skips, state->pack_choices, state->unused_discards,
        state->blind_hands_mask, state->tarots_used, state->planet_usage_mask,
    };
    memcpy(globals.u16, u16, sizeof(u16));
    const float q[BALATRO_COMPACT_Q_SCALARS] = {
        balatro_signed_log2(state->dollars), balatro_signed_log2(state->chips),
        balatro_signed_log2(state->blind_chips), balatro_signed_log2(state->last_hand_score),
        balatro_signed_log2(chips_over_blind), balatro_signed_log2(state->reroll_cost),
        balatro_signed_log2(state->round_earnings), balatro_signed_log2(state->interest_cap),
        balatro_signed_log2(state->interest_amount), balatro_signed_log2(state->blind_reward), state->joker_rate,
        state->tarot_rate, state->planet_rate, state->spectral_rate, state->playing_card_rate, state->edition_rate,
    };
    for (int i = 0; i < BALATRO_COMPACT_Q_SCALARS; ++i) globals.q8_8[i] = compact_q8_8(q[i]);
    size_t voucher_count = 0;
    const uint16_t *vouchers = balatro_center_pool(SET_VOUCHER, &voucher_count);
    for (size_t i = 0; i < voucher_count; ++i) {
        uint16_t id = vouchers[i];
        if ((state->used_centers[id / 8] >> (id % 8)) & 1u)
            globals.redeemed_vouchers[id / 8] |= (uint8_t)(1u << (id % 8));
    }
    out->globals = globals;
    return balatro_legal_masks(state, legal);
}

int balatro_observe_compact_legal_view(const BalatroState *state, BalatroCompactObservation *out,
                                       BalatroLegalView *legal_view) {
    if (!legal_view) return BALATRO_ERR_ARGUMENT;
    BalatroLegalMasks masks;
    int error = balatro_observe_rl(state, out, &masks);
    return error ? error : balatro_legal_expand(&masks, legal_view);
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

