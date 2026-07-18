#include "balatro.h"

#include <string.h>

static const int16_t base_chips[BALATRO_HAND_COUNT] = {160, 140, 120, 100, 60, 40, 35, 30, 30, 20, 10, 5};
static const int16_t base_mult[BALATRO_HAND_COUNT] = {16, 14, 12, 8, 7, 4, 4, 4, 3, 2, 2, 1};
static const int16_t level_chips[BALATRO_HAND_COUNT] = {50, 40, 35, 40, 30, 25, 15, 30, 20, 20, 15, 10};
static const int16_t level_mult[BALATRO_HAND_COUNT] = {3, 4, 3, 4, 3, 2, 2, 3, 2, 1, 1, 1};

BalatroHandType balatro_classify_hand_rules(const BalatroCard *cards, size_t count, uint8_t *scoring_mask, int four_fingers, int shortcut,
                                            int smeared) {
    uint8_t ranks[15] = {0};
    uint8_t suits[4] = {0};
    uint8_t rank_masks[15] = {0};
    uint8_t flush_mask = 0;
    memset(scoring_mask, 0, sizeof(*scoring_mask));
    if (count == 0 || count > BALATRO_MAX_SELECTION) return BALATRO_HIGH_CARD;
    for (size_t i = 0; i < count; ++i) {
        if (cards[i].enhancement == 6) continue;
        uint8_t rank = cards[i].rank;
        if (rank >= 2 && rank <= 14) {
            ranks[rank]++;
            rank_masks[rank] |= (uint8_t)(1u << i);
        }
        if (cards[i].enhancement == 3) {
            for (int suit = 0; suit < 4; ++suit) suits[suit]++;
        } else if (cards[i].suit < 4) {
            if (smeared) {
                /* Smeared Joker makes Hearts/Diamonds and Spades/Clubs
                   interchangeable for flush detection. */
                suits[cards[i].suit == BALATRO_HEARTS || cards[i].suit == BALATRO_DIAMONDS ? 0 : 1]++;
            } else
                suits[cards[i].suit]++;
        }
    }
    int flush = -1;
    int flush_need = four_fingers ? 4 : 5;
    int suit_count = smeared ? 2 : 4;
    for (int s = 0; s < suit_count; ++s)
        if (suits[s] >= flush_need) flush = s;
    if (flush >= 0) {
        for (size_t i = 0; i < count; ++i)
            if (cards[i].enhancement != 6) {
                int mapped = smeared ? (cards[i].suit == BALATRO_HEARTS || cards[i].suit == BALATRO_DIAMONDS ? 0 : 1) : cards[i].suit;
                if (cards[i].enhancement == 3 || mapped == flush) flush_mask |= (uint8_t)(1u << i);
            }
    }
    uint8_t straight_mask = 0;
    int straight_need = four_fingers ? 4 : 5;
    for (int high = 14; high >= straight_need; --high) {
        uint8_t mask = 0;
        int valid = 1;
        for (int d = 0; d < straight_need; ++d) {
            int rank = high - d;
            if (!ranks[rank]) {
                valid = 0;
                break;
            }
            mask |= rank_masks[rank];
        }
        if (valid) {
            straight_mask = mask;
            break;
        }
    }
    if (!straight_mask) {
        int wheel_need = straight_need - 1;
        int wheel_ok = ranks[14] != 0;
        for (int rank = 2; rank <= 1 + wheel_need; ++rank)
            if (!ranks[rank]) wheel_ok = 0;
        if (wheel_ok) {
            straight_mask = rank_masks[14];
            for (int rank = 2; rank <= 1 + wheel_need; ++rank) straight_mask |= rank_masks[rank];
        }
    }
    if (!straight_mask && shortcut) {
        /* The rank scan runs A-low, 2..A and allows one absent rank after every
           found rank. Thus 2-4-6-8-10 is legal: "gaps of 1 rank" does not
           mean only one gap in the entire straight. */
        static const uint8_t order[] = {
            14, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        };
        int length = 0, last = -3;
        uint8_t mask = 0;
        for (int position = 0; position < (int)sizeof(order); ++position) {
            uint8_t rank = order[position];
            if (!ranks[rank]) continue;
            if (length && position - last > 2) {
                length = 0;
                mask = 0;
            }
            /* Ace appears at both ends of the scan; do not count the
               same physical rank twice in a single candidate. */
            if (!(mask & rank_masks[rank])) {
                mask |= rank_masks[rank];
                length++;
            }
            last = position;
            if (length >= straight_need) {
                straight_mask = mask;
                break;
            }
        }
    }
    int five = 0, four = 0, three = 0, pair1 = 0, pair2 = 0;
    for (int rank = 14; rank >= 2; --rank) {
        if (ranks[rank] == 5)
            five = rank;
        else if (ranks[rank] == 4)
            four = rank;
        else if (ranks[rank] == 3 && !three)
            three = rank;
        else if (ranks[rank] == 2) {
            if (!pair1)
                pair1 = rank;
            else if (!pair2)
                pair2 = rank;
        }
    }
    if (five && flush >= 0) {
        *scoring_mask = rank_masks[five];
        return BALATRO_FLUSH_FIVE;
    }
    if (three && pair1 && flush >= 0) {
        *scoring_mask = rank_masks[three] | rank_masks[pair1];
        return BALATRO_FLUSH_HOUSE;
    }
    if (five) {
        *scoring_mask = rank_masks[five];
        return BALATRO_FIVE_OF_A_KIND;
    }
    if (straight_mask && flush >= 0 && (straight_mask & flush_mask) == straight_mask) {
        *scoring_mask = straight_mask;
        return BALATRO_STRAIGHT_FLUSH;
    }
    if (four) {
        *scoring_mask = rank_masks[four];
        return BALATRO_FOUR_OF_A_KIND;
    }
    if (three && pair1) {
        *scoring_mask = rank_masks[three] | rank_masks[pair1];
        return BALATRO_FULL_HOUSE;
    }
    if (flush >= 0) {
        *scoring_mask = flush_mask;
        return BALATRO_FLUSH;
    }
    if (straight_mask) {
        *scoring_mask = straight_mask;
        return BALATRO_STRAIGHT;
    }
    if (three) {
        *scoring_mask = rank_masks[three];
        return BALATRO_THREE_OF_A_KIND;
    }
    if (pair1 && pair2) {
        *scoring_mask = rank_masks[pair1] | rank_masks[pair2];
        return BALATRO_TWO_PAIR;
    }
    if (pair1) {
        *scoring_mask = rank_masks[pair1];
        return BALATRO_PAIR;
    }
    for (int rank = 14; rank >= 2; --rank)
        if (ranks[rank]) {
            *scoring_mask = rank_masks[rank] & (uint8_t)(-(int8_t)rank_masks[rank]);
            break;
        }
    return BALATRO_HIGH_CARD;
}

BalatroHandType balatro_classify_hand(const BalatroCard *cards, size_t count, uint8_t *scoring_mask) {
    return balatro_classify_hand_rules(cards, count, scoring_mask, 0, 0, 0);
}

int64_t balatro_base_hand_score(BalatroHandType hand, uint8_t level, const BalatroCard *cards, uint8_t scoring_mask) {
    if (hand >= BALATRO_HAND_COUNT) return 0;
    if (level == 0) level = 1;
    int64_t chips = base_chips[hand] + level_chips[hand] * (level - 1);
    int64_t mult = base_mult[hand] + level_mult[hand] * (level - 1);
    for (size_t i = 0; i < BALATRO_MAX_SELECTION; ++i) {
        if (!(scoring_mask & (1u << i))) continue;
        uint8_t rank = cards[i].rank;
        chips += rank == 14 ? 11 : rank >= 10 ? 10 : rank;
        chips += cards[i].perma_bonus;
    }
    return chips * mult;
}
