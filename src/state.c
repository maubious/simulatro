#include "internal.h"

#include <string.h>

int balatro_state_layout_valid(const BalatroState *state) {
    return state && state->rng_count <= BALATRO_MAX_RNG_STREAMS && state->deck_count <= BALATRO_MAX_DECK &&
           state->hand_count <= BALATRO_MAX_HAND && state->discard_count <= BALATRO_MAX_DECK && state->joker_count <= BALATRO_MAX_JOKERS &&
           state->consumable_count <= BALATRO_MAX_CONSUMABLES &&
           state->shop_main_count <= BALATRO_OBS_MAX_SHOP_MAIN &&
           state->shop_voucher_count <= BALATRO_OBS_MAX_SHOP_VOUCHERS &&
           state->shop_booster_count <= BALATRO_OBS_MAX_SHOP_BOOSTERS &&
           balatro_playing_card_count(state) <= BALATRO_OBS_MAX_PLAYING_CARDS &&
           state->pack_count <= BALATRO_MAX_PACK_CARDS && state->phase <= BALATRO_PHASE_GAME_OVER &&
           memchr(state->seed, 0, sizeof(state->seed));
}

static int joker_cache_bit(uint16_t center_id) {
    switch (center_id) {
    case BALATRO_CENTER_J_FOUR_FINGERS: return 0;
    case BALATRO_CENTER_J_SHORTCUT: return 1;
    case BALATRO_CENTER_J_SMEARED: return 2;
    case BALATRO_CENTER_J_PAREIDOLIA: return 3;
    case BALATRO_CENTER_J_SPLASH: return 4;
    case BALATRO_CENTER_J_OOPS: return 5;
    case BALATRO_CENTER_J_DNA: return 6;
    case BALATRO_CENTER_J_RING_MASTER: return 7;
    case BALATRO_CENTER_J_CERTIFICATE: return 8;
    case BALATRO_CENTER_J_SIXTH_SENSE: return 9;
    case BALATRO_CENTER_J_SEANCE: return 10;
    case BALATRO_CENTER_J_SUPERPOSITION: return 11;
    case BALATRO_CENTER_J_VAGABOND: return 12;
    case BALATRO_CENTER_J_PERKEO: return 13;
    default: return -1;
    }
}

void balatro_refresh_joker_cache(BalatroState *state) {
    state->joker_flags = 0;
    state->joker_active_flags = 0;
    for (uint8_t i = 0; i < state->joker_count; ++i)
        if (joker_cache_bit(state->jokers[i].center_id) >= 0) {
            uint64_t flag = UINT64_C(1) << joker_cache_bit(state->jokers[i].center_id);
            state->joker_flags |= flag;
            if (!(state->jokers[i].flags & BALATRO_CARD_DEBUFFED)) state->joker_active_flags |= flag;
        }
}

int balatro_joker_active(const BalatroState *state, uint16_t center_id) {
    int bit = joker_cache_bit(center_id);
    if (bit >= 0) return (state->joker_active_flags & (UINT64_C(1) << bit)) != 0;
    for (uint8_t i = 0; i < state->joker_count; ++i)
        if (!(state->jokers[i].flags & BALATRO_CARD_DEBUFFED) && state->jokers[i].center_id == center_id) return 1;
    return 0;
}


size_t balatro_state_size(void) {
    return sizeof(BalatroState);
}
size_t balatro_observation_size(void) {
    return sizeof(BalatroObservation);
}
size_t balatro_compact_observation_size(void) {
    return sizeof(BalatroCompactObservation);
}
size_t balatro_legal_masks_size(void) {
    return sizeof(BalatroLegalMasks);
}

void balatro_clone_state(BalatroState *destination, const BalatroState *source) {
    if (!destination || !source || destination == source) return;
    uint16_t rng_count = source->rng_count < BALATRO_MAX_RNG_STREAMS ? source->rng_count : BALATRO_MAX_RNG_STREAMS;
    memcpy(destination, source, offsetof(BalatroState, rng));
    memcpy(destination->rng, source->rng, (size_t)rng_count * sizeof(source->rng[0]));
    memcpy(&destination->rng_count, &source->rng_count, offsetof(BalatroState, deck) - offsetof(BalatroState, rng_count));
    memcpy(destination->deck, source->deck, (size_t)source->deck_count * sizeof(source->deck[0]));
    memcpy(destination->hand, source->hand, (size_t)source->hand_count * sizeof(source->hand[0]));
    memcpy(destination->discard, source->discard, (size_t)source->discard_count * sizeof(source->discard[0]));
    memcpy(destination->jokers, source->jokers, (size_t)source->joker_count * sizeof(source->jokers[0]));
    memcpy(destination->consumables, source->consumables, (size_t)source->consumable_count * sizeof(source->consumables[0]));
    memcpy(destination->shop_main, source->shop_main,
           (size_t)source->shop_main_count * sizeof(source->shop_main[0]));
    memcpy(destination->shop_vouchers, source->shop_vouchers,
           (size_t)source->shop_voucher_count * sizeof(source->shop_vouchers[0]));
    memcpy(destination->shop_boosters, source->shop_boosters,
           (size_t)source->shop_booster_count * sizeof(source->shop_boosters[0]));
    memcpy(destination->pack_cards, source->pack_cards, (size_t)source->pack_count * sizeof(source->pack_cards[0]));
    memcpy(&destination->deck_count, &source->deck_count, sizeof(*source) - offsetof(BalatroState, deck_count));
}


static uint64_t hash_bytes(uint64_t hash, const void *data, size_t length) {
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t balatro_state_hash(const BalatroState *state) {
    if (!balatro_state_layout_valid(state)) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_bytes(hash, state, offsetof(BalatroState, rng));
    hash = hash_bytes(hash, state->rng, (size_t)state->rng_count * sizeof(state->rng[0]));
    hash = hash_bytes(hash, &state->rng_count, offsetof(BalatroState, deck) - offsetof(BalatroState, rng_count));
    hash = hash_bytes(hash, state->deck, (size_t)state->deck_count * sizeof(state->deck[0]));
    hash = hash_bytes(hash, state->hand, (size_t)state->hand_count * sizeof(state->hand[0]));
    hash = hash_bytes(hash, state->discard, (size_t)state->discard_count * sizeof(state->discard[0]));
    hash = hash_bytes(hash, state->jokers, (size_t)state->joker_count * sizeof(state->jokers[0]));
    hash = hash_bytes(hash, state->consumables, (size_t)state->consumable_count * sizeof(state->consumables[0]));
    hash = hash_bytes(hash, state->shop_main, (size_t)state->shop_main_count * sizeof(state->shop_main[0]));
    hash = hash_bytes(hash, state->shop_vouchers,
                      (size_t)state->shop_voucher_count * sizeof(state->shop_vouchers[0]));
    hash = hash_bytes(hash, state->shop_boosters,
                      (size_t)state->shop_booster_count * sizeof(state->shop_boosters[0]));
    hash = hash_bytes(hash, state->pack_cards, (size_t)state->pack_count * sizeof(state->pack_cards[0]));
    return hash_bytes(hash, &state->deck_count, sizeof(*state) - offsetof(BalatroState, deck_count));
}

typedef struct BalatroSnapshotHeader {
    uint8_t magic[8];
    uint32_t state_size;
    uint64_t state_hash;
} BalatroSnapshotHeader;

size_t balatro_serialize(const BalatroState *state, void *out, size_t capacity) {
    if (!balatro_state_layout_valid(state)) return 0;
    size_t required = sizeof(BalatroSnapshotHeader) + sizeof(*state);
    if (!out) return required;
    if (capacity < required) return 0;
    BalatroState canonical = {0};
    balatro_clone_state(&canonical, state);
    /* Snapshot bytes are deterministic.  In particular, clear the ABI
       padding before populating the fields so serializing the same logical
       state never leaks or compares unequal because of stack contents. */
    BalatroSnapshotHeader header = {0};
    static const uint8_t magic[8] = {'B', 'A', 'L', 'A', 'T', 'R', 'O', '\0'};
    memcpy(header.magic, magic, sizeof(header.magic));
    header.state_size = sizeof(*state);
    header.state_hash = balatro_state_hash(&canonical);
    memcpy(out, &header, sizeof(header));
    memcpy((uint8_t *)out + sizeof(header), &canonical, sizeof(canonical));
    return required;
}

int balatro_deserialize(BalatroState *state, const void *data, size_t length) {
    if (!state || !data) return BALATRO_ERR_ARGUMENT;
    if (length != sizeof(BalatroSnapshotHeader) + sizeof(*state)) return BALATRO_ERR_SNAPSHOT;
    BalatroSnapshotHeader header;
    memcpy(&header, data, sizeof(header));
    static const uint8_t magic[8] = {'B', 'A', 'L', 'A', 'T', 'R', 'O', '\0'};
    if (memcmp(header.magic, magic, sizeof(magic)) || header.state_size != sizeof(*state))
        return BALATRO_ERR_SNAPSHOT;
    BalatroState copy;
    memcpy(&copy, (const uint8_t *)data + sizeof(header), sizeof(copy));
    if (!balatro_state_layout_valid(&copy)) return BALATRO_ERR_SNAPSHOT;
    if (balatro_state_hash(&copy) != header.state_hash) return BALATRO_ERR_SNAPSHOT;
    *state = copy;
    return BALATRO_OK;
}

