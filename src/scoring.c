#include "balatro.h"
#include "internal.h"
#include "content.h"

#include <math.h>
#include <string.h>

static const int16_t base_chips[BALATRO_HAND_COUNT] = {160, 140, 120, 100, 60, 40, 35, 30, 30, 20, 10, 5};
static const int16_t base_mult[BALATRO_HAND_COUNT] = {16, 14, 12, 8, 7, 4, 4, 4, 3, 2, 2, 1};
static const int16_t level_chips[BALATRO_HAND_COUNT] = {50, 40, 35, 40, 30, 25, 15, 30, 20, 20, 15, 10};
static const int16_t level_mult[BALATRO_HAND_COUNT] = {3, 4, 3, 4, 3, 2, 2, 3, 2, 1, 1, 1};

static int rank_chips(uint8_t rank) {
    return rank == 14 ? 11 : rank >= 10 ? 10 : rank;
}

static void apply_edition(const BalatroCard *card, double *chips, double *mult) {
    if (card->edition == 1)
        *chips += 50.0;
    else if (card->edition == 2)
        *mult += 10.0;
    else if (card->edition == 3)
        *mult *= 1.5;
}

static int hand_contains(BalatroHandType hand, BalatroHandType wanted, const BalatroCard *played, size_t played_count) {
    /* Jokers receive the complete poker-hands table, not only the
       highest-scoring hand. A Flush can therefore also contain a Pair (as in
       a duplicated deck), Three of a Kind, or Two Pair. Reconstruct the rank
       entries independently so those overlapping hands remain observable. */
    if (wanted == BALATRO_PAIR || wanted == BALATRO_TWO_PAIR || wanted == BALATRO_THREE_OF_A_KIND || wanted == BALATRO_FOUR_OF_A_KIND) {
        uint8_t ranks[15] = {0};
        for (size_t i = 0; i < played_count; ++i)
            if (played[i].enhancement != BALATRO_ENHANCEMENT_STONE && played[i].rank >= 2 && played[i].rank <= 14) ranks[played[i].rank]++;
        int groups_of_two = 0;
        int has_three = 0;
        int has_four = 0;
        for (uint8_t rank = 2; rank <= 14; ++rank) {
            if (ranks[rank] >= 2) groups_of_two++;
            if (ranks[rank] >= 3) has_three = 1;
            if (ranks[rank] >= 4) has_four = 1;
        }
        if (wanted == BALATRO_PAIR) return groups_of_two > 0;
        if (wanted == BALATRO_TWO_PAIR) return groups_of_two >= 2;
        if (wanted == BALATRO_THREE_OF_A_KIND) return has_three;
        return has_four;
    }
    if (hand == wanted) return 1;
    if (wanted == BALATRO_STRAIGHT) return hand == BALATRO_STRAIGHT_FLUSH;
    if (wanted == BALATRO_FLUSH) return hand == BALATRO_STRAIGHT_FLUSH || hand == BALATRO_FLUSH_HOUSE || hand == BALATRO_FLUSH_FIVE;
    return 0;
}

static int is_face(const BalatroCard *card, int pareidolia) {
    return pareidolia || (card->rank >= 11 && card->rank <= 13);
}

static int is_suit_state(const BalatroState *state, const BalatroCard *card, uint8_t suit) {
    if (card->enhancement == 3 || card->suit == suit) return 1;
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        if ((state->jokers[j].flags & 1u) || state->jokers[j].center_id != BALATRO_CENTER_J_SMEARED) continue;
        int card_red = card->suit == BALATRO_HEARTS || card->suit == BALATRO_DIAMONDS;
        int suit_red = suit == BALATRO_HEARTS || suit == BALATRO_DIAMONDS;
        int card_black = card->suit == BALATRO_CLUBS || card->suit == BALATRO_SPADES;
        int suit_black = suit == BALATRO_CLUBS || suit == BALATRO_SPADES;
        if ((card_red && suit_red) || (card_black && suit_black)) return 1;
    }
    return 0;
}

static double probability_normal(const BalatroState *state, double base) {
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_OOPS) base *= 2.0;
    return base;
}

static int matador_debuff_bonus(const BalatroState *state) {
    if (state->blind_disabled) return 0;
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_MATADOR) return 8;
    return 0;
}

static void add_score_tarot(BalatroState *state) {
    if (state->consumable_count >= state->consumable_slots) return;
    BalatroCard card = balatro_create_pooled_card(state, 4, "8ba", 0);
    state->consumables[state->consumable_count++] = card;
    balatro_consumable_added(state, &card);
}

static int joker_rarity(const BalatroCard *card) {
    return card->center_id < BALATRO_CENTER_COUNT ? balatro_centers[card->center_id].rarity : 0;
}

static int card_in_state(const BalatroState *state, const BalatroCard *card) {
    if (!card->sort_id) return 0;
    for (uint16_t i = 0; i < state->deck_count; ++i)
        if (state->deck[i].sort_id == card->sort_id) return 1;
    for (uint8_t i = 0; i < state->hand_count; ++i)
        if (state->hand[i].sort_id == card->sort_id) return 1;
    for (uint16_t i = 0; i < state->discard_count; ++i)
        if (state->discard[i].sort_id == card->sort_id) return 1;
    return 0;
}

static uint8_t resolved_joker_source(const BalatroState *state, uint8_t start) {
    uint8_t index = start;
    for (uint8_t depth = 0; depth <= state->joker_count; ++depth) {
        if (index >= state->joker_count) return UINT8_MAX;
        const BalatroCard *joker = &state->jokers[index];
        if (joker->flags & 1u) return UINT8_MAX;
        if (joker->center_id == BALATRO_CENTER_J_BLUEPRINT) {
            index = (uint8_t)(index + 1);
            continue;
        }
        if (joker->center_id == BALATRO_CENTER_J_BRAINSTORM) {
            if (index == 0) return UINT8_MAX;
            index = 0;
            continue;
        }
        return index;
    }
    return UINT8_MAX;
}

static int joker_repetitions(const BalatroState *state, const BalatroCard *card, size_t card_index, uint8_t scoring_mask, int pareidolia) {
    int repetitions = 0;
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        uint8_t source = resolved_joker_source(state, j);
        if (source == UINT8_MAX) continue;
        const BalatroCard *joker = &state->jokers[source];
        switch (joker->center_id) {
        case BALATRO_CENTER_J_HACK:
            if (card->rank >= 2 && card->rank <= 5) repetitions++;
            break;
        case BALATRO_CENTER_J_SOCK_AND_BUSKIN:
            if (is_face(card, pareidolia)) repetitions++;
            break;
        case BALATRO_CENTER_J_HANGING_CHAD: {
            size_t first = 0;
            while (first < 5 && !(scoring_mask & (1u << first))) first++;
            if (card_index == first) repetitions += 2;
            break;
        }
        case BALATRO_CENTER_J_DUSK:
            if (state->hands_left == 0) repetitions++;
            break;
        case BALATRO_CENTER_J_SELZER:
            repetitions++;
            break;
        default:
            break;
        }
    }
    return repetitions;
}

static void individual_jokers(BalatroState *state, const BalatroCard *played, size_t card_index, uint8_t scoring_mask, double *chips,
                              double *mult, int *dollars, int pareidolia) {
    const BalatroCard *card = &played[card_index];
    BalatroCard *sources[BALATRO_MAX_JOKERS * 2];
    uint8_t copied_flags[BALATRO_MAX_JOKERS * 2];
    size_t source_count = 0;
    /* Build the same evaluation stream as Card:calculate_joker: a copier
       contributes its target's individual hook, while the target is still
       evaluated at its own position later in the Joker area. */
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        uint8_t source = resolved_joker_source(state, j);
        if (source == UINT8_MAX) continue;
        sources[source_count] = &state->jokers[source];
        copied_flags[source_count++] = source != j;
    }
    for (size_t source_index = 0; source_index < source_count; ++source_index) {
        BalatroCard *joker = sources[source_index];
        int copied = copied_flags[source_index];
        switch (joker->center_id) {
        case BALATRO_CENTER_J_GREEDY_JOKER:
            if (is_suit_state(state, card, BALATRO_DIAMONDS)) *mult += 3;
            break;
        case BALATRO_CENTER_J_LUSTY_JOKER:
            if (is_suit_state(state, card, BALATRO_HEARTS)) *mult += 3;
            break;
        case BALATRO_CENTER_J_WRATHFUL_JOKER:
            if (is_suit_state(state, card, BALATRO_SPADES)) *mult += 3;
            break;
        case BALATRO_CENTER_J_GLUTTENOUS_JOKER:
            if (is_suit_state(state, card, BALATRO_CLUBS)) *mult += 3;
            break;
        case BALATRO_CENTER_J_ARROWHEAD:
            if (is_suit_state(state, card, BALATRO_SPADES)) *chips += 50;
            break;
        case BALATRO_CENTER_J_ONYX_AGATE:
            if (is_suit_state(state, card, BALATRO_CLUBS)) *mult += 7;
            break;
        case BALATRO_CENTER_J_ROUGH_GEM:
            if (is_suit_state(state, card, BALATRO_DIAMONDS)) (*dollars)++;
            break;
        case BALATRO_CENTER_J_BLOODSTONE:
            if (is_suit_state(state, card, BALATRO_HEARTS) && balatro_pseudorandom(state, "bloodstone") < probability_normal(state, 0.5))
                *mult *= 1.5;
            break;
        case BALATRO_CENTER_J_FIBONACCI:
            if (card->rank == 2 || card->rank == 3 || card->rank == 5 || card->rank == 8 || card->rank == 14) *mult += 8;
            break;
        case BALATRO_CENTER_J_SCHOLAR:
            if (card->rank == 14) {
                *chips += 20;
                *mult += 4;
            }
            break;
        case BALATRO_CENTER_J_WALKIE_TALKIE:
            if (card->rank == 10 || card->rank == 4) {
                *chips += 10;
                *mult += 4;
            }
            break;
        case BALATRO_CENTER_J_EVEN_STEVEN:
            if (card->rank <= 10 && !(card->rank & 1u)) *mult += 4;
            break;
        case BALATRO_CENTER_J_ODD_TODD:
            if ((card->rank <= 10 && (card->rank & 1u)) || card->rank == 14) *chips += 31;
            break;
        case BALATRO_CENTER_J_SCARY_FACE:
            if (is_face(card, pareidolia)) *chips += 30;
            break;
        case BALATRO_CENTER_J_SMILEY:
            if (is_face(card, pareidolia)) *mult += 5;
            break;
        case BALATRO_CENTER_J_TRIBOULET:
            if (card->rank == 12 || card->rank == 13) *mult *= 2;
            break;
        case BALATRO_CENTER_J_IDOL:
            if (joker->state[0] && card->rank == (uint8_t)joker->state[0] && is_suit_state(state, card, (uint8_t)joker->state[1]))
                *mult *= 2;
            break;
        case BALATRO_CENTER_J_PHOTOGRAPH: {
            size_t first_face = 0;
            while (first_face < 5 && (!(scoring_mask & (1u << first_face)) || !is_face(&played[first_face], pareidolia))) first_face++;
            if (card_index == first_face) *mult *= 2;
            break;
        }
        case BALATRO_CENTER_J_ANCIENT:
            if (state->ancient_suit < 4 && is_suit_state(state, card, state->ancient_suit)) *mult *= 1.5;
            break;
        case BALATRO_CENTER_J_TICKET:
            if (card->enhancement == 7)
                *dollars += 4;
            else if (is_face(card, pareidolia))
                for (uint8_t k = 0; k < state->joker_count; ++k)
                    if (state->jokers[k].center_id == BALATRO_CENTER_J_MIDAS_MASK && !(state->jokers[k].flags & 1u)) *dollars += 4;
            break;
        case BALATRO_CENTER_J_BUSINESS:
            if (is_face(card, pareidolia) && balatro_pseudorandom(state, "business") < probability_normal(state, 0.5)) *dollars += 2;
            break;
        case BALATRO_CENTER_J_HIKER:
            /* Hiker's individual hook upgrades the card once per scoring
               evaluation. Card chips were already read for this evaluation,
               so the upgrade starts with the next retrigger or later hand. */
            ((BalatroCard *)card)->perma_bonus += 5;
            break;
        case BALATRO_CENTER_J_WEE:
            /* Each evaluation upgrades Wee, but the accumulated chips are
               returned once later from its ordinary joker-main context. */
            if (!copied && card->rank == 2) {
                joker->state[0] += 8;
            }
            break;
        case BALATRO_CENTER_J_8_BALL:
            if (card->rank == 8 && state->consumable_count < state->consumable_slots &&
                balatro_pseudorandom(state, "8ball") < probability_normal(state, 0.25))
                add_score_tarot(state);
            break;
        default:
            break;
        }
    }
}

static void held_card_effects(BalatroState *state, const BalatroCard *held, size_t held_count, double *mult, int *dollars) {
    BalatroCard *sources[BALATRO_MAX_JOKERS * 2];
    size_t source_count = 0;
    /* Held-card hooks use the same ordered copier stream as scoring hooks.
       The compact implementation previously only inspected physical Mime,
       Baron, Shoot the Moon, and Raised Fist cards, so Blueprint/Brainstorm
       could silently fail to copy held-card effects. */
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        uint8_t source = resolved_joker_source(state, j);
        if (source == UINT8_MAX) continue;
        sources[source_count++] = &state->jokers[source];
    }
    int pareidolia = 0;
    int mime_count = 0;
    for (size_t j = 0; j < source_count; ++j) {
        if (sources[j]->center_id == BALATRO_CENTER_J_PAREIDOLIA) pareidolia = 1;
        if (sources[j]->center_id == BALATRO_CENTER_J_MIME) mime_count++;
    }
    for (size_t i = 0; i < held_count; ++i) {
        if (held[i].flags & 1u) continue;
        int repetitions = (held[i].seal == 2 ? 2 : 1) + mime_count;
        for (int repetition = 0; repetition < repetitions; ++repetition) {
            if (held[i].enhancement == 5) *mult *= 1.5;
            for (size_t j = 0; j < source_count; ++j) {
                const BalatroCard *joker = sources[j];
                if (joker->center_id == BALATRO_CENTER_J_SHOOT_THE_MOON && held[i].rank == 12) *mult += 13;
                if (joker->center_id == BALATRO_CENTER_J_BARON && held[i].rank == 13) *mult *= 1.5;
                if (joker->center_id == BALATRO_CENTER_J_RESERVED_PARKING && is_face(&held[i], pareidolia) &&
                    balatro_pseudorandom(state, "parking") < probability_normal(state, 0.5))
                    (*dollars)++;
            }
        }
    }
    for (size_t j = 0; j < source_count; ++j) {
        const BalatroCard *joker = sources[j];
        if (joker->center_id != BALATRO_CENTER_J_RAISED_FIST || !held_count) continue;
        size_t lowest = held_count;
        for (size_t i = 0; i < held_count; ++i) {
            if (held[i].enhancement == 6) continue;
            /* Choose the lowest non-Stone card before testing its debuff. The
               >= comparison also makes the rightmost equal-rank
               card the target.  A debuffed target suppresses the effect; it
               must not make Raised Fist retarget a higher card. */
            if (lowest == held_count || held[i].rank <= held[lowest].rank) lowest = i;
        }
        if (lowest < held_count && !(held[lowest].flags & 1u)) *mult += 2 * rank_chips(held[lowest].rank);
    }
}

static void joker_on_joker_effects(const BalatroState *state, uint8_t target_index, double *mult) {
    if (target_index >= state->joker_count) return;
    const BalatroCard *target = &state->jokers[target_index];
    if (joker_rarity(target) != 2) return;
    for (uint8_t source = 0; source < state->joker_count; ++source) {
        const BalatroCard *joker = &state->jokers[source];
        if (source == target_index || (joker->flags & 1u)) continue;
        /* Baseball Card fires from context.other_joker, after the target's
           own joker_main hook.  It is not itself a joker_main effect. */
        if (joker->center_id == BALATRO_CENTER_J_BASEBALL) *mult *= 1.5;
    }
}

static void joker_edition_before(const BalatroCard *joker, double *chips, double *mult) {
    if (joker->edition == 1)
        *chips += 50;
    else if (joker->edition == 2)
        *mult += 10;
}

static void joker_edition_after(const BalatroCard *joker, double *mult) {
    if (joker->edition == 3) *mult *= 1.5;
}

static void main_jokers(BalatroState *state, BalatroHandType hand, const BalatroCard *played, size_t played_count, uint8_t scoring_mask,
                        const BalatroCard *held, size_t held_count, double *chips, double *mult, int *dollars) {
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        BalatroCard *physical = &state->jokers[j];
        BalatroCard *joker = physical;
        uint8_t source_index = j;
        if (!(joker->flags & 1u) && joker->center_id == BALATRO_CENTER_J_BLUEPRINT) {
            if (j + 1 >= state->joker_count || (state->jokers[j + 1].flags & 1u)) {
                joker_edition_before(physical, chips, mult);
                joker_on_joker_effects(state, j, mult);
                joker_edition_after(physical, mult);
                continue;
            }
            joker = &state->jokers[j + 1];
            source_index = (uint8_t)(j + 1);
        } else if (!(joker->flags & 1u) && joker->center_id == BALATRO_CENTER_J_BRAINSTORM) {
            /* Brainstorm always targets the first Joker. A
               Brainstorm in that first slot therefore has no distinct Joker
               to copy; it does not target the card to its right. */
            if (j == 0) {
                joker_edition_before(physical, chips, mult);
                joker_on_joker_effects(state, j, mult);
                joker_edition_after(physical, mult);
                continue;
            }
            uint8_t target = 0;
            if (target >= state->joker_count || (state->jokers[target].flags & 1u)) {
                joker_edition_before(physical, chips, mult);
                joker_on_joker_effects(state, j, mult);
                joker_edition_after(physical, mult);
                continue;
            }
            joker = &state->jokers[target];
            source_index = target;
        }
        if (joker->flags & 1u) {
            joker_on_joker_effects(state, j, mult);
            continue;
        }
        /* `context.before` upgrades are suppressed for a Blueprint or
           Brainstorm copy (`context.blueprint`), while the copied Joker's
           current value still participates in its ordinary joker_main hook.
           Keep the physical target's state mutation for its own turn below. */
        int copied = source_index != j;
        /* Edition effects belong to the physical Joker being evaluated, not
           the target copied by Blueprint/Brainstorm.  Holo/Foil occur before
           joker_main; Polychrome occurs after joker-on-joker effects. */
        joker_edition_before(physical, chips, mult);
        switch (joker->center_id) {
        case BALATRO_CENTER_J_JOKER:
            *mult += 4;
            break;
        case BALATRO_CENTER_J_MISPRINT:
            *mult += floor(balatro_pseudorandom(state, "misprint") * 24);
            break;
        case BALATRO_CENTER_J_STUNTMAN:
            *chips += 250;
            break;
        case BALATRO_CENTER_J_JOLLY:
            if (hand_contains(hand, BALATRO_PAIR, played, played_count)) *mult += 8;
            break;
        case BALATRO_CENTER_J_ZANY:
            if (hand_contains(hand, BALATRO_THREE_OF_A_KIND, played, played_count)) *mult += 12;
            break;
        case BALATRO_CENTER_J_MAD:
            if (hand_contains(hand, BALATRO_TWO_PAIR, played, played_count)) *mult += 10;
            break;
        case BALATRO_CENTER_J_CRAZY:
            if (hand_contains(hand, BALATRO_STRAIGHT, played, played_count)) *mult += 12;
            break;
        case BALATRO_CENTER_J_DROLL:
            if (hand_contains(hand, BALATRO_FLUSH, played, played_count)) *mult += 10;
            break;
        case BALATRO_CENTER_J_SLY:
            if (hand_contains(hand, BALATRO_PAIR, played, played_count)) *chips += 50;
            break;
        case BALATRO_CENTER_J_WILY:
            if (hand_contains(hand, BALATRO_THREE_OF_A_KIND, played, played_count)) *chips += 100;
            break;
        case BALATRO_CENTER_J_CLEVER:
            if (hand_contains(hand, BALATRO_TWO_PAIR, played, played_count)) *chips += 80;
            break;
        case BALATRO_CENTER_J_DEVIOUS:
            if (hand_contains(hand, BALATRO_STRAIGHT, played, played_count)) *chips += 100;
            break;
        case BALATRO_CENTER_J_CRAFTY:
            if (hand_contains(hand, BALATRO_FLUSH, played, played_count)) *chips += 80;
            break;
        case BALATRO_CENTER_J_DUO:
            if (hand_contains(hand, BALATRO_PAIR, played, played_count)) *mult *= 2;
            break;
        case BALATRO_CENTER_J_TRIO:
            if (hand_contains(hand, BALATRO_THREE_OF_A_KIND, played, played_count)) *mult *= 3;
            break;
        case BALATRO_CENTER_J_FAMILY:
            if (hand_contains(hand, BALATRO_FOUR_OF_A_KIND, played, played_count)) *mult *= 4;
            break;
        case BALATRO_CENTER_J_ORDER:
            if (hand_contains(hand, BALATRO_STRAIGHT, played, played_count)) *mult *= 3;
            break;
        case BALATRO_CENTER_J_TRIBE:
            if (hand_contains(hand, BALATRO_FLUSH, played, played_count)) *mult *= 2;
            break;
        case BALATRO_CENTER_J_HALF:
            if (played_count <= 3) *mult += 20;
            break;
        case BALATRO_CENTER_J_ABSTRACT:
            *mult += state->joker_count * 3;
            break;
        case BALATRO_CENTER_J_ACROBAT:
            if (state->hands_left == 0) *mult *= 3;
            break;
        case BALATRO_CENTER_J_MYSTIC_SUMMIT:
            if (state->discards_left == 0) *mult += 15;
            break;
        case BALATRO_CENTER_J_BANNER:
            *chips += state->discards_left * 30;
            break;
        case BALATRO_CENTER_J_BLUE_JOKER:
            *chips += state->deck_count * 2;
            break;
        case BALATRO_CENTER_J_BULL:
            if (state->dollars > 0) *chips += state->dollars * 2;
            break;
        case BALATRO_CENTER_J_BLACKBOARD: {
            int all_black = held_count > 0;
            for (size_t i = 0; i < held_count; ++i)
                if (!(is_suit_state(state, &held[i], BALATRO_CLUBS) || is_suit_state(state, &held[i], BALATRO_SPADES))) all_black = 0;
            if (all_black) *mult *= 3;
            break;
        }
        case BALATRO_CENTER_J_STENCIL: {
            int stencils = 0;
            for (uint8_t k = 0; k < state->joker_count; ++k)
                if (state->jokers[k].center_id == BALATRO_CENTER_J_STENCIL && !(state->jokers[k].flags & 1u)) stencils++;
            int factor = state->joker_slots - state->joker_count + stencils;
            if (factor > 1) *mult *= factor;
            break;
        }
        case BALATRO_CENTER_J_BOOTSTRAPS:
            if (state->dollars >= 5) *mult += (state->dollars / 5) * 2;
            break;
        case BALATRO_CENTER_J_SUPERNOVA:
            *mult += state->hand_plays[hand];
            break;
        case BALATRO_CENTER_J_CARD_SHARP:
            if (state->hand_plays_round[hand] > 1) *mult *= 3;
            break;
        case BALATRO_CENTER_J_FORTUNE_TELLER:
            *mult += state->tarots_used;
            break;
        case BALATRO_CENTER_J_EROSION: {
            int cards = state->deck_count + state->hand_count + state->discard_count;
            /* Store the actual starting deck size (Abandoned Deck
               starts at 40 after removing face cards; normal decks start at
               52), rather than a universal 52 constant. */
            int starting = state->config.deck == BALATRO_CENTER_B_ABANDONED ? 40 : 52;
            if (cards < starting) *mult += (starting - cards) * 4;
            break;
        }
        case BALATRO_CENTER_J_STONE: {
            int stones = 0;
            for (uint16_t k = 0; k < state->deck_count; ++k)
                if (state->deck[k].enhancement == 6) stones++;
            for (uint8_t k = 0; k < state->hand_count; ++k)
                if (state->hand[k].enhancement == 6) stones++;
            for (uint16_t k = 0; k < state->discard_count; ++k)
                if (state->discard[k].enhancement == 6) stones++;
            for (size_t k = 0; k < played_count; ++k)
                if (played[k].enhancement == 6 && !card_in_state(state, &played[k])) stones++;
            *chips += stones * 25;
            break;
        }
        case BALATRO_CENTER_J_STEEL_JOKER: {
            int steels = 0;
            for (uint16_t k = 0; k < state->deck_count; ++k)
                if (state->deck[k].enhancement == 5) steels++;
            for (uint8_t k = 0; k < state->hand_count; ++k)
                if (state->hand[k].enhancement == 5) steels++;
            for (uint16_t k = 0; k < state->discard_count; ++k)
                if (state->discard[k].enhancement == 5) steels++;
            for (size_t k = 0; k < played_count; ++k)
                if (played[k].enhancement == 5 && !card_in_state(state, &played[k])) steels++;
            if (steels) *mult *= 1.0 + steels * 0.2;
            break;
        }
        case BALATRO_CENTER_J_DRIVERS_LICENSE: {
            int enhanced = 0;
            for (uint16_t k = 0; k < state->deck_count; ++k)
                if (state->deck[k].enhancement) enhanced++;
            for (uint8_t k = 0; k < state->hand_count; ++k)
                if (state->hand[k].enhancement) enhanced++;
            for (uint16_t k = 0; k < state->discard_count; ++k)
                if (state->discard[k].enhancement) enhanced++;
            for (size_t k = 0; k < played_count; ++k)
                if (played[k].enhancement && !card_in_state(state, &played[k])) enhanced++;
            if (enhanced >= 16) *mult *= 3;
            break;
        }
        case BALATRO_CENTER_J_FLOWER_POT: {
            uint8_t suits = 0;
            /* Evaluate ordinary suits first, then let each Wild
               Card fill the first still-missing suit.  Doing this in one
               pass makes a Wild before a duplicate suit consume the wrong
               slot and can incorrectly fail Flower Pot. */
            for (size_t k = 0; k < played_count; ++k)
                if ((scoring_mask & (1u << k)) && played[k].enhancement != 3 && played[k].suit < 4)
                    suits |= (uint8_t)(1u << played[k].suit);
            for (size_t k = 0; k < played_count; ++k)
                if ((scoring_mask & (1u << k)) && played[k].enhancement == 3)
                    for (uint8_t suit = 0; suit < 4; ++suit)
                        if (!(suits & (1u << suit))) {
                            suits |= (uint8_t)(1u << suit);
                            break;
                        }
            if (suits == 0x0f) *mult *= 3;
            break;
        }
        case BALATRO_CENTER_J_SEEING_DOUBLE: {
            uint8_t suits = 0;
            for (size_t k = 0; k < played_count; ++k)
                if (scoring_mask & (1u << k)) {
                    if (played[k].enhancement == 3)
                        suits = 0x0f;
                    else if (played[k].suit < 4)
                        suits |= (uint8_t)(1u << played[k].suit);
                }
            if ((suits & (1u << BALATRO_CLUBS)) && (suits & ~(1u << BALATRO_CLUBS))) *mult *= 2;
            break;
        }
        case BALATRO_CENTER_J_LOYALTY_CARD: {
            int every = joker->state[0] > 0 ? joker->state[0] : 5;
            int created = joker->state[2] ? joker->state[1] : 0;
            /* Hand evaluation runs before hands_played increments; the
               modulo therefore activates on the sixth hand for
               the default every=5 cycle, not the fifth. */
            int played = (int)state->run_hands_played - created;
            if (every > 0 && played > 0 && played % (every + 1) == every) *mult *= 4;
            break;
        }
        case BALATRO_CENTER_J_GREEN_JOKER:
            if (!copied) joker->state[0]++;
            *mult += joker->state[0];
            break;
        case BALATRO_CENTER_J_RIDE_THE_BUS: {
            if (!copied) {
                int face = 0;
                for (size_t k = 0; k < played_count; ++k)
                    if (scoring_mask & (1u << k)) face |= is_face(&played[k], 0);
                joker->state[0] = face ? 0 : joker->state[0] + 1;
            }
            *mult += joker->state[0];
            break;
        }
        case BALATRO_CENTER_J_TROUSERS:
            if (!copied && hand_contains(hand, BALATRO_TWO_PAIR, played, played_count)) joker->state[0] += 2;
            *mult += joker->state[0];
            break;
        case BALATRO_CENTER_J_SQUARE:
            if (!copied && played_count == 4) joker->state[0] += 4;
            *chips += joker->state[0];
            break;
        case BALATRO_CENTER_J_RUNNER:
            if (!copied && hand_contains(hand, BALATRO_STRAIGHT, played, played_count)) joker->state[0] += 15;
            *chips += joker->state[0];
            break;
        case BALATRO_CENTER_J_THROWBACK:
            *mult *= 1.0 + state->skips * 0.25;
            break;
        case BALATRO_CENTER_J_SWASHBUCKLER: {
            int sell_total = 0;
            for (uint8_t k = 0; k < state->joker_count; ++k)
                if (k != source_index && !(state->jokers[k].flags & 1u)) sell_total += state->jokers[k].sell_cost;
            *mult += sell_total;
            break;
        }
        case BALATRO_CENTER_J_WEE:
            *chips += joker->state[0];
            break;
        case BALATRO_CENTER_J_POPCORN: {
            int current = joker->state[0] > 0 ? joker->state[0] : 20;
            *mult += current;
            break;
        }
        case BALATRO_CENTER_J_FLASH:
        case BALATRO_CENTER_J_RED_CARD:
            *mult += joker->state[0];
            break;
        case BALATRO_CENTER_J_LUCKY_CAT:
        case BALATRO_CENTER_J_CAMPFIRE:
        case BALATRO_CENTER_J_HOLOGRAM:
        case BALATRO_CENTER_J_CONSTELLATION:
        case BALATRO_CENTER_J_GLASS:
        case BALATRO_CENTER_J_CAINO:
        case BALATRO_CENTER_J_MADNESS:
        case BALATRO_CENTER_J_HIT_THE_ROAD:
        case BALATRO_CENTER_J_YORICK:
            if (joker->state[0] > 100) *mult *= joker->state[0] / 100.0;
            break;
        case BALATRO_CENTER_J_RAMEN: {
            if (joker->state[1]) break;
            int x = joker->state[0] > 100 ? joker->state[0] : 200;
            *mult *= x / 100.0;
            joker->state[0] = x;
            break;
        }
        case BALATRO_CENTER_J_GROS_MICHEL:
            *mult += 15;
            break;
        case BALATRO_CENTER_J_CAVENDISH:
            *mult *= 3;
            break;
        case BALATRO_CENTER_J_ICE_CREAM: {
            if (joker->state[1]) break;
            int current = joker->state[0] > 0 ? joker->state[0] : 100;
            *chips += current;
            if (current <= 5) {
                joker->state[0] = 0;
                joker->state[1] = 1; /* context.after dissolves it */
            } else
                joker->state[0] = current - 5;
            break;
        }
        case BALATRO_CENTER_J_CASTLE:
            *chips += joker->state[0];
            break;
        case BALATRO_CENTER_J_CEREMONIAL:
            *mult += joker->state[0];
            break;
        case BALATRO_CENTER_J_TODO_LIST:
            if (joker->state[1] == (int32_t)hand) *dollars += 4;
            break;
        case BALATRO_CENTER_J_VAMPIRE: {
            if (joker->state[0] > 100) *mult *= joker->state[0] / 100.0;
            break;
        }
        case BALATRO_CENTER_J_OBELISK: {
            if (!copied) {
                int reset = 1;
                for (uint8_t h = 0; h < BALATRO_HAND_COUNT; ++h)
                    if (h != hand && state->hand_plays[h] >= state->hand_plays[hand]) {
                        reset = 0;
                        break;
                    }
                if (reset)
                    joker->state[0] = 100;
                else
                    joker->state[0] = (joker->state[0] > 100 ? joker->state[0] : 100) + 20;
            }
            if (joker->state[0] > 100) {
                /* x_mult mutates with repeated floating-point +0.2,
                   rather than deriving it from an exact integer percentage.
                   Recreate that evaluation order: at three increments the
                   stored value is just below 1.6, which can affect floor(). */
                int32_t scaled = joker->state[0];
                double x_mult;
                if ((scaled - 100) % 20) {
                    x_mult = scaled / 100.0;
                } else {
                    /* Volatile preserves the source's store-and-round after
                       every mutation; an optimizing compiler may otherwise
                       strength-reduce the loop to one multiplication. */
                    volatile double accumulated = 1.0;
                    for (int32_t increment = 0; increment < (scaled - 100) / 20; ++increment) accumulated += 0.2;
                    x_mult = accumulated;
                }
                *mult *= x_mult;
            }
            break;
        }
        default:
            break;
        }
        joker_on_joker_effects(state, j, mult);
        joker_edition_after(physical, mult);
    }
}

int balatro_score_hand(BalatroState *state, const BalatroCard *played, size_t played_count, const BalatroCard *held, size_t held_count,
                       BalatroScoreResult *out) {
    if (!state || !played || !out || played_count < 1 || played_count > 5) return BALATRO_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    balatro_refresh_joker_cache(state);
    uint8_t scoring = 0;
    int four_fingers = balatro_joker_active(state, BALATRO_CENTER_J_FOUR_FINGERS);
    int shortcut = balatro_joker_active(state, BALATRO_CENTER_J_SHORTCUT);
    int smeared = balatro_joker_active(state, BALATRO_CENTER_J_SMEARED);
    BalatroHandType hand = balatro_classify_hand_rules(played, played_count, &scoring, four_fingers, shortcut, smeared);
    /* Record this hand before debuff checks and Joker-main
       evaluation, so Supernova/Card Sharp/Obelisk observe the new count. */
    state->hand_plays[hand]++;
    state->hand_plays_round[hand]++;
    int pareidolia = balatro_joker_active(state, BALATRO_CENTER_J_PAREIDOLIA);
    if (balatro_joker_active(state, BALATRO_CENTER_J_SPLASH))
        scoring = (uint8_t)((1u << played_count) - 1u);
    for (size_t i = 0; i < played_count; ++i)
        if (played[i].enhancement == 6) scoring |= (uint8_t)(1u << i);
    int debuffed_card = 0;
    for (size_t i = 0; i < played_count; ++i)
        if ((scoring & (1u << i)) && (played[i].flags & 1u)) debuffed_card = 1;
    /* Execute Joker `before` hooks before individual card scoring.
       Preserve Joker order so Midas Mask and Vampire compose as in source. */
    for (uint8_t j = 0; j < state->joker_count; ++j) {
        BalatroCard *joker = &state->jokers[j];
        if (joker->flags & 1u) continue;
        if (joker->center_id == BALATRO_CENTER_J_MIDAS_MASK) {
            for (size_t k = 0; k < played_count; ++k)
                if ((scoring & (1u << k)) && !(played[k].flags & 1u) && is_face(&played[k], pareidolia))
                    ((BalatroCard *)&played[k])->enhancement = 7;
        } else if (joker->center_id == BALATRO_CENTER_J_VAMPIRE) {
            int enhanced = 0;
            for (size_t k = 0; k < played_count; ++k)
                if ((scoring & (1u << k)) && !(played[k].flags & 1u) && played[k].enhancement) {
                    enhanced++;
                    ((BalatroCard *)&played[k])->enhancement = 0;
                }
            if (enhanced) joker->state[0] = (joker->state[0] > 100 ? joker->state[0] : 100) + enhanced * 10;
        }
    }
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_SPACE &&
            balatro_pseudorandom(state, "space") < probability_normal(state, 0.25) && state->hand_levels[hand] < 255)
            state->hand_levels[hand]++;
    if (!state->blind_disabled) {
        if (state->blind_id == BALATRO_BLIND_BL_PSYCHIC && played_count < 5) {
            out->hand_type = hand;
            out->scoring_mask = scoring;
            out->dollars = matador_debuff_bonus(state);
            return BALATRO_OK;
        }
        if (state->blind_id == BALATRO_BLIND_BL_EYE && (state->blind_hands_mask & (1u << hand))) {
            out->hand_type = hand;
            out->scoring_mask = scoring;
            out->dollars = matador_debuff_bonus(state);
            return BALATRO_OK;
        }
        if (state->blind_id == BALATRO_BLIND_BL_MOUTH) {
            if (state->blind_only_hand == UINT8_MAX)
                state->blind_only_hand = (uint8_t)hand;
            else if (state->blind_only_hand != (uint8_t)hand) {
                out->hand_type = hand;
                out->scoring_mask = scoring;
                out->dollars = matador_debuff_bonus(state);
                return BALATRO_OK;
            }
        }
        state->blind_hands_mask |= (uint16_t)(1u << hand);
        if (state->blind_id == BALATRO_BLIND_BL_ARM && state->hand_levels[hand] > 1) state->hand_levels[hand]--;
    }
    uint8_t level = state->hand_levels[hand] ? state->hand_levels[hand] : 1;
    double chips = base_chips[hand] + level_chips[hand] * (level - 1);
    double mult = base_mult[hand] + level_mult[hand] * (level - 1);
    int dollars = 0;
    if (!state->blind_disabled && state->blind_id == BALATRO_BLIND_BL_FLINT) {
        chips = floor(chips * 0.5 + 0.5);
        mult = fmax(floor(mult * 0.5 + 0.5), 1.0);
    }
    for (size_t i = 0; i < played_count; ++i) {
        if (!(scoring & (1u << i)) || (played[i].flags & 1u)) continue;
        int repetitions = (played[i].seal == 2 ? 2 : 1) + joker_repetitions(state, &played[i], i, scoring, pareidolia);
        for (int repetition = 0; repetition < repetitions; ++repetition) {
            const BalatroCard *card = &played[i];
            chips += card->enhancement == 6 ? 50 : rank_chips(card->rank);
            chips += card->perma_bonus;
            if (card->enhancement == 1) chips += 30;
            if (card->enhancement == 2) mult += 4;
            if (card->enhancement == 4) mult *= 2;
            if (card->enhancement == 8) {
                int lucky_trigger = 0;
                if (balatro_pseudorandom(state, "lucky_mult") < probability_normal(state, 0.2)) {
                    mult += 20;
                    lucky_trigger = 1;
                }
                if (balatro_pseudorandom(state, "lucky_money") < probability_normal(state, 1.0 / 15.0)) {
                    dollars += 20;
                    lucky_trigger = 1;
                }
                /* Both successful Lucky effects set Card.lucky_trigger, and
                   Lucky Cat observes that shared flag once per evaluation. */
                if (lucky_trigger)
                    for (uint8_t j = 0; j < state->joker_count; ++j)
                        if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_LUCKY_CAT)
                            state->jokers[j].state[0] = (state->jokers[j].state[0] > 100 ? state->jokers[j].state[0] : 100) + 25;
            }
            if (card->seal == 1) dollars += 3;
            apply_edition(card, &chips, &mult);
            individual_jokers(state, played, i, scoring, &chips, &mult, &dollars, pareidolia);
        }
    }
    held_card_effects(state, held, held_count, &mult, &dollars);
    main_jokers(state, hand, played, played_count, scoring, held, held_count, &chips, &mult, &dollars);
    /* Mark the blind triggered when an individual scoring
       card is debuffed, then Matador pays during Joker-main.  The compact
       native path skips the card's effects directly, so account for that
       trigger here as well (Psychic/Eye/Mouth early exits are handled above). */
    if (debuffed_card) dollars += matador_debuff_bonus(state);
    if (balatro_center_used(state, BALATRO_CENTER_V_OBSERVATORY)) {
        for (uint8_t i = 0; i < state->consumable_count; ++i)
            if (balatro_planet_hand(state->consumables[i].center_id) == hand) mult *= 1.5;
    }
    /* Plasma Deck's Back:trigger_effect(final_scoring_step) runs after every
       Joker/edition effect and balances the two final score components. */
    if (state->config.deck == BALATRO_CENTER_B_PLASMA) {
        double balanced = floor((chips + mult) / 2.0);
        chips = balanced;
        mult = balanced;
    }
    out->hand_type = hand;
    out->scoring_mask = scoring;
    out->chips = chips;
    out->mult = mult;
    out->total = floor(chips * mult);
    out->dollars = dollars;
    /* Glass destruction is resolved after all scoring phases in the Lua
       state machine.  Return a mask so the transition layer can remove the
       destroyed cards from play/discard after Joker destruction hooks. */
    for (size_t i = 0; i < played_count; ++i) {
        if ((scoring & (1u << i)) && !(played[i].flags & 1u) && played[i].enhancement == 4 &&
            balatro_pseudorandom(state, "glass") < probability_normal(state, 0.25))
            out->destroyed_mask |= (uint8_t)(1u << i);
    }
    if (!state->blind_disabled && state->blind_id == BALATRO_BLIND_BL_TOOTH) out->dollars -= (int32_t)played_count;
    return BALATRO_OK;
}
