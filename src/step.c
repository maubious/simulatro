#include "internal.h"
#include "content.h"

#include <math.h>
#include <string.h>

static int joker_active(const BalatroState *state, uint16_t center_id) {
    return balatro_joker_active(state, center_id);
}

static void end_round_jokers(BalatroState *state);

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

/* A bounded training potential tied to the actual objective.  The old shaped
   reward used raw chip and money deltas, which makes spending money look bad
   and makes a successful blind look bad when chips reset for the next one.
   Keep the potential independent of card identity/economy so it cannot alter
   game rules or the sparse evaluation metric. */
static double progress_potential(const BalatroState *state) {
    const double target_blinds = (double)((state->config.win_ante > 1 ?
        state->config.win_ante - 1 : 1) * 3 + 2);
    double completed = state->ante > 0 ? (double)(state->ante - 1) * 3.0 : 0.0;
    if (state->blind_on_deck <= 2) completed += (double)state->blind_on_deck;
    double stage = completed / target_blinds;
    if (stage < 0.0) stage = 0.0;
    if (stage > 1.0) stage = 1.0;
    double fraction = 0.0;
    if (state->phase == BALATRO_PHASE_SELECTING_HAND && state->blind_chips > 0) {
        fraction = state->chips / state->blind_chips;
        if (isnan(fraction)) fraction = 0.0;
        if (fraction < 0.0) fraction = 0.0;
        if (fraction > 1.0) fraction = 1.0;
    }
    /* Stage dominates the small within-blind score signal, so resetting chips
       after a successful blind remains net positive. */
    return 2.0 * stage + 0.05 * fraction;
}

void remove_joker_at(BalatroState *state, uint8_t index) {
    BalatroCard removed = state->jokers[index];
    balatro_joker_removed(state, &removed);
    memmove(&state->jokers[index], &state->jokers[index + 1], (state->joker_count - index - 1) * sizeof(BalatroCard));
    state->joker_count--;
    balatro_refresh_joker_cache(state);
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
            if (balatro_pseudorandom(state, stream) < balatro_probability_normal(state, 1.0 / odds)) {
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
    if (state->blind_id == BALATRO_BLIND_BL_NEEDLE)
        state->blind_chips /= 2;
    else if (state->blind_id == BALATRO_BLIND_BL_WALL)
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
        if (joker->center_id == BALATRO_CENTER_J_MARBLE && state->deck_count < BALATRO_MAX_DECK &&
            balatro_can_add_playing_cards(state, 1)) {
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
    if (joker_active(state, BALATRO_CENTER_J_CERTIFICATE) && state->hand_count < BALATRO_MAX_HAND &&
        balatro_can_add_playing_cards(state, 1)) {
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
        if (first_hand && count == 1 && joker_active(state, BALATRO_CENTER_J_DNA) && state->hand_count < BALATRO_MAX_HAND &&
            balatro_can_add_playing_cards(state, 1)) {
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
    BalatroCard card = state->shop_main[action->primary];
    /* Remove the shop card and pay before use, but never emplace
       Buy & Use consumables in G.consumeables. Card:add_to_deck/remove_from_deck
       hooks still bracket the effect (notably for Negative editions). */
    state->dollars -= card.cost;
    memmove(&state->shop_main[action->primary], &state->shop_main[action->primary + 1],
            (state->shop_main_count - action->primary - 1) * sizeof(BalatroCard));
    state->shop_main_count--;
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
    state->shop_main_count = 0;
    state->shop_voucher_count = 0;
    state->shop_booster_count = 0;
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
    if (!trusted && !balatro_state_layout_valid(state)) return BALATRO_ERR_INVARIANT;
    balatro_refresh_joker_cache(state);
    if (!trusted && !balatro_action_is_legal(state, action)) return BALATRO_ERR_ACTION;
    double previous_progress = progress_potential(state);
    state->actions_taken++;
    apply_action_transition(state, action);
    out->terminal = state->terminal;
    out->won = state->won;
    out->ante = state->ante;
    out->sparse_reward = state->won ? 1.0f : 0.0f;
    out->reward = 0.0f;
    if (state->config.shaped_reward) {
        double next_progress = progress_potential(state);
        double progress_delta = next_progress - previous_progress;
        /* Skipping is a valid strategic action, but it is not blind
           completion.  Do not leak a positive progress reward for it. */
        if (action->type == BALATRO_ACTION_SKIP_BLIND && progress_delta > 0.0) progress_delta = 0.0;
        if (progress_delta > 0.25) progress_delta = 0.25;
        if (progress_delta < -0.25) progress_delta = -0.25;
        /* Keep sparse_reward as a win-only evaluation metric, but do not mix
           it into the shaped training reward. */
        out->reward = (float)progress_delta;
    }
    return BALATRO_OK;
}

int balatro_step(BalatroState *state, const BalatroAction *action, BalatroStepResult *out) {
    return balatro_step_impl(state, action, out, 0);
}

int balatro_step_trusted(BalatroState *state, const BalatroAction *action, BalatroStepResult *out) {
    return balatro_step_impl(state, action, out, 1);
}

int balatro_score_plays(const BalatroState *state, const BalatroAction *actions, size_t count, double *scores) {
    if (!state || (!actions && count) || (!scores && count)) return BALATRO_ERR_ARGUMENT;
    int simple_score = state->joker_count == 0 && state->blind_on_deck < 2 &&
                       state->config.deck != BALATRO_CENTER_B_PLASMA &&
                       !balatro_center_used(state, BALATRO_CENTER_V_OBSERVATORY);
    for (uint8_t hand_index = 0; simple_score && hand_index < state->hand_count; ++hand_index) {
        const BalatroCard *card = &state->hand[hand_index];
        if (card->enhancement != BALATRO_ENHANCEMENT_NONE || card->edition != BALATRO_EDITION_NONE ||
            card->seal != BALATRO_SEAL_NONE || card->perma_bonus != 0 ||
            (card->flags & BALATRO_CARD_DEBUFFED))
            simple_score = 0;
    }
    for (size_t i = 0; i < count; ++i) {
        const BalatroAction *action = &actions[i];
        if (action->type != BALATRO_ACTION_PLAY_HAND || action->selection_count < 1 ||
            action->selection_count > BALATRO_MAX_SELECTION)
            return BALATRO_ERR_ACTION;
        for (uint8_t selected_index = 0; selected_index < action->selection_count; ++selected_index)
            if (action->selection[selected_index] >= state->hand_count ||
                (selected_index && action->selection[selected_index - 1] >= action->selection[selected_index]))
                return BALATRO_ERR_ACTION;

        if (simple_score) {
            BalatroCard played[BALATRO_MAX_SELECTION] = {0};
            for (uint8_t selected_index = 0; selected_index < action->selection_count; ++selected_index)
                played[selected_index] = state->hand[action->selection[selected_index]];
            uint8_t scoring_mask = 0;
            BalatroHandType hand = balatro_classify_hand(played, action->selection_count, &scoring_mask);
            scores[i] = (double)balatro_base_hand_score(hand, state->hand_levels[hand], played, scoring_mask);
            continue;
        }

        BalatroState candidate;
        balatro_clone_state(&candidate, state);
        BalatroCard played[BALATRO_MAX_SELECTION] = {0};
        int played_count = remove_selected(&candidate, action, played);
        if (candidate.hands_played == 0 && played_count == 1 &&
            joker_active(&candidate, BALATRO_CENTER_J_DNA) && candidate.hand_count < BALATRO_MAX_HAND &&
            balatro_can_add_playing_cards(&candidate, 1)) {
            BalatroCard copy = played[0];
            copy.sort_id = ++candidate.next_sort_id;
            copy.flags &= (uint8_t)~BALATRO_CARD_DEBUFFED;
            candidate.hand[candidate.hand_count++] = copy;
            sort_hand_desc(&candidate);
            balatro_playing_card_added(&candidate, 1);
        }
        apply_hook_discard(&candidate);
        for (uint8_t held = 0; held < candidate.hand_count; ++held)
            candidate.hand[held].flags &= (uint8_t)~(1u << 4);
        if (candidate.hands_left) candidate.hands_left--;
        BalatroScoreResult score;
        int error = balatro_score_hand(&candidate, played, (size_t)played_count,
                                       candidate.hand, candidate.hand_count, &score);
        if (error) return error;
        scores[i] = score.total;
    }
    return BALATRO_OK;
}

int balatro_score_play_actions_trusted(const BalatroState *state, const BalatroAction *actions, size_t count, double *scores) {
    return balatro_score_plays(state, actions, count, scores);
}

static int action_has_primary(uint8_t type) {
    return type >= BALATRO_ACTION_BUY_CARD && type <= BALATRO_ACTION_SWAP_HAND_RIGHT;
}

static int observed_action_primary(const BalatroState *state, uint8_t type, uint8_t native_primary) {
    (void)state;
    return action_has_primary(type) ? native_primary : 0;
}

int balatro_action_to_observation(const BalatroState *state, const BalatroAction *action, BalatroPolicyAction *out) {
    if (!state || !action || !out || action->type >= BALATRO_ACTION_TYPE_COUNT ||
        action->selection_count > BALATRO_MAX_SELECTION)
        return BALATRO_ERR_ARGUMENT;
    int primary = observed_action_primary(state, action->type, action->primary);
    if (primary < 0 || primary >= 64) return BALATRO_ERR_ACTION;
    *out = (BalatroPolicyAction){
        .type = action->type,
        .selection_count = action->selection_count,
        .primary = (uint16_t)primary,
    };
    for (uint8_t i = 0; i < action->selection_count; ++i) out->selection[i] = action->selection[i];
    return BALATRO_OK;
}

static int translate_policy_action(const BalatroState *state, const BalatroPolicyAction *policy, BalatroAction *out) {
    if (!state || !policy || !out || policy->type >= BALATRO_ACTION_TYPE_COUNT || policy->selection_count > BALATRO_MAX_SELECTION)
        return BALATRO_ERR_ARGUMENT;
    *out = (BalatroAction){.type = policy->type, .selection_count = policy->selection_count};
    int primary = policy->primary;
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
    return balatro_action_is_legal(state, out) ? BALATRO_OK : BALATRO_ERR_ACTION;
}

int balatro_step_policy(BalatroState *state, const BalatroPolicyAction *action, BalatroStepResult *result) {
    BalatroAction native;
    int error = translate_policy_action(state, action, &native);
    if (error) return error;
    return balatro_step_impl(state, &native, result, 0);
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

static int step_observe_rl_impl(BalatroState *state, const BalatroPolicyAction *action,
                                BalatroStepResult *result, BalatroCompactObservation *observation,
                                BalatroLegalMasks *legal, int trusted) {
    if (!state || !action || !result || !observation || !legal) return BALATRO_ERR_ARGUMENT;
    BalatroAction native;
    int error = translate_policy_action(state, action, &native);
    if (error) return error;
    error = balatro_step_impl(state, &native, result, trusted);
    if (error) return error;
    error = balatro_observe_rl(state, observation, legal);
    if (error == BALATRO_ERR_OBSERVATION_CAPACITY) {
        result->truncated = 1;
        result->truncation_reason = BALATRO_TRUNCATED_OBSERVATION_CAPACITY;
        return BALATRO_OK;
    }
    return error;
}

int balatro_step_observe_rl(BalatroState *state, const BalatroPolicyAction *action,
                            BalatroStepResult *result, BalatroCompactObservation *observation,
                            BalatroLegalMasks *legal) {
    return step_observe_rl_impl(state, action, result, observation, legal, 0);
}

int balatro_step_observe_rl_trusted(BalatroState *state, const BalatroPolicyAction *action,
                                    BalatroStepResult *result, BalatroCompactObservation *observation,
                                    BalatroLegalMasks *legal) {
    return step_observe_rl_impl(state, action, result, observation, legal, 1);
}

int balatro_step_observe_rl_batch(BalatroState *states, const BalatroPolicyAction *actions, size_t count,
                                  BalatroStepResult *results, BalatroCompactObservation *observations,
                                  BalatroLegalMasks *legal, int8_t *status, int trusted) {
    if ((!states && count) || (!actions && count) || (!results && count) ||
        (!observations && count) || (!legal && count))
        return BALATRO_ERR_ARGUMENT;
    for (size_t i = 0; i < count; ++i) {
        int error = step_observe_rl_impl(&states[i], &actions[i], &results[i],
                                         &observations[i], &legal[i], trusted != 0);
        if (status)
            status[i] = (int8_t)error;
        else if (error)
            return error;
    }
    return BALATRO_OK;
}
