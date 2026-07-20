#include "internal.h"
#include "content.h"

#include <math.h>
#include <string.h>

uint8_t balatro_card_set(const BalatroCard *card) {
    if (card->center_id >= BALATRO_CENTER_COUNT) return 0;
    return balatro_centers[card->center_id].set;
}

static const uint16_t planet_centers[BALATRO_HAND_COUNT] = {
    BALATRO_CENTER_C_ERIS,  BALATRO_CENTER_C_CERES,  BALATRO_CENTER_C_PLANET_X, BALATRO_CENTER_C_NEPTUNE,
    BALATRO_CENTER_C_MARS,  BALATRO_CENTER_C_EARTH,  BALATRO_CENTER_C_JUPITER,  BALATRO_CENTER_C_SATURN,
    BALATRO_CENTER_C_VENUS, BALATRO_CENTER_C_URANUS, BALATRO_CENTER_C_MERCURY,  BALATRO_CENTER_C_PLUTO,
};

uint16_t balatro_planet_center(uint8_t hand) {
    return planet_centers[hand];
}

BalatroHandType balatro_planet_hand(uint16_t center_id) {
    for (uint8_t hand = 0; hand < BALATRO_HAND_COUNT; ++hand)
        if (planet_centers[hand] == center_id) return (BalatroHandType)hand;
    return BALATRO_HAND_COUNT;
}

int balatro_center_used(const BalatroState *state, uint16_t id) {
    return (state->used_centers[id / 8] >> (id % 8)) & 1u;
}

void balatro_mark_center_used(BalatroState *state, uint16_t id) {
    state->used_centers[id / 8] |= (uint8_t)(1u << (id % 8));
}

void balatro_unmark_center_used(BalatroState *state, uint16_t id) {
    state->used_centers[id / 8] &= (uint8_t)~(1u << (id % 8));
}

static int showman_active(const BalatroState *state) {
    for (uint8_t i = 0; i < state->joker_count; ++i)
        if (!(state->jokers[i].flags & 1u) && state->jokers[i].center_id == BALATRO_CENTER_J_RING_MASTER) return 1;
    return 0;
}

static int center_used_for_pool(const BalatroState *state, uint16_t id) {
    if (balatro_center_used(state, id)) return 1;
    /* Card:set_ability updates G.GAME.used_jokers as soon as a Card object is
       constructed.  Therefore unsold shop inventory and the cards already
       constructed for an open pack suppress duplicates too. */
    for (uint8_t i = 0; i < state->shop_main_count; ++i)
        if (state->shop_main[i].center_id == id) return 1;
    for (uint8_t i = 0; i < state->shop_voucher_count; ++i)
        if (state->shop_vouchers[i].center_id == id) return 1;
    for (uint8_t i = 0; i < state->shop_booster_count; ++i)
        if (state->shop_boosters[i].center_id == id) return 1;
    for (uint8_t i = 0; i < state->pack_count; ++i)
        if (state->pack_cards[i].center_id == id) return 1;
    return 0;
}

static int playing_cards_have_enhancement(const BalatroState *state, uint8_t enhancement) {
    for (uint16_t i = 0; i < state->deck_count; ++i)
        if (state->deck[i].enhancement == enhancement) return 1;
    for (uint8_t i = 0; i < state->hand_count; ++i)
        if (state->hand[i].enhancement == enhancement) return 1;
    for (uint16_t i = 0; i < state->discard_count; ++i)
        if (state->discard[i].enhancement == enhancement) return 1;
    return 0;
}

static int center_available(const BalatroState *state, uint16_t id) {
    if (id == BALATRO_CENTER_J_GROS_MICHEL) return !state->gros_michel_extinct;
    if (id == BALATRO_CENTER_J_CAVENDISH) return state->gros_michel_extinct;
    uint8_t gate = id == BALATRO_CENTER_J_STEEL_JOKER ? BALATRO_ENHANCEMENT_STEEL
                   : id == BALATRO_CENTER_J_STONE     ? BALATRO_ENHANCEMENT_STONE
                   : id == BALATRO_CENTER_J_LUCKY_CAT ? BALATRO_ENHANCEMENT_LUCKY
                   : id == BALATRO_CENTER_J_TICKET    ? BALATRO_ENHANCEMENT_GOLD
                   : id == BALATRO_CENTER_J_GLASS     ? BALATRO_ENHANCEMENT_GLASS
                                                      : 0;
    if (gate) return playing_cards_have_enhancement(state, gate);
    return balatro_centers[id].base_available;
}

static uint16_t pick_pool(BalatroState *state, uint8_t set, uint8_t rarity, const char *key) {
    size_t count = 0;
    const uint16_t *pool = set == SET_JOKER ? balatro_joker_pool(rarity, &count) : balatro_center_pool(set, &count);
    if (!pool || !count) return BALATRO_CENTER_NONE;
    int ring_master = showman_active(state);
    for (unsigned attempt = 0; attempt < 21; ++attempt) {
        char stream[64];
        BalatroKeyBuilder builder;
        balatro_key_begin(&builder, stream, sizeof(stream));
        balatro_key_append(&builder, key);
        if (attempt) {
            balatro_key_append(&builder, "_resample");
            balatro_key_append_u64(&builder, attempt + 1);
        }
        size_t index = (size_t)floor(balatro_pseudorandom(state, stream) * count);
        uint16_t id = pool[index];
        int available = center_available(state, id);
        /* Planet X, Ceres, and Eris are softlocked until their hidden hand
           type has been played. get_current_pool keeps their UNAVAILABLE
           entries in place, so a hit must use the normal resample stream rather than compacting the pool. */
        if (set == SET_PLANET) {
            uint8_t hand = id == BALATRO_CENTER_C_PLANET_X ? BALATRO_FIVE_OF_A_KIND
                           : id == BALATRO_CENTER_C_CERES  ? BALATRO_FLUSH_HOUSE
                           : id == BALATRO_CENTER_C_ERIS   ? BALATRO_FLUSH_FIVE
                                                           : BALATRO_HAND_COUNT;
            if (hand < BALATRO_HAND_COUNT && !state->hand_plays[hand]) available = 0;
        }
        if ((!center_used_for_pool(state, id) || ring_master) && available) return id;
    }
    if (set == SET_JOKER) {
        for (size_t i = 0; i < count; ++i) {
            uint16_t id = pool[i];
            int available = center_available(state, id);
            if ((!center_used_for_pool(state, id) || ring_master) && available) return id;
        }
        return BALATRO_CENTER_J_JOKER;
    }
    return pool[0];
}

static uint8_t choose_shop_set(BalatroState *state) {
    char stream[32];
    balatro_key_with_u64(stream, sizeof(stream), "cdt", state->ante);
    double total = state->joker_rate + state->tarot_rate + state->planet_rate + state->spectral_rate + state->playing_card_rate;
    double poll = balatro_pseudorandom(state, stream) * total;
    uint8_t playing_set = SET_DEFAULT;
    /* The Illusion expression is evaluated while constructing the
       ordered shop-rate table, before it knows which category cdt selected.
       It therefore consumes this roll for every generated shop card. */
    if (balatro_center_used(state, BALATRO_CENTER_V_ILLUSION) && balatro_pseudorandom(state, "illusion") > 0.6)
        playing_set = SET_ENHANCED;
    if (poll < state->joker_rate) return SET_JOKER;
    poll -= state->joker_rate;
    if (poll < state->tarot_rate) return SET_TAROT;
    poll -= state->tarot_rate;
    if (poll < state->planet_rate) return SET_PLANET;
    poll -= state->planet_rate;
    if (poll < state->playing_card_rate) return playing_set;
    return SET_SPECTRAL;
}

static uint16_t create_pool_center(BalatroState *state, uint8_t set, const char *append, int pack_area) {
    char stream[64];
    if (set == SET_JOKER) {
        if (!pack_area && state->tag_force_rarity) {
            uint8_t rarity = state->tag_force_rarity;
            if (state->tag_force_rarity_count > 1)
                state->tag_force_rarity_count--;
            else {
                state->tag_force_rarity = 0;
                state->tag_force_rarity_count = 0;
            }
            BalatroKeyBuilder key;
            balatro_key_begin(&key, stream, sizeof(stream));
            balatro_key_append(&key, "Joker");
            balatro_key_append_u64(&key, rarity);
            balatro_key_append(&key, append);
            balatro_key_append_u64(&key, state->ante);
            return pick_pool(state, set, rarity, stream);
        }
        BalatroKeyBuilder key;
        balatro_key_begin(&key, stream, sizeof(stream));
        balatro_key_append(&key, "rarity");
        balatro_key_append_u64(&key, state->ante);
        balatro_key_append(&key, append);
        double roll = balatro_pseudorandom(state, stream);
        uint8_t rarity = roll > 0.95 ? 3 : roll > 0.7 ? 2 : 1;
        balatro_key_begin(&key, stream, sizeof(stream));
        balatro_key_append(&key, "Joker");
        balatro_key_append_u64(&key, rarity);
        balatro_key_append(&key, append);
        balatro_key_append_u64(&key, state->ante);
        return pick_pool(state, set, rarity, stream);
    }
    const char *name = set == SET_TAROT ? "Tarot" : set == SET_PLANET ? "Planet" : "Spectral";
    if (set == SET_TAROT && pack_area) {
        balatro_key_with_u64(stream, sizeof(stream), "soul_Tarot", state->ante);
        if ((!center_used_for_pool(state, BALATRO_CENTER_C_SOUL) || showman_active(state)) && balatro_pseudorandom(state, stream) > 0.997)
            return BALATRO_CENTER_C_SOUL;
    }
    if (set == SET_PLANET && pack_area) {
        balatro_key_with_u64(stream, sizeof(stream), "soul_Planet", state->ante);
        if ((!center_used_for_pool(state, BALATRO_CENTER_C_BLACK_HOLE) || showman_active(state)) &&
            balatro_pseudorandom(state, stream) > 0.997)
            return BALATRO_CENTER_C_BLACK_HOLE;
    }
    if (set == SET_SPECTRAL && pack_area) {
        balatro_key_with_u64(stream, sizeof(stream), "soul_Spectral", state->ante);
        if ((!center_used_for_pool(state, BALATRO_CENTER_C_SOUL) || showman_active(state)) && balatro_pseudorandom(state, stream) > 0.997)
            return BALATRO_CENTER_C_SOUL;
        if ((!center_used_for_pool(state, BALATRO_CENTER_C_BLACK_HOLE) || showman_active(state)) &&
            balatro_pseudorandom(state, stream) > 0.997)
            return BALATRO_CENTER_C_BLACK_HOLE;
    }
    BalatroKeyBuilder key;
    balatro_key_begin(&key, stream, sizeof(stream));
    balatro_key_append(&key, name);
    balatro_key_append(&key, append);
    balatro_key_append_u64(&key, state->ante);
    return pick_pool(state, set, 0, stream);
}

void balatro_price_card(const BalatroState *state, BalatroCard *card) {
    int cost = balatro_centers[card->center_id].cost;
    int edition_cost = card->edition == 1 ? 2 : card->edition == 2 ? 3 : card->edition == 3 || card->edition == 4 ? 5 : 0;
    int raw = cost + edition_cost;
    int free_planet = 0;
    if (balatro_card_set(card) == SET_PLANET)
        for (uint8_t i = 0; i < state->joker_count; ++i)
            if (state->jokers[i].center_id == BALATRO_CENTER_J_ASTRONOMER && !(state->jokers[i].flags & 1u)) {
                raw = 0;
                free_planet = 1;
            }
    int discount = state->discount_percent > 100 ? 100 : state->discount_percent;
    int discounted = (int)floor((raw + 0.5) * (100 - discount) / 100.0);
    card->cost = (int16_t)(free_planet ? 0 : (discounted > 0 ? discounted : 1));
    card->sell_cost = (int16_t)(free_planet ? 0 : (card->cost / 2 > 0 ? card->cost / 2 : 1));
}

static void reprice_card_after_discount(const BalatroState *state, BalatroCard *card, int shop_area) {
    int old_base_sell = card->cost == 0 ? 0 : (card->cost / 2 > 0 ? card->cost / 2 : 1);
    /* A zero shop cost represents a coupon rather than extra sell value.
       Once owned, set_cost restores its normal discounted cost. */
    int extra_value = card->cost == 0 ? 0 : card->sell_cost - old_base_sell;
    int couponed = shop_area && card->cost == 0;
    balatro_price_card(state, card);
    if (card->flags & BALATRO_CARD_RENTAL) {
        card->cost = 1;
        card->sell_cost = 1;
    }
    if (extra_value > 0) card->sell_cost += (int16_t)extra_value;
    /* Coupon/edition tags retain a zero shop price. Card:set_cost computes
       the discounted sell value first, then reapplies the shop coupon. */
    if (couponed) card->cost = 0;
}

static void reprice_all_cards(BalatroState *state) {
    /* Voucher:redeem calls set_cost on every live Card immediately after
       changing discount_percent, including owned Jokers/consumables and the
       cards already visible in this shop. */
    for (uint8_t i = 0; i < state->joker_count; ++i) reprice_card_after_discount(state, &state->jokers[i], 0);
    for (uint8_t i = 0; i < state->consumable_count; ++i) reprice_card_after_discount(state, &state->consumables[i], 0);
    for (uint8_t i = 0; i < state->shop_main_count; ++i)
        reprice_card_after_discount(state, &state->shop_main[i], 1);
    for (uint8_t i = 0; i < state->shop_voucher_count; ++i)
        reprice_card_after_discount(state, &state->shop_vouchers[i], 1);
    for (uint8_t i = 0; i < state->shop_booster_count; ++i)
        reprice_card_after_discount(state, &state->shop_boosters[i], 1);
    for (uint8_t i = 0; i < state->pack_count; ++i) reprice_card_after_discount(state, &state->pack_cards[i], 0);
}

BalatroCard balatro_create_pooled_card(BalatroState *state, uint8_t set, const char *append, int pack_area) {
    BalatroCard card = {0};
    uint8_t forced_edition = 0;
    if (set == SET_DEFAULT)
        card.center_id = BALATRO_CENTER_C_BASE;
    else
        card.center_id = create_pool_center(state, set, append, pack_area);
    card.sort_id = ++state->next_sort_id;
    if (set == SET_DEFAULT || set == SET_ENHANCED) {
        char stream[32];
        BalatroKeyBuilder key;
        balatro_key_begin(&key, stream, sizeof(stream));
        balatro_key_append(&key, "front");
        balatro_key_append(&key, append);
        balatro_key_append_u64(&key, state->ante);
        size_t front = (size_t)floor(balatro_pseudorandom(state, stream) * 52.0);
        BalatroPlayingCardDefinition definition = balatro_playing_card((uint8_t)front);
        card.suit = definition.suit;
        card.rank = definition.rank;
        switch (card.center_id) {
        case BALATRO_CENTER_M_BONUS:
            card.enhancement = BALATRO_ENHANCEMENT_BONUS;
            break;
        case BALATRO_CENTER_M_MULT:
            card.enhancement = BALATRO_ENHANCEMENT_MULT;
            break;
        case BALATRO_CENTER_M_WILD:
            card.enhancement = BALATRO_ENHANCEMENT_WILD;
            break;
        case BALATRO_CENTER_M_GLASS:
            card.enhancement = BALATRO_ENHANCEMENT_GLASS;
            break;
        case BALATRO_CENTER_M_STEEL:
            card.enhancement = BALATRO_ENHANCEMENT_STEEL;
            break;
        case BALATRO_CENTER_M_STONE:
            card.enhancement = BALATRO_ENHANCEMENT_STONE;
            break;
        case BALATRO_CENTER_M_GOLD:
            card.enhancement = BALATRO_ENHANCEMENT_GOLD;
            break;
        case BALATRO_CENTER_M_LUCKY:
            card.enhancement = BALATRO_ENHANCEMENT_LUCKY;
            break;
        }
    }
    if (set == SET_JOKER) {
        /* Card initialization applies abilities before shop stickers/editions and
           before ownership. Stateful construction RNG happens even if this
           visible card is never purchased. */
        balatro_initialize_joker_card(state, &card);
        char stream[32];
        balatro_key_with_u64(stream, sizeof(stream), pack_area ? "packetper" : "etperpoll", state->ante);
        double ep = balatro_pseudorandom(state, stream);
        if (state->config.stake >= 4 && ep > 0.7)
            card.flags |= 1u << 1;
        else if (state->config.stake >= 7 && ep > 0.4)
            card.flags |= 1u << 2;
        balatro_key_with_u64(stream, sizeof(stream), pack_area ? "packssjr" : "ssjr", state->ante);
        if (state->config.stake >= 8 && balatro_pseudorandom(state, stream) > 0.7) card.flags |= 1u << 3;
        BalatroKeyBuilder key;
        balatro_key_begin(&key, stream, sizeof(stream));
        balatro_key_append(&key, "edi");
        balatro_key_append(&key, append);
        balatro_key_append_u64(&key, state->ante);
        double edition = balatro_pseudorandom(state, stream);
        if (edition > 0.997)
            card.edition = 4;
        else if (edition > 1.0 - 0.006 * state->edition_rate)
            card.edition = 3;
        else if (edition > 1.0 - 0.02 * state->edition_rate)
            card.edition = 2;
        else if (edition > 1.0 - 0.04 * state->edition_rate)
            card.edition = 1;
        if (!pack_area && state->tag_force_edition && !card.edition) {
            forced_edition = state->tag_force_edition;
            /* store_joker_modify applies only to an editionless Joker.  If
               this poll produced an already-editioned Joker, the tag
               remains available for a later eligible shop card. */
            card.edition = forced_edition;
            if (state->tag_force_edition_count > 1)
                state->tag_force_edition_count--;
            else {
                state->tag_force_edition = 0;
                state->tag_force_edition_count = 0;
            }
        }
        if (card.flags & (1u << 2)) card.state[3] = 5;
    }
    balatro_price_card(state, &card);
    if (forced_edition) card.cost = 0;
    return card;
}

static BalatroCard create_shop_card(BalatroState *state) {
    /* Rare/Uncommon Tag's store_joker_create hook replaces the first shop
       card with a guaranteed Joker, rather than merely biasing the normal
       shop-set poll. The special card is couponed by the tag. */
    if (state->tag_force_rarity) {
        uint8_t rarity = state->tag_force_rarity;
        const char *append = rarity == 3 ? "rta" : "uta";
        BalatroCard card = balatro_create_pooled_card(state, SET_JOKER, append, 0);
        card.cost = 0;
        return card;
    }
    uint8_t set = choose_shop_set(state);
    BalatroCard card = balatro_create_pooled_card(state, set, "sho", 0);
    if ((set == SET_DEFAULT || set == SET_ENHANCED) && balatro_center_used(state, BALATRO_CENTER_V_ILLUSION) &&
        balatro_pseudorandom(state, "illusion") > 0.8) {
        double edition = balatro_pseudorandom(state, "illusion");
        card.edition = edition > 0.85 ? BALATRO_EDITION_POLYCHROME
                       : edition > 0.5 ? BALATRO_EDITION_HOLO
                                       : BALATRO_EDITION_FOIL;
        balatro_price_card(state, &card);
    }
    return card;
}

static BalatroCard center_card(BalatroState *state, uint16_t center_id) {
    BalatroCard card = {0};
    card.center_id = center_id;
    card.sort_id = ++state->next_sort_id;
    balatro_price_card(state, &card);
    return card;
}

static uint16_t pick_voucher_stream(BalatroState *state, const char *key) {
    size_t count = 0;
    const uint16_t *pool = balatro_center_pool(SET_VOUCHER, &count);
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        char stream[64];
        BalatroKeyBuilder builder;
        balatro_key_begin(&builder, stream, sizeof(stream));
        balatro_key_append(&builder, key);
        if (attempt) {
            balatro_key_append(&builder, "_resample");
            balatro_key_append_u64(&builder, attempt + 1);
        }
        size_t index = (size_t)floor(balatro_pseudorandom(state, stream) * count);
        uint16_t id = pool[index];
        uint16_t requirement = balatro_centers[id].requires;
        if (!balatro_center_used(state, id) && (!requirement || balatro_center_used(state, requirement))) return id;
    }
    return BALATRO_CENTER_V_BLANK;
}

uint16_t balatro_pick_voucher(BalatroState *state) {
    char key[32];
    balatro_key_with_u64(key, sizeof(key), "Voucher", state->ante);
    return pick_voucher_stream(state, key);
}

uint16_t balatro_pick_voucher_from_tag(BalatroState *state) {
    return pick_voucher_stream(state, "Voucher_fromtag");
}

static uint16_t pick_booster(BalatroState *state, int first) {
    if (first) return BALATRO_CENTER_P_BUFFOON_NORMAL_1;
    size_t count = 0;
    const uint16_t *pool = balatro_center_pool(SET_BOOSTER, &count);
    double total = 0.0;
    for (size_t i = 0; i < count; ++i) total += balatro_centers[pool[i]].weight;
    char stream[32];
    balatro_key_with_u64(stream, sizeof(stream), "shop_pack", state->ante);
    double poll = balatro_pseudorandom(state, stream) * total;
    double cumulative = 0.0;
    for (size_t i = 0; i < count; ++i) {
        cumulative += balatro_centers[pool[i]].weight;
        if (poll <= cumulative) return pool[i];
    }
    return pool[count - 1];
}

void balatro_populate_shop(BalatroState *state) {
    state->shop_main_count = 0;
    state->shop_voucher_count = 0;
    state->shop_booster_count = 0;
    state->tag_d_six_active = 0;
    if (state->tag_d_six_pending) {
        /* D6 Tag fires at shop_start: the first reroll costs $0, then
           rerolls use the temporary base cost of $0 plus their increment. */
        state->tag_d_six_pending = 0;
        state->tag_d_six_active = 1;
        /* D6 changes the temporary base reroll cost to zero; unlike a
           Director's Cut/Retcon voucher it does not add a free-reroll count. */
        state->free_rerolls = 0;
        state->reroll_increase = 0;
        state->reroll_cost = 0;
    }
    int coupon = state->tag_coupon_pending != 0;
    state->tag_coupon_pending = 0;
    for (uint8_t i = 0; i < state->shop_joker_max; ++i) {
        BalatroCard card = create_shop_card(state);
        if (state->shop_main_count < BALATRO_OBS_MAX_SHOP_MAIN)
            state->shop_main[state->shop_main_count++] = card;
    }
    /* current_round.voucher persists across shops until it is redeemed or a
       Boss is defeated.  Do not draw a replacement merely for opening the next shop. */
    if (state->next_voucher_id && state->shop_voucher_count < BALATRO_OBS_MAX_SHOP_VOUCHERS)
        state->shop_vouchers[state->shop_voucher_count++] = center_card(state, state->next_voucher_id);
    while (state->tag_voucher_pending && state->shop_voucher_count < BALATRO_OBS_MAX_SHOP_VOUCHERS) {
        BalatroCard free_voucher = center_card(state, balatro_pick_voucher_from_tag(state));
        free_voucher.cost = 0;
        free_voucher.sell_cost = 0;
        state->shop_vouchers[state->shop_voucher_count++] = free_voucher;
        state->tag_voucher_pending--;
    }
    state->shop_boosters[state->shop_booster_count++] = center_card(state, pick_booster(state, !state->first_shop_buffoon));
    state->first_shop_buffoon = 1;
    state->shop_boosters[state->shop_booster_count++] = center_card(state, pick_booster(state, 0));
    /* Coupon Tag's shop_final_pass visits every card in G.shop_jokers
       (Jokers, consumables, and shop playing cards) and G.shop_booster.
       The separate voucher area is the only inventory it leaves priced. */
    if (coupon) {
        for (uint8_t i = 0; i < state->shop_main_count; ++i) state->shop_main[i].cost = 0;
        for (uint8_t i = 0; i < state->shop_booster_count; ++i) state->shop_boosters[i].cost = 0;
    }
}

int balatro_can_afford(const BalatroState *state, int32_t cost) {
    int32_t bankrupt_at = 0;
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (state->jokers[j].center_id == BALATRO_CENTER_J_CREDIT_CARD && !(state->jokers[j].flags & BALATRO_CARD_DEBUFFED))
            bankrupt_at -= 20;
    return (int64_t)state->dollars - cost >= bankrupt_at;
}

int balatro_buy_shop_card(BalatroState *state, uint8_t index) {
    if (!state || index >= state->shop_main_count) return BALATRO_ERR_ACTION;
    BalatroCard card = state->shop_main[index];
    uint8_t set = balatro_card_set(&card);
    if (!balatro_can_afford(state, card.cost)) return BALATRO_ERR_ACTION;
    if (set == SET_JOKER && (state->joker_count >= BALATRO_MAX_JOKERS || (state->joker_count >= state->joker_slots && card.edition != 4)))
        return BALATRO_ERR_CAPACITY;
    if ((set == SET_TAROT || set == SET_PLANET || set == SET_SPECTRAL) &&
        (state->consumable_count >= BALATRO_MAX_CONSUMABLES || (state->consumable_count >= state->consumable_slots && card.edition != 4)))
        return BALATRO_ERR_CAPACITY;
    if ((set == SET_DEFAULT || set == SET_ENHANCED) &&
        (state->deck_count >= BALATRO_MAX_DECK || !balatro_can_add_playing_cards(state, 1))) return BALATRO_ERR_CAPACITY;
    state->dollars -= card.cost;
    if (set == SET_JOKER) {
        state->jokers[state->joker_count++] = card;
        balatro_mark_center_used(state, card.center_id);
        balatro_joker_added(state, &state->jokers[state->joker_count - 1]);
    } else if (set == SET_TAROT || set == SET_PLANET || set == SET_SPECTRAL) {
        state->consumables[state->consumable_count++] = card;
        balatro_consumable_added(state, &card);
    } else if (set == SET_DEFAULT || set == SET_ENHANCED) {
        /* CardArea:emplace inserts into a deck at index one. */
        memmove(&state->deck[1], &state->deck[0], state->deck_count * sizeof(BalatroCard));
        state->deck[0] = card;
        state->deck_count++;
        balatro_playing_card_added(state, 1);
    } else
        return BALATRO_ERR_ACTION;
    memmove(&state->shop_main[index], &state->shop_main[index + 1],
            (state->shop_main_count - index - 1) * sizeof(BalatroCard));
    state->shop_main_count--;
    return BALATRO_OK;
}

int balatro_sell_joker(BalatroState *state, uint8_t index) {
    if (!state || index >= state->joker_count || (state->jokers[index].flags & (1u << 1))) return BALATRO_ERR_ACTION;
    BalatroCard card = state->jokers[index];
    state->dollars += card.sell_cost;
    balatro_joker_removed(state, &card);
    if (!(card.flags & 1u) && card.center_id == BALATRO_CENTER_J_DIET_COLA) state->double_tag = 1;
    if (!(card.flags & 1u) && card.center_id == BALATRO_CENTER_J_INVISIBLE && card.state[0] >= 2 && state->joker_count > 0 &&
        state->joker_count - 1 < state->joker_slots) {
        uint8_t eligible[BALATRO_MAX_JOKERS], eligible_count = 0;
        for (uint8_t j = 0; j < state->joker_count; ++j)
            if (j != index) eligible[eligible_count++] = j;
        if (eligible_count) {
            for (uint8_t j = 1; j < eligible_count; ++j) {
                uint8_t value = eligible[j], k = j;
                while (k && state->jokers[eligible[k - 1]].sort_id > state->jokers[value].sort_id) {
                    eligible[k] = eligible[k - 1];
                    --k;
                }
                eligible[k] = value;
            }
            size_t pick = (size_t)floor(balatro_pseudorandom(state, "invisible") * eligible_count);
            if (pick >= eligible_count) pick = eligible_count - 1;
            BalatroCard copy = state->jokers[eligible[pick]];
            copy.sort_id = ++state->next_sort_id;
            if (copy.center_id == BALATRO_CENTER_J_INVISIBLE) copy.state[0] = 0;
            state->jokers[state->joker_count++] = copy;
            balatro_joker_added(state, &state->jokers[state->joker_count - 1]);
        }
    }
    /* Selling any Joker disables Verdant Leaf while it is the current blind.
       Disabling the blind immediately recalculates every card's
       debuff, so the remaining hand scores normally on the very next play.
       Luchador reaches the same Blind:disable path for any active Boss. */
    int luchador_disables =
        !(card.flags & BALATRO_CARD_DEBUFFED) && card.center_id == BALATRO_CENTER_J_LUCHADOR && state->blind_on_deck == 2;
    if (!state->blind_disabled && (luchador_disables || state->blind_id == BALATRO_BLIND_BL_FINAL_LEAF)) {
        state->blind_disabled = 1;
        balatro_clear_card_debuffs(state);
    }
    for (uint8_t j = 0; j < state->joker_count; ++j)
        if (j != index && !(state->jokers[j].flags & 1u) && state->jokers[j].center_id == BALATRO_CENTER_J_CAMPFIRE)
            state->jokers[j].state[0] = (state->jokers[j].state[0] > 100 ? state->jokers[j].state[0] : 100) + 25;
    memmove(&state->jokers[index], &state->jokers[index + 1], (state->joker_count - index - 1) * sizeof(BalatroCard));
    state->joker_count--;
    balatro_refresh_joker_cache(state);
    return BALATRO_OK;
}

int balatro_reroll_shop(BalatroState *state) {
    if (!state || (state->free_rerolls == 0 && !balatro_can_afford(state, state->reroll_cost))) return BALATRO_ERR_ACTION;
    int free = state->free_rerolls != 0;
    if (free) {
        state->free_rerolls--;
    } else
        state->dollars -= state->reroll_cost;
    /* calculate_reroll_cost(final_free) does not increment the
       per-round cost after a free reroll. */
    if (!free) state->reroll_increase++;
    state->reroll_cost = state->free_rerolls ? 0 : (state->tag_d_six_active ? 0 : state->reroll_base) + state->reroll_increase;
    state->shop_main_count = 0;
    for (uint8_t i = 0; i < state->shop_joker_max; ++i) {
        BalatroCard card = create_shop_card(state);
        if (state->shop_main_count < BALATRO_OBS_MAX_SHOP_MAIN)
            state->shop_main[state->shop_main_count++] = card;
    }
    return BALATRO_OK;
}

static void apply_voucher(BalatroState *state, uint16_t id) {
    const BalatroCenterDefinition *voucher = &balatro_centers[id];
    const float extra = voucher->extra;
    switch (voucher->voucher_effect) {
    case VOUCHER_SHOP_SIZE: {
        state->shop_joker_max++;
        /* change_shop_size(1) immediately fills every empty G.shop_jokers
           slot.  This can create more than one card when an earlier shop
           purchase left a hole before Overstock was redeemed. */
        while (state->shop_main_count < state->shop_joker_max &&
               state->shop_main_count < BALATRO_OBS_MAX_SHOP_MAIN) {
            BalatroCard card = create_shop_card(state);
            state->shop_main[state->shop_main_count++] = card;
        }
        break;
    }
    case VOUCHER_TAROT_RATE:
        state->tarot_rate = 4.0f * extra;
        break;
    case VOUCHER_PLANET_RATE:
        state->planet_rate = 4.0f * extra;
        break;
    case VOUCHER_EDITION_RATE:
        state->edition_rate = extra;
        break;
    case VOUCHER_PLAYING_RATE:
        state->playing_card_rate = extra;
        break;
    case VOUCHER_CONSUMABLE_SLOT:
        state->consumable_slots++;
        break;
    case VOUCHER_DISCOUNT:
        state->discount_percent = (uint8_t)extra;
        reprice_all_cards(state);
        break;
    case VOUCHER_REROLL:
        state->reroll_base = state->reroll_base > (uint8_t)extra ? (uint8_t)(state->reroll_base - (uint8_t)extra) : 0;
        state->reroll_cost -= (int32_t)extra;
        if (state->reroll_cost < 0) state->reroll_cost = 0;
        break;
    case VOUCHER_INTEREST:
        state->interest_cap = (int16_t)extra;
        break;
    case VOUCHER_HANDS:
        state->hands_per_round += (uint8_t)extra;
        break;
    case VOUCHER_HAND_SIZE:
        state->hand_size++;
        state->base_hand_size++;
        break;
    case VOUCHER_DISCARDS:
        state->discards_per_round += (uint8_t)extra;
        break;
    case VOUCHER_JOKER_SLOT:
        state->joker_slots++;
        break;
    case VOUCHER_ANTE_HANDS:
        if (state->ante > 1) state->ante--;
        if (state->hands_per_round > (uint8_t)extra) state->hands_per_round -= (uint8_t)extra;
        break;
    case VOUCHER_ANTE_DISCARDS:
        if (state->ante > 1) state->ante--;
        if (state->discards_per_round > (uint8_t)extra) state->discards_per_round -= (uint8_t)extra;
        break;
    default:
        break;
    }
}

int balatro_redeem_voucher(BalatroState *state, uint8_t index) {
    if (!state || index >= state->shop_voucher_count) return BALATRO_ERR_ACTION;
    BalatroCard card = state->shop_vouchers[index];
    if (!balatro_can_afford(state, card.cost)) return BALATRO_ERR_ACTION;
    state->dollars -= card.cost;
    if (state->next_voucher_id == card.center_id) state->next_voucher_id = 0;
    balatro_mark_center_used(state, card.center_id);
    apply_voucher(state, card.center_id);
    memmove(&state->shop_vouchers[index], &state->shop_vouchers[index + 1],
            (state->shop_voucher_count - index - 1) * sizeof(BalatroCard));
    state->shop_voucher_count--;
    return BALATRO_OK;
}
