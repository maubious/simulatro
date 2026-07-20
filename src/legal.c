#include "internal.h"
#include "content.h"

#include <string.h>

static const BalatroObservedSelection *expanded_selection(const BalatroLegalMasks *masks,
                                                           uint8_t type, uint8_t primary);

static int consumable_target_limit(uint16_t center_id) {
    return center_id < BALATRO_CENTER_COUNT ? balatro_centers[center_id].target_max : 0;
}

static int consumable_target_minimum(uint16_t center_id) {
    return center_id < BALATRO_CENTER_COUNT ? balatro_centers[center_id].target_min : 0;
}

static int consumable_no_target_legal(const BalatroState *state, uint16_t center_id) {
    switch (center_id) {
    case BALATRO_CENTER_C_FAMILIAR:
    case BALATRO_CENTER_C_GRIM:
    case BALATRO_CENTER_C_INCANTATION:
    case BALATRO_CENTER_C_IMMOLATE:
    case BALATRO_CENTER_C_SIGIL:
    case BALATRO_CENTER_C_OUIJA:
        return state->hand_count > 1;
    case BALATRO_CENTER_C_ANKH:
        return state->joker_count > 0 && state->joker_count < state->joker_slots;
    case BALATRO_CENTER_C_WRAITH:
    case BALATRO_CENTER_C_SOUL:
        return state->joker_count < state->joker_slots;
    case BALATRO_CENTER_C_ECTOPLASM:
    case BALATRO_CENTER_C_HEX:
        for (uint8_t i = 0; i < state->joker_count; ++i)
            if (state->jokers[i].edition == 0) return 1;
        return 0;
    case BALATRO_CENTER_C_WHEEL_OF_FORTUNE:
        for (uint8_t i = 0; i < state->joker_count; ++i)
            if (!(state->jokers[i].flags & 1u) && state->jokers[i].edition == 0) return 1;
        return 0;
    case BALATRO_CENTER_C_EMPEROR:
    case BALATRO_CENTER_C_HIGH_PRIESTESS:
        /* Consumable use removes the original card before creating the
           generated cards, so a full area still has one transient slot. */
        return 1;
    case BALATRO_CENTER_C_JUDGEMENT:
        return state->joker_count < state->joker_slots;
    case BALATRO_CENTER_C_FOOL:
        return state->last_tarot_planet != 0 && state->last_tarot_planet != BALATRO_CENTER_C_FOOL;
    default:
        return 1;
    }
}

int balatro_action_is_legal(const BalatroState *state, const BalatroAction *action) {
    if (!state || !action || action->selection_count > BALATRO_MAX_SELECTION) return 0;
    BalatroLegalMasks masks;
    if (balatro_legal_masks(state, &masks) || action->type >= BALATRO_ACTION_TYPE_COUNT ||
        !masks.action_type[action->type] || action->primary >= 64 ||
        !(masks.primary[action->type] & (UINT64_C(1) << action->primary)))
        return 0;
    const BalatroObservedSelection *selection = expanded_selection(&masks, action->type, action->primary);
    if (!selection || !selection->valid) return action->selection_count == 0;
    if (action->selection_count < selection->minimum || action->selection_count > selection->maximum) return 0;
    uint64_t selected = 0;
    for (uint8_t i = 0; i < action->selection_count; ++i) {
        uint8_t index = action->selection[i];
        if (index >= BALATRO_MAX_HAND || (i && action->selection[i - 1] >= index)) return 0;
        selected |= UINT64_C(1) << index;
    }
    return selected && !(selected & ~selection->allowed_hand) &&
           (selected & selection->required_hand) == selection->required_hand;
}

static uint8_t bit_count64(uint64_t value) {
    uint8_t count = 0;
    while (value) {
        value &= value - 1;
        count++;
    }
    return count;
}

static uint32_t choose_count(uint8_t n, uint8_t k) {
    if (k > n) return 0;
    if (k > n - k) k = (uint8_t)(n - k);
    uint32_t result = 1;
    for (uint8_t i = 1; i <= k; ++i) result = result * (uint32_t)(n - k + i) / i;
    return result;
}

static uint32_t selection_family_size(const BalatroSelectionFamily *family, uint8_t selected_count) {
    uint8_t allowed = bit_count64(family->allowed_mask);
    uint8_t required = bit_count64(family->required_mask);
    if ((family->required_mask & family->allowed_mask) != family->required_mask || selected_count < required || selected_count > allowed)
        return 0;
    return choose_count((uint8_t)(allowed - required), (uint8_t)(selected_count - required));
}

uint32_t balatro_legal_group_count(const BalatroLegalGroup *group) {
    if (!group) return 0;
    if (group->kind == BALATRO_LEGAL_DISCRETE) return 1;
    if (group->kind != BALATRO_LEGAL_SELECTION) return 0;
    uint32_t count = 0;
    for (uint8_t selected = group->selection.minimum; selected <= group->selection.maximum; ++selected)
        count += selection_family_size(&group->selection, selected);
    return count;
}

int balatro_legal_group_action(const BalatroLegalGroup *group, uint32_t ordinal, BalatroAction *out) {
    if (!group || !out) return BALATRO_ERR_ARGUMENT;
    if (group->kind == BALATRO_LEGAL_DISCRETE) {
        if (ordinal) return BALATRO_ERR_ACTION;
        *out = group->action;
        return BALATRO_OK;
    }
    if (group->kind != BALATRO_LEGAL_SELECTION) return BALATRO_ERR_ACTION;
    const BalatroSelectionFamily *family = &group->selection;
    uint8_t selected_count = family->minimum;
    for (; selected_count <= family->maximum; ++selected_count) {
        uint32_t count = selection_family_size(family, selected_count);
        if (ordinal < count) break;
        ordinal -= count;
    }
    if (selected_count > family->maximum) return BALATRO_ERR_ACTION;
    BalatroAction action = {
        .type = family->type,
        .primary = family->primary,
        .selection_count = selected_count,
    };
    uint64_t chosen = 0;
    int previous = -1;
    for (uint8_t position = 0; position < selected_count; ++position) {
        uint8_t remaining = (uint8_t)(selected_count - position - 1);
        int found = 0;
        for (int candidate = previous + 1; candidate < BALATRO_MAX_HAND; ++candidate) {
            uint64_t bit = UINT64_C(1) << candidate;
            if (!(family->allowed_mask & bit)) continue;
            uint64_t required_left = family->required_mask & ~(chosen | bit);
            uint64_t lower = candidate == 63 ? UINT64_MAX : (UINT64_C(1) << (candidate + 1)) - 1;
            if (required_left & lower) continue;
            uint64_t allowed_after = family->allowed_mask & ~lower;
            uint8_t required_after = bit_count64(required_left);
            uint8_t optional_after = (uint8_t)(bit_count64(allowed_after) - required_after);
            if (required_after > remaining) continue;
            uint32_t completions = choose_count(optional_after, (uint8_t)(remaining - required_after));
            if (ordinal >= completions) {
                ordinal -= completions;
                continue;
            }
            action.selection[position] = (uint8_t)candidate;
            chosen |= bit;
            previous = candidate;
            found = 1;
            break;
        }
        if (!found) return BALATRO_ERR_INVARIANT;
    }
    *out = action;
    return BALATRO_OK;
}

int balatro_legal_group_actions(const BalatroLegalGroup *group, BalatroAction *out, size_t capacity) {
    if (!group || (capacity && !out)) return BALATRO_ERR_ARGUMENT;
    uint32_t count = balatro_legal_group_count(group);
    uint32_t written = count < capacity ? count : (uint32_t)capacity;
    for (uint32_t ordinal = 0; ordinal < written; ++ordinal) {
        int error = balatro_legal_group_action(group, ordinal, &out[ordinal]);
        if (error) return error;
    }
    return (int)count;
}

static uint64_t hand_mask(const BalatroState *state) {
    return state->hand_count == 64 ? UINT64_MAX : (UINT64_C(1) << state->hand_count) - 1;
}

static void masks_add_discrete(BalatroLegalMasks *masks, const BalatroState *state, BalatroAction action);
static void masks_add_selection(BalatroLegalMasks *masks, const BalatroState *state,
                                BalatroSelectionFamily family);

static void legal_add_selection(BalatroLegalMasks *masks, const BalatroState *state,
                                uint8_t type, uint8_t primary, uint8_t minimum,
                                uint8_t maximum, uint64_t allowed, uint64_t required) {
    if (maximum > bit_count64(allowed)) maximum = bit_count64(allowed);
    if (minimum > maximum || bit_count64(required) > maximum || (required & allowed) != required) return;
    masks_add_selection(masks, state, (BalatroSelectionFamily){
        .type = type,
        .primary = primary,
        .minimum = minimum,
        .maximum = maximum,
        .allowed_mask = allowed,
        .required_mask = required,
    });
}

static uint64_t consumable_allowed_mask(const BalatroState *state, uint16_t center_id) {
    uint64_t allowed = hand_mask(state);
    if (center_id == BALATRO_CENTER_C_AURA)
        for (uint8_t i = 0; i < state->hand_count; ++i)
            if (state->hand[i].edition != BALATRO_EDITION_NONE) allowed &= ~(UINT64_C(1) << i);
    return allowed;
}

static void legal_add_consumables(const BalatroState *state, BalatroLegalMasks *masks) {
    for (uint8_t i = 0; i < state->consumable_count && i < BALATRO_MAX_CONSUMABLES; ++i) {
        const BalatroCard *card = &state->consumables[i];
        if (!(card->flags & BALATRO_CARD_ETERNAL))
            masks_add_discrete(masks, state, (BalatroAction){.type = BALATRO_ACTION_SELL_CONSUMABLE, .primary = i});
        int maximum = consumable_target_limit(card->center_id);
        if (!maximum) {
            if (consumable_no_target_legal(state, card->center_id))
                masks_add_discrete(masks, state, (BalatroAction){.type = BALATRO_ACTION_USE_CONSUMABLE, .primary = i});
        } else {
            legal_add_selection(masks, state, BALATRO_ACTION_USE_CONSUMABLE, i,
                                (uint8_t)consumable_target_minimum(card->center_id), (uint8_t)maximum,
                                consumable_allowed_mask(state, card->center_id), 0);
        }
    }
}

static void build_legal(const BalatroState *state, BalatroLegalMasks *masks) {
#define ADD(action_value) masks_add_discrete(masks, state, (action_value))
    if (state->phase == BALATRO_PHASE_BLIND_SELECT) {
        ADD(((BalatroAction){.type = BALATRO_ACTION_SELECT_BLIND}));
        if (state->blind_on_deck < 2) ADD(((BalatroAction){.type = BALATRO_ACTION_SKIP_BLIND}));
        int can_reroll = balatro_center_used(state, BALATRO_CENTER_V_RETCON) ||
                         (balatro_center_used(state, BALATRO_CENTER_V_DIRECTORS_CUT) && !state->boss_rerolled);
        if (can_reroll && balatro_can_afford(state, 10)) ADD(((BalatroAction){.type = BALATRO_ACTION_REROLL_BOSS}));
    } else if (state->phase == BALATRO_PHASE_SELECTING_HAND) {
        uint64_t allowed = hand_mask(state), forced = 0;
        uint16_t discard_space = (uint16_t)(BALATRO_MAX_DECK - state->discard_count);
        uint8_t maximum = discard_space > BALATRO_MAX_SELECTION
            ? BALATRO_MAX_SELECTION : (uint8_t)discard_space;
        for (uint8_t i = 0; i < state->hand_count && i < BALATRO_MAX_HAND; ++i)
            if (state->hand[i].flags & BALATRO_CARD_FORCED) {
                forced = UINT64_C(1) << i;
                break;
        }
        if (state->hands_left) {
            legal_add_selection(masks, state, BALATRO_ACTION_PLAY_HAND, 0, 1, maximum, allowed, forced);
        }
        if (state->discards_left) {
            legal_add_selection(masks, state, BALATRO_ACTION_DISCARD, 0, 1, maximum, allowed, 0);
        }
        for (uint8_t i = 0; i < state->hand_count && i < BALATRO_MAX_HAND; ++i) {
            if (i) ADD(((BalatroAction){.type = BALATRO_ACTION_SWAP_HAND_LEFT, .primary = i}));
            if (i + 1 < state->hand_count) ADD(((BalatroAction){.type = BALATRO_ACTION_SWAP_HAND_RIGHT, .primary = i}));
        }
        ADD(((BalatroAction){.type = BALATRO_ACTION_SORT_HAND_RANK}));
        ADD(((BalatroAction){.type = BALATRO_ACTION_SORT_HAND_SUIT}));
    } else if (state->phase == BALATRO_PHASE_ROUND_EVAL) {
        ADD(((BalatroAction){.type = BALATRO_ACTION_CASH_OUT}));
    } else if (state->phase == BALATRO_PHASE_SHOP) {
        for (uint8_t i = 0; i < state->shop_main_count && i < BALATRO_OBS_MAX_SHOP_MAIN; ++i) {
            const BalatroCard *card = &state->shop_main[i];
            uint8_t set = balatro_card_set(card);
            int affordable = balatro_can_afford(state, card->cost);
            int has_space = set == SET_JOKER ? state->joker_count < BALATRO_MAX_JOKERS &&
                                                   (state->joker_count < state->joker_slots || card->edition == BALATRO_EDITION_NEGATIVE)
                            : (set >= SET_TAROT && set <= SET_SPECTRAL)
                                ? state->consumable_count < BALATRO_MAX_CONSUMABLES &&
                                      (state->consumable_count < state->consumable_slots || card->edition == BALATRO_EDITION_NEGATIVE)
                            : (set == SET_DEFAULT || set == SET_ENHANCED)
                                ? state->deck_count < BALATRO_MAX_DECK && balatro_can_add_playing_cards(state, 1)
                                : 0;
            if ((set == SET_DEFAULT || (set >= SET_ENHANCED && set <= SET_SPECTRAL)) && affordable && has_space)
                ADD(((BalatroAction){.type = BALATRO_ACTION_BUY_CARD, .primary = i}));
            if (set >= SET_TAROT && set <= SET_SPECTRAL && affordable) {
                int maximum = consumable_target_limit(card->center_id);
                if (!maximum) {
                    if (consumable_no_target_legal(state, card->center_id))
                        ADD(((BalatroAction){.type = BALATRO_ACTION_BUY_AND_USE, .primary = i}));
                } else {
                    legal_add_selection(masks, state, BALATRO_ACTION_BUY_AND_USE, i,
                                        (uint8_t)consumable_target_minimum(card->center_id), (uint8_t)maximum,
                                        consumable_allowed_mask(state, card->center_id), 0);
                }
            }
        }
        for (uint8_t i = 0; i < state->shop_voucher_count && i < BALATRO_OBS_MAX_SHOP_VOUCHERS; ++i)
            if (balatro_can_afford(state, state->shop_vouchers[i].cost))
                ADD(((BalatroAction){.type = BALATRO_ACTION_REDEEM_VOUCHER, .primary = i}));
        for (uint8_t i = 0; i < state->shop_booster_count && i < BALATRO_OBS_MAX_SHOP_BOOSTERS; ++i)
            if (balatro_can_afford(state, state->shop_boosters[i].cost))
                ADD(((BalatroAction){.type = BALATRO_ACTION_OPEN_BOOSTER, .primary = i}));
        if (state->free_rerolls || balatro_can_afford(state, state->reroll_cost)) ADD(((BalatroAction){.type = BALATRO_ACTION_REROLL}));
        ADD(((BalatroAction){.type = BALATRO_ACTION_NEXT_ROUND}));
    } else if (state->phase == BALATRO_PHASE_PACK_OPENING) {
        for (uint8_t i = 0; i < state->pack_count && i < BALATRO_MAX_PACK_CARDS; ++i) {
            const BalatroCard *card = &state->pack_cards[i];
            uint8_t set = balatro_card_set(card);
            int maximum = consumable_target_limit(card->center_id);
            if (set >= SET_TAROT && set <= SET_SPECTRAL && maximum) {
                legal_add_selection(masks, state, BALATRO_ACTION_PICK_PACK_CARD, i,
                                    (uint8_t)consumable_target_minimum(card->center_id), (uint8_t)maximum,
                                    consumable_allowed_mask(state, card->center_id), 0);
            } else if (!(set >= SET_TAROT && set <= SET_SPECTRAL) || consumable_no_target_legal(state, card->center_id)) {
                int has_space =
                    set != SET_JOKER || (state->joker_count < BALATRO_MAX_JOKERS &&
                                         (state->joker_count < state->joker_slots || card->edition == BALATRO_EDITION_NEGATIVE));
                if ((set == SET_PLAYING || set == SET_ENHANCED) &&
                    (state->deck_count >= BALATRO_MAX_DECK || !balatro_can_add_playing_cards(state, 1))) has_space = 0;
                if (has_space) ADD(((BalatroAction){.type = BALATRO_ACTION_PICK_PACK_CARD, .primary = i}));
            }
        }
        ADD(((BalatroAction){.type = BALATRO_ACTION_SKIP_PACK}));
    }
    if (state->phase == BALATRO_PHASE_BLIND_SELECT || state->phase == BALATRO_PHASE_SELECTING_HAND || state->phase == BALATRO_PHASE_SHOP ||
        state->phase == BALATRO_PHASE_PACK_OPENING) {
        for (uint8_t i = 0; i < state->joker_count && i < BALATRO_MAX_JOKERS; ++i) {
            if (i) ADD(((BalatroAction){.type = BALATRO_ACTION_SWAP_JOKERS_LEFT, .primary = i}));
            if (i + 1 < state->joker_count) ADD(((BalatroAction){.type = BALATRO_ACTION_SWAP_JOKERS_RIGHT, .primary = i}));
            if (!(state->jokers[i].flags & BALATRO_CARD_ETERNAL)) ADD(((BalatroAction){.type = BALATRO_ACTION_SELL_JOKER, .primary = i}));
        }
    }
    if (state->phase == BALATRO_PHASE_BLIND_SELECT || state->phase == BALATRO_PHASE_SELECTING_HAND ||
        state->phase == BALATRO_PHASE_ROUND_EVAL || state->phase == BALATRO_PHASE_SHOP || state->phase == BALATRO_PHASE_PACK_OPENING) {
        legal_add_consumables(state, masks);
    }
#undef ADD
}

static int action_has_primary(uint8_t type) {
    return type >= BALATRO_ACTION_BUY_CARD && type <= BALATRO_ACTION_SWAP_HAND_RIGHT;
}

static int observed_action_primary(const BalatroState *state, uint8_t type, uint8_t native_primary) {
    (void)state;
    return action_has_primary(type) ? native_primary : 0;
}

static BalatroObservedSelection *mask_selection(BalatroLegalMasks *masks, uint8_t type, uint8_t primary) {
    if (type == BALATRO_ACTION_PLAY_HAND) return &masks->play;
    if (type == BALATRO_ACTION_DISCARD) return &masks->discard;
    if (type == BALATRO_ACTION_USE_CONSUMABLE && primary < BALATRO_OBS_MAX_CONSUMABLES)
        return &masks->consumable[primary];
    if (type == BALATRO_ACTION_BUY_AND_USE && primary < BALATRO_OBS_MAX_SHOP_MAIN)
        return &masks->shop[primary];
    if (type == BALATRO_ACTION_PICK_PACK_CARD && primary < BALATRO_OBS_MAX_PACK_CARDS)
        return &masks->pack[primary];
    return NULL;
}

static void masks_add_discrete(BalatroLegalMasks *masks, const BalatroState *state, BalatroAction action) {
    int primary = observed_action_primary(state, action.type, action.primary);
    if (action.type >= BALATRO_ACTION_TYPE_COUNT || primary < 0 || primary >= 64) return;
    masks->action_type[action.type] = 1;
    masks->primary[action.type] |= UINT64_C(1) << primary;
    if (action.type == BALATRO_ACTION_SWAP_HAND_LEFT && action.primary < BALATRO_OBS_MAX_HAND)
        masks->hand_reorder_destination[action.primary] |= UINT64_C(1) << (action.primary - 1);
    else if (action.type == BALATRO_ACTION_SWAP_HAND_RIGHT && action.primary < BALATRO_OBS_MAX_HAND)
        masks->hand_reorder_destination[action.primary] |= UINT64_C(1) << (action.primary + 1);
    else if (action.type == BALATRO_ACTION_SWAP_JOKERS_LEFT && action.primary < BALATRO_OBS_MAX_JOKERS)
        masks->joker_reorder_destination[action.primary] |= UINT64_C(1) << (action.primary - 1);
    else if (action.type == BALATRO_ACTION_SWAP_JOKERS_RIGHT && action.primary < BALATRO_OBS_MAX_JOKERS)
        masks->joker_reorder_destination[action.primary] |= UINT64_C(1) << (action.primary + 1);
}

static void masks_add_selection(BalatroLegalMasks *masks, const BalatroState *state,
                                BalatroSelectionFamily family) {
    int primary = observed_action_primary(state, family.type, family.primary);
    if (family.type >= BALATRO_ACTION_TYPE_COUNT || primary < 0 || primary >= 64) return;
    masks->action_type[family.type] = 1;
    masks->primary[family.type] |= UINT64_C(1) << primary;
    BalatroObservedSelection *selection = mask_selection(masks, family.type, (uint8_t)primary);
    if (!selection) return;
    *selection = (BalatroObservedSelection){
        .allowed_hand = family.allowed_mask,
        .required_hand = family.required_mask,
        .minimum = family.minimum,
        .maximum = family.maximum,
        .valid = 1,
    };
}

int balatro_legal_masks(const BalatroState *state, BalatroLegalMasks *out) {
    if (!state || !out) return BALATRO_ERR_ARGUMENT;
    *out = (BalatroLegalMasks){0};
    build_legal(state, out);
    return BALATRO_OK;
}

static const BalatroObservedSelection *expanded_selection(const BalatroLegalMasks *masks,
                                                           uint8_t type, uint8_t primary) {
    if (type == BALATRO_ACTION_PLAY_HAND) return &masks->play;
    if (type == BALATRO_ACTION_DISCARD) return &masks->discard;
    if (type == BALATRO_ACTION_USE_CONSUMABLE && primary < BALATRO_OBS_MAX_CONSUMABLES)
        return &masks->consumable[primary];
    if (type == BALATRO_ACTION_BUY_AND_USE && primary < BALATRO_OBS_MAX_SHOP_MAIN)
        return &masks->shop[primary];
    if (type == BALATRO_ACTION_PICK_PACK_CARD && primary < BALATRO_OBS_MAX_PACK_CARDS)
        return &masks->pack[primary];
    return NULL;
}

int balatro_legal_expand(const BalatroLegalMasks *masks, BalatroLegalView *out) {
    if (!masks || !out) return BALATRO_ERR_ARGUMENT;
    *out = (BalatroLegalView){0};
    for (uint8_t type = 0; type < BALATRO_ACTION_TYPE_COUNT; ++type) {
        uint64_t primary_mask = masks->primary[type];
        while (primary_mask) {
            uint8_t primary = 0;
            while (!(primary_mask & (UINT64_C(1) << primary))) primary++;
            primary_mask &= primary_mask - 1;
            if (out->group_count >= BALATRO_MAX_LEGAL_GROUPS) return BALATRO_ERR_CAPACITY;
            const BalatroObservedSelection *selection = expanded_selection(masks, type, primary);
            BalatroLegalGroup *group = &out->groups[out->group_count++];
            if (selection && selection->valid) {
                *group = (BalatroLegalGroup){
                    .kind = BALATRO_LEGAL_SELECTION,
                    .selection = {
                        .type = type,
                        .primary = primary,
                        .minimum = selection->minimum,
                        .maximum = selection->maximum,
                        .allowed_mask = selection->allowed_hand,
                        .required_mask = selection->required_hand,
                    },
                };
                out->action_count += balatro_legal_group_count(group);
            } else {
                *group = (BalatroLegalGroup){
                    .kind = BALATRO_LEGAL_DISCRETE,
                    .action = {.type = type, .primary = primary},
                };
                out->action_count++;
            }
        }
    }
    return BALATRO_OK;
}

static int legal_view_impl(const BalatroState *state, BalatroLegalView *out, int count_actions) {
    (void)count_actions;
    BalatroLegalMasks masks;
    int error = balatro_legal_masks(state, &masks);
    return error ? error : balatro_legal_expand(&masks, out);
}

int balatro_legal_view(const BalatroState *state, BalatroLegalView *out) {
    return legal_view_impl(state, out, 1);
}
