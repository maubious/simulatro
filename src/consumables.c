#include "internal.h"
#include "content.h"

#include <math.h>
#include <string.h>

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

void apply_consumable(BalatroState *state, const BalatroAction *action, BalatroCard card) {
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
            if (!balatro_can_add_playing_cards(state, 1)) break;
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
        if (count && balatro_pseudorandom(state, "wheel_of_fortune") < balatro_probability_normal(state, 0.25)) {
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

void use_consumable(BalatroState *state, const BalatroAction *action) {
    BalatroCard card = state->consumables[action->primary];
    apply_consumable(state, action, card);
    balatro_consumable_removed(state, &card);
    memmove(&state->consumables[action->primary], &state->consumables[action->primary + 1],
            (state->consumable_count - action->primary - 1) * sizeof(BalatroCard));
    state->consumable_count--;
}

void sell_consumable(BalatroState *state, uint8_t index) {
    BalatroCard card = state->consumables[index];
    state->dollars += card.sell_cost;
    balatro_consumable_removed(state, &card);
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (!(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_CAMPFIRE)
            state->jokers[j].state[0] = (state->jokers[j].state[0] > 100 ? state->jokers[j].state[0] : 100) + 25;
    memmove(&state->consumables[index], &state->consumables[index + 1], (state->consumable_count - index - 1) * sizeof(BalatroCard));
    state->consumable_count--;
}
