"""Small ctypes binding for differential tests and analysis.

The native Puffer adapter links the C API directly. This module deliberately
does not duplicate game rules.
"""

from __future__ import annotations

import ctypes as C
import os
from pathlib import Path

MAX_DECK = 104
MAX_HAND = 16
MAX_JOKERS = 12
MAX_CONSUMABLES = 8
MAX_SHOP_CARDS = 12
MAX_PACK_CARDS = 8
MAX_SELECTION = 5
MAX_LEGAL_ACTIONS = 20000
MAX_LEGAL_GROUPS = 128
MAX_RNG_STREAMS = 256
HAND_COUNT = 12
ACTION_TYPE_COUNT = 23
NUMBER_FINITE = 0
NUMBER_POSITIVE_INFINITY = 1
NUMBER_NEGATIVE_INFINITY = 2
NUMBER_NAN = 3
OBS_MAX_PLAYING_CARDS = 256
OBS_MAX_PLAYING_VARIANTS = 256
OBS_MAX_HAND = 64
OBS_MAX_JOKERS = 32
OBS_MAX_CONSUMABLES = 32
OBS_MAX_TAGS = 64
OBS_MAX_SHOP_MAIN = 4
OBS_MAX_SHOP_VOUCHERS = 64
OBS_MAX_SHOP_BOOSTERS = 2
OBS_MAX_PACK_CARDS = 5
CENTER_COUNT = 300


class ObservationProfile(C.Structure):
    _fields_ = [
        ("playing_cards", C.c_uint16), ("playing_variants", C.c_uint16),
        ("hand", C.c_uint16), ("jokers", C.c_uint16),
        ("consumables", C.c_uint16), ("tags", C.c_uint16),
        ("shop_vouchers", C.c_uint16),
    ]


class Config(C.Structure):
    _fields_ = [
        ("deck", C.c_uint8), ("stake", C.c_uint8), ("win_ante", C.c_uint8),
        ("shaped_reward", C.c_uint8), ("validation", C.c_uint8), ("reserved", C.c_uint8),
        ("observation", ObservationProfile),
    ]


class Card(C.Structure):
    _fields_ = [
        ("center_id", C.c_uint16), ("sort_id", C.c_uint16),
        ("suit", C.c_uint8), ("rank", C.c_uint8),
        ("enhancement", C.c_uint8), ("edition", C.c_uint8),
        ("seal", C.c_uint8), ("flags", C.c_uint8),
        ("perma_bonus", C.c_int16), ("cost", C.c_int16), ("sell_cost", C.c_int16),
        ("state", C.c_int32 * 4),
    ]


class Action(C.Structure):
    _fields_ = [
        ("type", C.c_uint8), ("primary", C.c_uint8),
        ("selection_count", C.c_uint8), ("selection", C.c_uint8 * MAX_SELECTION),
    ]


class SelectionFamily(C.Structure):
    _fields_ = [
        ("type", C.c_uint8), ("primary", C.c_uint8),
        ("minimum", C.c_uint8), ("maximum", C.c_uint8),
        ("allowed_mask", C.c_uint16), ("required_mask", C.c_uint16),
    ]


class LegalGroup(C.Structure):
    _fields_ = [
        ("kind", C.c_uint8), ("reserved", C.c_uint8 * 3),
        ("action", Action), ("selection", SelectionFamily),
    ]


class LegalView(C.Structure):
    _fields_ = [
        ("group_count", C.c_uint16), ("reserved", C.c_uint16),
        ("action_count", C.c_uint32),
        ("groups", LegalGroup * MAX_LEGAL_GROUPS),
    ]


class RngStream(C.Structure):
    _fields_ = [("key_hash", C.c_uint64), ("value", C.c_double)]


class State(C.Structure):
    _fields_ = [
        ("config", Config), ("numeric_seed", C.c_uint64), ("seed", C.c_char * 32),
        ("hashed_seed", C.c_double), ("rng", RngStream * MAX_RNG_STREAMS),
        ("rng_count", C.c_uint16), ("next_sort_id", C.c_uint16),
        ("phase", C.c_uint8), ("blind_on_deck", C.c_uint8), ("ante", C.c_uint8),
        ("round", C.c_uint8), ("won", C.c_uint8), ("terminal", C.c_uint8),
        ("hands_left", C.c_uint8), ("discards_left", C.c_uint8),
        ("hands_played", C.c_uint8), ("discards_used", C.c_uint8),
        ("hand_size", C.c_uint8), ("joker_slots", C.c_uint8),
        ("consumable_slots", C.c_uint8), ("skips", C.c_uint8),
        ("blind_skipped_mask", C.c_uint8), ("hand_sort_suit", C.c_uint8),
        ("dollars", C.c_int32), ("chips", C.c_double), ("blind_chips", C.c_double),
        ("last_hand_score", C.c_double), ("reroll_cost", C.c_int32),
        ("round_earnings", C.c_int32), ("interest_cap", C.c_int16),
        ("interest_amount", C.c_int8), ("rental_rate", C.c_int8),
        ("blind_reward", C.c_int8), ("stake_scaling", C.c_uint8),
        ("actions_taken", C.c_uint32), ("run_hands_played", C.c_uint32),
        ("deck", Card * MAX_DECK), ("hand", Card * MAX_HAND),
        ("discard", Card * MAX_DECK), ("jokers", Card * MAX_JOKERS),
        ("consumables", Card * MAX_CONSUMABLES),
        ("shop_cards", Card * MAX_SHOP_CARDS), ("pack_cards", Card * MAX_PACK_CARDS),
        ("deck_count", C.c_uint16), ("hand_count", C.c_uint8),
        ("discard_count", C.c_uint16), ("joker_count", C.c_uint8),
        ("consumable_count", C.c_uint8), ("shop_count", C.c_uint8),
        ("pack_count", C.c_uint8), ("hand_levels", C.c_uint8 * HAND_COUNT),
        ("used_centers", C.c_uint8 * ((300 + 7) // 8)),
        ("reroll_base", C.c_uint8), ("reroll_increase", C.c_uint8),
        ("free_rerolls", C.c_uint8),
        ("first_shop_buffoon", C.c_uint8),
        ("shop_joker_max", C.c_uint8), ("hands_per_round", C.c_uint8),
        ("discards_per_round", C.c_uint8), ("discount_percent", C.c_uint8),
        ("joker_rate", C.c_float), ("tarot_rate", C.c_float),
        ("planet_rate", C.c_float), ("spectral_rate", C.c_float),
        ("playing_card_rate", C.c_float), ("edition_rate", C.c_float),
        ("pack_choices", C.c_uint8), ("pack_kind", C.c_uint8),
        ("shop_return_phase", C.c_uint8),
        ("pending_free_pack_id", C.c_uint16),
        ("hand_plays", C.c_uint16 * HAND_COUNT),
        ("hand_plays_round", C.c_uint8 * HAND_COUNT),
        ("tarots_used", C.c_uint16), ("planet_usage_mask", C.c_uint16),
        ("ancient_suit", C.c_uint8),
        ("idol_rank", C.c_uint8), ("idol_suit", C.c_uint8),
        ("mail_rank", C.c_uint8), ("castle_suit", C.c_uint8),
        ("blind_id", C.c_uint16), ("blind_disabled", C.c_uint8),
        ("blind_only_hand", C.c_uint8), ("blind_hands_mask", C.c_uint16),
        ("most_played_hand", C.c_uint8),
        ("last_hand_type", C.c_uint8),
        ("base_hand_size", C.c_uint8),
        ("next_boss_id", C.c_uint16), ("boss_rerolled", C.c_uint8),
        ("boss_usage", C.c_uint8 * 31),
        ("double_tag", C.c_uint8),
        ("blind_tags", C.c_uint8 * 2), ("orbital_hands", C.c_uint8 * 3),
        ("active_tag", C.c_uint8),
        ("unused_discards", C.c_uint16), ("tag_hand_bonus", C.c_uint8),
        ("tag_force_rarity", C.c_uint8), ("tag_force_rarity_count", C.c_uint8),
        ("tag_force_edition", C.c_uint8), ("tag_force_edition_count", C.c_uint8),
        ("tag_voucher_pending", C.c_uint8), ("tag_coupon_pending", C.c_uint8),
        ("tag_coupon_active", C.c_uint8), ("tag_saved_discount", C.c_uint8),
        ("tag_investment_pending", C.c_uint8),
        ("tag_d_six_pending", C.c_uint8), ("tag_d_six_active", C.c_uint8),
        ("ecto_penalty", C.c_uint8),
        ("next_voucher_id", C.c_uint16),
        ("gros_michel_extinct", C.c_uint8),
        ("last_tarot_planet", C.c_uint16),
    ]


class PlayingCardVariants(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("rank", C.c_uint8 * OBS_MAX_PLAYING_VARIANTS),
        ("suit", C.c_uint8 * OBS_MAX_PLAYING_VARIANTS),
        ("enhancement", C.c_uint8 * OBS_MAX_PLAYING_VARIANTS),
        ("edition", C.c_uint8 * OBS_MAX_PLAYING_VARIANTS),
        ("seal", C.c_uint8 * OBS_MAX_PLAYING_VARIANTS),
        ("flags", C.c_uint8 * OBS_MAX_PLAYING_VARIANTS),
        ("perma_bonus", C.c_float * OBS_MAX_PLAYING_VARIANTS),
        ("owned_count", C.c_uint16 * OBS_MAX_PLAYING_VARIANTS),
        ("draw_count", C.c_uint16 * OBS_MAX_PLAYING_VARIANTS),
        ("hand_count", C.c_uint16 * OBS_MAX_PLAYING_VARIANTS),
        ("discard_count", C.c_uint16 * OBS_MAX_PLAYING_VARIANTS),
        ("valid", C.c_uint8 * OBS_MAX_PLAYING_VARIANTS),
    ]


class ObservedHand(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("variant", C.c_uint16 * OBS_MAX_HAND),
        ("flags", C.c_uint8 * OBS_MAX_HAND),
        ("valid", C.c_uint8 * OBS_MAX_HAND),
    ]


class DeckSummary(C.Structure):
    _fields_ = [
        ("rank", C.c_uint16 * 13), ("suit", C.c_uint16 * 4),
        ("rank_suit", (C.c_uint16 * 13) * 4),
        ("enhancement", C.c_uint16 * 9), ("edition", C.c_uint16 * 5),
        ("seal", C.c_uint16 * 5),
        ("face", C.c_uint16), ("numbered", C.c_uint16),
        ("ace", C.c_uint16), ("stone", C.c_uint16),
        ("wild", C.c_uint16), ("steel", C.c_uint16),
        ("gold", C.c_uint16), ("glass", C.c_uint16),
        ("enhanced", C.c_uint16), ("unmodified", C.c_uint16),
        ("total", C.c_uint16),
    ]


def _public_cards_type(name: str, capacity: int) -> type[C.Structure]:
    return type(name, (C.Structure,), {"_fields_": [
        ("count", C.c_uint16), ("center_id", C.c_uint16 * capacity),
        ("rank", C.c_uint8 * capacity), ("suit", C.c_uint8 * capacity),
        ("enhancement", C.c_uint8 * capacity), ("edition", C.c_uint8 * capacity),
        ("seal", C.c_uint8 * capacity), ("flags", C.c_uint8 * capacity),
        ("perma_bonus", C.c_float * capacity), ("cost", C.c_float * capacity),
        ("sell_cost", C.c_float * capacity),
        ("mutable_raw", (C.c_int32 * capacity) * 4),
        ("mutable_value", (C.c_float * capacity) * 4),
        ("valid", C.c_uint8 * capacity),
    ]})


ObservedJokers = _public_cards_type("ObservedJokers", OBS_MAX_JOKERS)
ObservedConsumables = _public_cards_type("ObservedConsumables", OBS_MAX_CONSUMABLES)
ObservedShopMain = _public_cards_type("ObservedShopMain", OBS_MAX_SHOP_MAIN)
ObservedShopVouchers = _public_cards_type("ObservedShopVouchers", OBS_MAX_SHOP_VOUCHERS)
ObservedShopBoosters = _public_cards_type("ObservedShopBoosters", OBS_MAX_SHOP_BOOSTERS)
ObservedPackCards = _public_cards_type("ObservedPackCards", OBS_MAX_PACK_CARDS)


class ObservedTags(C.Structure):
    _fields_ = [
        ("count", C.c_uint16), ("tag_id", C.c_uint8 * OBS_MAX_TAGS),
        ("orbital_hand", C.c_uint8 * OBS_MAX_TAGS),
        ("flags", C.c_uint8 * OBS_MAX_TAGS), ("valid", C.c_uint8 * OBS_MAX_TAGS),
    ]


class ObservedPokerHands(C.Structure):
    _fields_ = [
        ("visible", C.c_uint8 * HAND_COUNT), ("level", C.c_uint32 * HAND_COUNT),
        ("chips", C.c_float * HAND_COUNT), ("mult", C.c_float * HAND_COUNT),
        ("total_plays", C.c_uint32 * HAND_COUNT),
        ("round_plays", C.c_uint32 * HAND_COUNT),
    ]


class ObservationScalars(C.Structure):
    _fields_ = [
        ("deck_id", C.c_uint16), ("blind_id", C.c_uint16),
        ("next_boss_id", C.c_uint16), ("next_voucher_id", C.c_uint16),
        ("last_tarot_planet", C.c_uint16), ("stake", C.c_uint8),
        ("phase", C.c_uint8), ("blind_on_deck", C.c_uint8),
        ("won", C.c_uint8), ("terminal", C.c_uint8),
        ("blind_disabled", C.c_uint8), ("hand_sort_suit", C.c_uint8),
        ("most_played_hand", C.c_uint8), ("last_hand_type", C.c_uint8),
        ("blind_skipped_mask", C.c_uint8), ("blind_only_hand", C.c_uint8),
        ("boss_rerolled", C.c_uint8), ("free_rerolls", C.c_uint8),
        ("reroll_base", C.c_uint8), ("reroll_increase", C.c_uint8),
        ("discount_percent", C.c_uint8), ("hands_per_round", C.c_uint8),
        ("discards_per_round", C.c_uint8), ("base_hand_size", C.c_uint8),
        ("pack_kind", C.c_uint8), ("double_tag", C.c_uint8),
        ("active_tag", C.c_uint8), ("tag_hand_bonus", C.c_uint8),
        ("tag_force_rarity", C.c_uint8), ("tag_force_rarity_count", C.c_uint8),
        ("tag_force_edition", C.c_uint8), ("tag_force_edition_count", C.c_uint8),
        ("tag_voucher_pending", C.c_uint8), ("tag_coupon_pending", C.c_uint8),
        ("tag_coupon_active", C.c_uint8), ("tag_investment_pending", C.c_uint8),
        ("tag_d_six_pending", C.c_uint8), ("tag_d_six_active", C.c_uint8),
        ("ecto_penalty", C.c_uint8), ("gros_michel_extinct", C.c_uint8),
        ("ante", C.c_uint32), ("round", C.c_uint32),
        ("actions_taken", C.c_uint32), ("run_hands_played", C.c_uint32),
        ("hands_left", C.c_uint16), ("discards_left", C.c_uint16),
        ("hands_played", C.c_uint16), ("discards_used", C.c_uint16),
        ("hand_size", C.c_uint16), ("joker_slots", C.c_uint16),
        ("consumable_slots", C.c_uint16), ("skips", C.c_uint16),
        ("pack_choices", C.c_uint16), ("unused_discards", C.c_uint16),
        ("blind_hands_mask", C.c_uint16), ("tarots_used", C.c_uint16),
        ("planet_usage_mask", C.c_uint16),
        ("chips_number", C.c_uint8), ("blind_chips_number", C.c_uint8),
        ("last_hand_score_number", C.c_uint8),
        ("chips_over_blind_number", C.c_uint8),
        ("dollars", C.c_float), ("chips", C.c_float),
        ("blind_chips", C.c_float), ("last_hand_score", C.c_float),
        ("chips_over_blind", C.c_float), ("reroll_cost", C.c_float),
        ("round_earnings", C.c_float), ("interest_cap", C.c_float),
        ("interest_amount", C.c_float), ("blind_reward", C.c_float),
        ("joker_rate", C.c_float), ("tarot_rate", C.c_float),
        ("planet_rate", C.c_float), ("spectral_rate", C.c_float),
        ("playing_card_rate", C.c_float), ("edition_rate", C.c_float),
        ("redeemed_vouchers", C.c_uint8 * CENTER_COUNT),
    ]


class ObservedSelection(C.Structure):
    _fields_ = [
        ("allowed_hand", C.c_uint64), ("required_hand", C.c_uint64),
        ("minimum", C.c_uint8), ("maximum", C.c_uint8),
        ("valid", C.c_uint8), ("reserved", C.c_uint8),
    ]


class ObservedLegality(C.Structure):
    _fields_ = [
        ("action_type", C.c_uint8 * ACTION_TYPE_COUNT),
        ("primary", C.c_uint64 * ACTION_TYPE_COUNT),
        ("play", ObservedSelection), ("discard", ObservedSelection),
        ("consumable", ObservedSelection * OBS_MAX_CONSUMABLES),
        ("shop", ObservedSelection * OBS_MAX_SHOP_MAIN),
        ("pack", ObservedSelection * OBS_MAX_PACK_CARDS),
        ("hand_reorder_destination", C.c_uint64 * OBS_MAX_HAND),
        ("joker_reorder_destination", C.c_uint64 * OBS_MAX_JOKERS),
    ]


class Observation(C.Structure):
    _fields_ = [
        ("profile", ObservationProfile), ("encoded_bytes", C.c_uint32),
        ("required_capacity", C.c_uint16), ("truncation_reason", C.c_uint8),
        ("overflow_section", C.c_uint8), ("scalars", ObservationScalars),
        ("variants", PlayingCardVariants), ("hand", ObservedHand),
        ("owned_deck", DeckSummary), ("draw_pile", DeckSummary),
        ("jokers", ObservedJokers), ("consumables", ObservedConsumables),
        ("shop", ObservedShopMain), ("shop_vouchers", ObservedShopVouchers),
        ("shop_boosters", ObservedShopBoosters), ("pack", ObservedPackCards),
        ("tags", ObservedTags), ("poker_hands", ObservedPokerHands),
        ("legal", ObservedLegality),
    ]


class PolicyAction(C.Structure):
    _fields_ = [
        ("type", C.c_uint8), ("selection_count", C.c_uint8),
        ("primary", C.c_uint16), ("selection", C.c_uint16 * MAX_SELECTION),
        ("reorder_destination", C.c_uint16),
    ]


def observation_tensors(observation: Observation) -> dict[str, object]:
    """Return a named NumPy tensor tree backed by a native observation.

    Fixed-capacity arrays are zero-copy views; scalar leaves are NumPy scalar
    arrays.  Keep ``observation`` alive while using the returned views.
    """
    import numpy as np

    def convert(value: object) -> object:
        if isinstance(value, C.Array):
            return np.ctypeslib.as_array(value)
        if isinstance(value, C.Structure):
            return {name: convert(getattr(value, name)) for name, *_ in value._fields_}
        return np.asarray(value)

    return convert(observation)  # type: ignore[return-value]


class StepResult(C.Structure):
    _fields_ = [
        ("reward", C.c_float), ("sparse_reward", C.c_float),
        ("terminal", C.c_uint8), ("won", C.c_uint8),
        ("ante", C.c_uint8), ("truncated", C.c_uint8),
        ("truncation_reason", C.c_uint8), ("reserved", C.c_uint8 * 3),
    ]


class BalatroCore:
    def __init__(self, library: str | Path | None = None):
        root = Path(__file__).resolve().parents[2]
        configured = os.environ.get("BALATRO_LIB")
        path = Path(library or configured) if (library or configured) else root / "build" / "libbalatro.so"
        self.lib = C.CDLL(str(path))
        self.trusted_steps = 0
        self.batched_play_actions = 0
        self.lib.balatro_default_config.argtypes = [C.POINTER(Config)]
        self.lib.balatro_observation_size.restype = C.c_size_t
        self.lib.balatro_default_observation_profile.argtypes = [C.POINTER(ObservationProfile)]
        self.lib.balatro_init.argtypes = [C.POINTER(State), C.POINTER(Config), C.c_uint64]
        self.lib.balatro_init.restype = C.c_int
        self.lib.balatro_init_seed_string.argtypes = [C.POINTER(State), C.POINTER(Config), C.c_char_p]
        self.lib.balatro_init_seed_string.restype = C.c_int
        self.lib.balatro_step.argtypes = [C.POINTER(State), C.POINTER(Action), C.POINTER(StepResult)]
        self.lib.balatro_step.restype = C.c_int
        self.lib.balatro_observe.argtypes = [C.POINTER(State), C.POINTER(Observation)]
        self.lib.balatro_observe.restype = C.c_int
        self.lib.balatro_observe_batch.argtypes = [
            C.POINTER(State), C.c_size_t, C.POINTER(Observation), C.POINTER(C.c_int8),
        ]
        self.lib.balatro_observe_batch.restype = C.c_int
        self.lib.balatro_action_from_observation.argtypes = [
            C.POINTER(State), C.POINTER(PolicyAction), C.POINTER(Action),
        ]
        self.lib.balatro_action_from_observation.restype = C.c_int
        self.lib.balatro_step_observe.argtypes = [
            C.POINTER(State), C.POINTER(PolicyAction),
            C.POINTER(StepResult), C.POINTER(Observation),
        ]
        self.lib.balatro_step_observe.restype = C.c_int
        self.lib.balatro_step_observe_trusted.argtypes = self.lib.balatro_step_observe.argtypes
        self.lib.balatro_step_observe_trusted.restype = C.c_int
        self.lib.balatro_step_observe_batch.argtypes = [
            C.POINTER(State), C.POINTER(PolicyAction), C.c_size_t,
            C.POINTER(StepResult), C.POINTER(Observation), C.POINTER(C.c_int8),
        ]
        self.lib.balatro_step_observe_batch.restype = C.c_int
        self.lib.balatro_step_observe_batch_trusted.argtypes = self.lib.balatro_step_observe_batch.argtypes
        self.lib.balatro_step_observe_batch_trusted.restype = C.c_int
        self.lib.balatro_clone_state.argtypes = [C.POINTER(State), C.POINTER(State)]
        self.lib.balatro_step_trusted.argtypes = [
            C.POINTER(State), C.POINTER(Action), C.POINTER(StepResult),
        ]
        self.lib.balatro_step_trusted.restype = C.c_int
        self.lib.balatro_score_play_actions_trusted.argtypes = [
            C.POINTER(State), C.POINTER(Action), C.c_size_t, C.POINTER(C.c_double),
        ]
        self.lib.balatro_score_play_actions_trusted.restype = C.c_int
        self.lib.balatro_legal_view.argtypes = [C.POINTER(State), C.POINTER(LegalView)]
        self.lib.balatro_legal_view.restype = C.c_int
        self.lib.balatro_legal_group_count.argtypes = [C.POINTER(LegalGroup)]
        self.lib.balatro_legal_group_count.restype = C.c_uint32
        self.lib.balatro_legal_group_action.argtypes = [
            C.POINTER(LegalGroup), C.c_uint32, C.POINTER(Action),
        ]
        self.lib.balatro_legal_group_action.restype = C.c_int
        self.lib.balatro_legal_group_actions.argtypes = [
            C.POINTER(LegalGroup), C.POINTER(Action), C.c_size_t,
        ]
        self.lib.balatro_legal_group_actions.restype = C.c_int
        self.lib.balatro_state_hash.argtypes = [C.POINTER(State)]
        self.lib.balatro_state_hash.restype = C.c_uint64
        self.lib.balatro_classify_hand.argtypes = [C.POINTER(Card), C.c_size_t, C.POINTER(C.c_uint8)]
        self.lib.balatro_classify_hand.restype = C.c_int
        self.lib.balatro_populate_shop.argtypes = [C.POINTER(State)]
        self.lib.balatro_populate_shop.restype = None
        self.lib.balatro_debug_add_center.argtypes = [
            C.POINTER(State), C.c_uint16, C.c_uint8, C.c_uint8,
        ]
        self.lib.balatro_debug_add_center.restype = C.c_int
        self.lib.balatro_debug_add_playing_card.argtypes = [
            C.POINTER(State), C.c_uint8, C.c_uint8, C.c_uint8, C.c_uint8, C.c_uint8,
        ]
        self.lib.balatro_debug_add_playing_card.restype = C.c_int
        if self.lib.balatro_state_size() != C.sizeof(State):
            raise RuntimeError("Python State layout does not match balatro_core ABI")
        if self.lib.balatro_observation_size() != C.sizeof(Observation):
            raise RuntimeError("Python Observation layout does not match balatro_core ABI")

    def reset(self, seed: int, config: Config | None = None) -> State:
        config = config or Config()
        if config.observation.playing_cards == 0:
            self.lib.balatro_default_config(C.byref(config))
        state = State()
        error = self.lib.balatro_init(C.byref(state), C.byref(config), seed)
        if error:
            raise RuntimeError(f"balatro_init failed: {error}")
        return state

    def reset_seed_string(self, seed: str, config: Config | None = None) -> State:
        config = config or Config()
        if config.observation.playing_cards == 0:
            self.lib.balatro_default_config(C.byref(config))
        state = State()
        error = self.lib.balatro_init_seed_string(
            C.byref(state), C.byref(config), seed.encode("ascii")
        )
        if error:
            raise RuntimeError(f"balatro_init_seed_string failed: {error}")
        return state

    def legal_actions(self, state: State) -> list[Action]:
        view = self.legal_view(state)
        actions: list[Action] = []
        for group_index in range(view.group_count):
            group = view.groups[group_index]
            count = self.legal_group_count(group)
            decoded = (Action * count)()
            result = self.lib.balatro_legal_group_actions(
                C.byref(group), decoded, count,
            )
            if result != count:
                raise RuntimeError(f"balatro_legal_group_actions failed: {result}")
            actions.extend(decoded)
        return actions

    def legal_view(self, state: State) -> LegalView:
        view = LegalView()
        error = self.lib.balatro_legal_view(C.byref(state), C.byref(view))
        if error:
            raise RuntimeError(f"balatro_legal_view failed: {error}")
        return view

    def legal_group_action(self, group: LegalGroup, ordinal: int) -> Action:
        action = Action()
        error = self.lib.balatro_legal_group_action(
            C.byref(group), ordinal, C.byref(action)
        )
        if error:
            raise RuntimeError(f"balatro_legal_group_action failed: {error}")
        return action

    def legal_group_count(self, group: LegalGroup) -> int:
        return int(self.lib.balatro_legal_group_count(C.byref(group)))

    def step(self, state: State, action: Action) -> StepResult:
        result = StepResult()
        error = self.lib.balatro_step(C.byref(state), C.byref(action), C.byref(result))
        if error:
            raise RuntimeError(f"balatro_step failed: {error}")
        return result

    def observe(self, state: State) -> Observation:
        observation = Observation()
        error = self.lib.balatro_observe(C.byref(state), C.byref(observation))
        if error:
            raise RuntimeError(
                f"balatro_observe failed: {error} "
                f"(section={observation.overflow_section}, "
                f"required={observation.required_capacity})"
            )
        return observation

    def observe_batch(
        self, states: C.Array[State],
    ) -> tuple[C.Array[Observation], C.Array[C.c_int8]]:
        count = len(states)
        observations = (Observation * count)()
        status = (C.c_int8 * count)()
        error = self.lib.balatro_observe_batch(states, count, observations, status)
        if error:
            raise RuntimeError(f"balatro_observe_batch failed: {error}")
        return observations, status

    def policy_action(self, state: State, policy: PolicyAction) -> Action:
        action = Action()
        error = self.lib.balatro_action_from_observation(
            C.byref(state), C.byref(policy), C.byref(action)
        )
        if error:
            raise RuntimeError(f"balatro_action_from_observation failed: {error}")
        return action

    def step_observe(
        self, state: State, policy: PolicyAction, *, trusted: bool = False,
    ) -> tuple[StepResult, Observation]:
        result = StepResult()
        observation = Observation()
        step = self.lib.balatro_step_observe_trusted if trusted else self.lib.balatro_step_observe
        error = step(
            C.byref(state), C.byref(policy), C.byref(result), C.byref(observation)
        )
        if error:
            raise RuntimeError(f"balatro_step_observe failed: {error}")
        return result, observation

    def step_observe_batch(
        self, states: C.Array[State], actions: C.Array[PolicyAction], *, trusted: bool = False,
    ) -> tuple[C.Array[StepResult], C.Array[Observation], C.Array[C.c_int8]]:
        count = len(states)
        if len(actions) != count:
            raise ValueError("states and actions must have the same length")
        results = (StepResult * count)()
        observations = (Observation * count)()
        status = (C.c_int8 * count)()
        step = self.lib.balatro_step_observe_batch_trusted if trusted else self.lib.balatro_step_observe_batch
        error = step(
            states, actions, count, results, observations, status
        )
        if error:
            raise RuntimeError(f"balatro_step_observe_batch failed: {error}")
        return results, observations, status

    @staticmethod
    def observation_tensors(observation: Observation) -> dict[str, object]:
        return observation_tensors(observation)

    def clone_state(self, state: State) -> State:
        cloned = State()
        self.lib.balatro_clone_state(C.byref(cloned), C.byref(state))
        return cloned

    def trusted_step(self, state: State, action: Action) -> StepResult:
        result = StepResult()
        error = self.lib.balatro_step_trusted(
            C.byref(state), C.byref(action), C.byref(result)
        )
        if error:
            raise RuntimeError(f"balatro_step_trusted failed: {error}")
        self.trusted_steps += 1
        return result

    def trusted_play_scores(self, state: State, actions: list[Action]) -> list[float]:
        if not actions:
            return []
        action_array = (Action * len(actions))(*actions)
        scores = (C.c_double * len(actions))()
        error = self.lib.balatro_score_play_actions_trusted(
            C.byref(state), action_array, len(actions), scores
        )
        if error:
            raise RuntimeError(f"balatro_score_play_actions_trusted failed: {error}")
        self.batched_play_actions += len(actions)
        return list(scores)

    def state_hash(self, state: State) -> int:
        return int(self.lib.balatro_state_hash(C.byref(state)))

    def debug_add_center(
        self, state: State, center_id: int, *, edition: int = 255, flags: int = 0
    ) -> None:
        error = self.lib.balatro_debug_add_center(
            C.byref(state), center_id, edition, flags
        )
        if error:
            raise RuntimeError(f"balatro_debug_add_center failed: {error}")

    def debug_add_playing_card(
        self, state: State, suit: int, rank: int, *, enhancement: int = 0,
        edition: int = 0, seal: int = 0,
    ) -> None:
        error = self.lib.balatro_debug_add_playing_card(
            C.byref(state), suit, rank, enhancement, edition, seal
        )
        if error:
            raise RuntimeError(f"balatro_debug_add_playing_card failed: {error}")
