"""Generated from include/balatro.h by tools/generate_ctypes.py."""

from __future__ import annotations

import ctypes as C

class Card(C.Structure):
    _fields_ = [
        ("center_id", C.c_uint16),
        ("sort_id", C.c_uint16),
        ("suit", C.c_uint8),
        ("rank", C.c_uint8),
        ("enhancement", C.c_uint8),
        ("edition", C.c_uint8),
        ("seal", C.c_uint8),
        ("flags", C.c_uint8),
        ("perma_bonus", C.c_int16),
        ("cost", C.c_int16),
        ("sell_cost", C.c_int16),
        ("state", (C.c_int32 * 4)),
    ]

class Action(C.Structure):
    _fields_ = [
        ("type", C.c_uint8),
        ("primary", C.c_uint8),
        ("selection_count", C.c_uint8),
        ("selection", (C.c_uint8 * 5)),
    ]

class SelectionFamily(C.Structure):
    _fields_ = [
        ("type", C.c_uint8),
        ("primary", C.c_uint8),
        ("minimum", C.c_uint8),
        ("maximum", C.c_uint8),
        ("allowed_mask", C.c_uint64),
        ("required_mask", C.c_uint64),
    ]

class LegalGroup(C.Structure):
    _fields_ = [
        ("kind", C.c_uint8),
        ("reserved", (C.c_uint8 * 3)),
        ("action", Action),
        ("selection", SelectionFamily),
    ]

class LegalView(C.Structure):
    _fields_ = [
        ("group_count", C.c_uint16),
        ("reserved", C.c_uint16),
        ("action_count", C.c_uint32),
        ("groups", (LegalGroup * 512)),
    ]

class ObservationProfile(C.Structure):
    _fields_ = [
        ("playing_cards", C.c_uint16),
        ("playing_variants", C.c_uint16),
        ("hand", C.c_uint16),
        ("jokers", C.c_uint16),
        ("consumables", C.c_uint16),
        ("tags", C.c_uint16),
        ("shop_vouchers", C.c_uint16),
    ]

class Config(C.Structure):
    _fields_ = [
        ("deck", C.c_uint8),
        ("stake", C.c_uint8),
        ("win_ante", C.c_uint8),
        ("shaped_reward", C.c_uint8),
        ("validation", C.c_uint8),
        ("reserved", C.c_uint8),
        ("observation", ObservationProfile),
    ]

class RngStream(C.Structure):
    _fields_ = [
        ("key_hash", C.c_uint64),
        ("value", C.c_double),
    ]

class State(C.Structure):
    _fields_ = [
        ("config", Config),
        ("numeric_seed", C.c_uint64),
        ("seed", (C.c_char * 32)),
        ("hashed_seed", C.c_double),
        ("rng", (RngStream * 256)),
        ("rng_count", C.c_uint16),
        ("next_sort_id", C.c_uint16),
        ("phase", C.c_uint8),
        ("blind_on_deck", C.c_uint8),
        ("ante", C.c_uint8),
        ("round", C.c_uint8),
        ("won", C.c_uint8),
        ("terminal", C.c_uint8),
        ("hands_left", C.c_uint8),
        ("discards_left", C.c_uint8),
        ("hands_played", C.c_uint8),
        ("discards_used", C.c_uint8),
        ("hand_size", C.c_uint8),
        ("joker_slots", C.c_uint8),
        ("consumable_slots", C.c_uint8),
        ("skips", C.c_uint8),
        ("blind_skipped_mask", C.c_uint8),
        ("hand_sort_suit", C.c_uint8),
        ("dollars", C.c_int32),
        ("chips", C.c_double),
        ("blind_chips", C.c_double),
        ("last_hand_score", C.c_double),
        ("reroll_cost", C.c_int32),
        ("round_earnings", C.c_int32),
        ("interest_cap", C.c_int16),
        ("interest_amount", C.c_int8),
        ("rental_rate", C.c_int8),
        ("blind_reward", C.c_int8),
        ("stake_scaling", C.c_uint8),
        ("actions_taken", C.c_uint32),
        ("run_hands_played", C.c_uint32),
        ("deck", (Card * 256)),
        ("hand", (Card * 64)),
        ("discard", (Card * 256)),
        ("jokers", (Card * 32)),
        ("consumables", (Card * 32)),
        ("shop_main", (Card * 4)),
        ("shop_vouchers", (Card * 64)),
        ("shop_boosters", (Card * 2)),
        ("pack_cards", (Card * 5)),
        ("deck_count", C.c_uint16),
        ("hand_count", C.c_uint8),
        ("discard_count", C.c_uint16),
        ("joker_count", C.c_uint8),
        ("consumable_count", C.c_uint8),
        ("shop_main_count", C.c_uint8),
        ("shop_voucher_count", C.c_uint8),
        ("shop_booster_count", C.c_uint8),
        ("pack_count", C.c_uint8),
        ("hand_levels", (C.c_uint8 * 12)),
        ("used_centers", (C.c_uint8 * 38)),
        ("reroll_base", C.c_uint8),
        ("reroll_increase", C.c_uint8),
        ("free_rerolls", C.c_uint8),
        ("first_shop_buffoon", C.c_uint8),
        ("shop_joker_max", C.c_uint8),
        ("hands_per_round", C.c_uint8),
        ("discards_per_round", C.c_uint8),
        ("discount_percent", C.c_uint8),
        ("joker_rate", C.c_float),
        ("tarot_rate", C.c_float),
        ("planet_rate", C.c_float),
        ("spectral_rate", C.c_float),
        ("playing_card_rate", C.c_float),
        ("edition_rate", C.c_float),
        ("pack_choices", C.c_uint8),
        ("pack_kind", C.c_uint8),
        ("shop_return_phase", C.c_uint8),
        ("pending_free_pack_id", C.c_uint16),
        ("hand_plays", (C.c_uint16 * 12)),
        ("hand_plays_round", (C.c_uint8 * 12)),
        ("tarots_used", C.c_uint16),
        ("planet_usage_mask", C.c_uint16),
        ("ancient_suit", C.c_uint8),
        ("idol_rank", C.c_uint8),
        ("idol_suit", C.c_uint8),
        ("mail_rank", C.c_uint8),
        ("castle_suit", C.c_uint8),
        ("blind_id", C.c_uint16),
        ("blind_disabled", C.c_uint8),
        ("blind_only_hand", C.c_uint8),
        ("blind_hands_mask", C.c_uint16),
        ("most_played_hand", C.c_uint8),
        ("last_hand_type", C.c_uint8),
        ("base_hand_size", C.c_uint8),
        ("next_boss_id", C.c_uint16),
        ("boss_rerolled", C.c_uint8),
        ("boss_usage", (C.c_uint8 * 31)),
        ("double_tag", C.c_uint8),
        ("blind_tags", (C.c_uint8 * 2)),
        ("orbital_hands", (C.c_uint8 * 3)),
        ("active_tag", C.c_uint8),
        ("unused_discards", C.c_uint16),
        ("tag_hand_bonus", C.c_uint8),
        ("tag_force_rarity", C.c_uint8),
        ("tag_force_rarity_count", C.c_uint8),
        ("tag_force_edition", C.c_uint8),
        ("tag_force_edition_count", C.c_uint8),
        ("tag_voucher_pending", C.c_uint8),
        ("tag_coupon_pending", C.c_uint8),
        ("tag_coupon_active", C.c_uint8),
        ("tag_saved_discount", C.c_uint8),
        ("tag_investment_pending", C.c_uint8),
        ("tag_d_six_pending", C.c_uint8),
        ("tag_d_six_active", C.c_uint8),
        ("ecto_penalty", C.c_uint8),
        ("next_voucher_id", C.c_uint16),
        ("gros_michel_extinct", C.c_uint8),
        ("last_tarot_planet", C.c_uint16),
        ("joker_flags", C.c_uint64),
        ("joker_active_flags", C.c_uint64),
    ]

class PlayingCardVariants(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("rank", (C.c_uint8 * 256)),
        ("suit", (C.c_uint8 * 256)),
        ("enhancement", (C.c_uint8 * 256)),
        ("edition", (C.c_uint8 * 256)),
        ("seal", (C.c_uint8 * 256)),
        ("flags", (C.c_uint8 * 256)),
        ("perma_bonus", (C.c_float * 256)),
        ("owned_count", (C.c_uint16 * 256)),
        ("draw_count", (C.c_uint16 * 256)),
        ("hand_count", (C.c_uint16 * 256)),
        ("discard_count", (C.c_uint16 * 256)),
        ("valid", (C.c_uint8 * 256)),
    ]

class ObservedHand(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("variant", (C.c_uint16 * 64)),
        ("flags", (C.c_uint8 * 64)),
        ("valid", (C.c_uint8 * 64)),
    ]

class DeckSummary(C.Structure):
    _fields_ = [
        ("rank", (C.c_uint16 * 13)),
        ("suit", (C.c_uint16 * 4)),
        ("rank_suit", ((C.c_uint16 * 13) * 4)),
        ("enhancement", (C.c_uint16 * 9)),
        ("edition", (C.c_uint16 * 5)),
        ("seal", (C.c_uint16 * 5)),
        ("face", C.c_uint16),
        ("numbered", C.c_uint16),
        ("ace", C.c_uint16),
        ("stone", C.c_uint16),
        ("wild", C.c_uint16),
        ("steel", C.c_uint16),
        ("gold", C.c_uint16),
        ("glass", C.c_uint16),
        ("enhanced", C.c_uint16),
        ("unmodified", C.c_uint16),
        ("total", C.c_uint16),
    ]

class ObservedJokers(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("center_id", (C.c_uint16 * 32)),
        ("rank", (C.c_uint8 * 32)),
        ("suit", (C.c_uint8 * 32)),
        ("enhancement", (C.c_uint8 * 32)),
        ("edition", (C.c_uint8 * 32)),
        ("seal", (C.c_uint8 * 32)),
        ("flags", (C.c_uint8 * 32)),
        ("perma_bonus", (C.c_float * 32)),
        ("cost", (C.c_float * 32)),
        ("sell_cost", (C.c_float * 32)),
        ("mutable_raw", ((C.c_int32 * 32) * 4)),
        ("mutable_value", ((C.c_float * 32) * 4)),
        ("valid", (C.c_uint8 * 32)),
    ]

class ObservedConsumables(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("center_id", (C.c_uint16 * 32)),
        ("rank", (C.c_uint8 * 32)),
        ("suit", (C.c_uint8 * 32)),
        ("enhancement", (C.c_uint8 * 32)),
        ("edition", (C.c_uint8 * 32)),
        ("seal", (C.c_uint8 * 32)),
        ("flags", (C.c_uint8 * 32)),
        ("perma_bonus", (C.c_float * 32)),
        ("cost", (C.c_float * 32)),
        ("sell_cost", (C.c_float * 32)),
        ("mutable_raw", ((C.c_int32 * 32) * 4)),
        ("mutable_value", ((C.c_float * 32) * 4)),
        ("valid", (C.c_uint8 * 32)),
    ]

class ObservedShopMain(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("center_id", (C.c_uint16 * 4)),
        ("rank", (C.c_uint8 * 4)),
        ("suit", (C.c_uint8 * 4)),
        ("enhancement", (C.c_uint8 * 4)),
        ("edition", (C.c_uint8 * 4)),
        ("seal", (C.c_uint8 * 4)),
        ("flags", (C.c_uint8 * 4)),
        ("perma_bonus", (C.c_float * 4)),
        ("cost", (C.c_float * 4)),
        ("sell_cost", (C.c_float * 4)),
        ("mutable_raw", ((C.c_int32 * 4) * 4)),
        ("mutable_value", ((C.c_float * 4) * 4)),
        ("valid", (C.c_uint8 * 4)),
    ]

class ObservedShopVouchers(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("center_id", (C.c_uint16 * 64)),
        ("rank", (C.c_uint8 * 64)),
        ("suit", (C.c_uint8 * 64)),
        ("enhancement", (C.c_uint8 * 64)),
        ("edition", (C.c_uint8 * 64)),
        ("seal", (C.c_uint8 * 64)),
        ("flags", (C.c_uint8 * 64)),
        ("perma_bonus", (C.c_float * 64)),
        ("cost", (C.c_float * 64)),
        ("sell_cost", (C.c_float * 64)),
        ("mutable_raw", ((C.c_int32 * 64) * 4)),
        ("mutable_value", ((C.c_float * 64) * 4)),
        ("valid", (C.c_uint8 * 64)),
    ]

class ObservedShopBoosters(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("center_id", (C.c_uint16 * 2)),
        ("rank", (C.c_uint8 * 2)),
        ("suit", (C.c_uint8 * 2)),
        ("enhancement", (C.c_uint8 * 2)),
        ("edition", (C.c_uint8 * 2)),
        ("seal", (C.c_uint8 * 2)),
        ("flags", (C.c_uint8 * 2)),
        ("perma_bonus", (C.c_float * 2)),
        ("cost", (C.c_float * 2)),
        ("sell_cost", (C.c_float * 2)),
        ("mutable_raw", ((C.c_int32 * 2) * 4)),
        ("mutable_value", ((C.c_float * 2) * 4)),
        ("valid", (C.c_uint8 * 2)),
    ]

class ObservedPackCards(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("center_id", (C.c_uint16 * 5)),
        ("rank", (C.c_uint8 * 5)),
        ("suit", (C.c_uint8 * 5)),
        ("enhancement", (C.c_uint8 * 5)),
        ("edition", (C.c_uint8 * 5)),
        ("seal", (C.c_uint8 * 5)),
        ("flags", (C.c_uint8 * 5)),
        ("perma_bonus", (C.c_float * 5)),
        ("cost", (C.c_float * 5)),
        ("sell_cost", (C.c_float * 5)),
        ("mutable_raw", ((C.c_int32 * 5) * 4)),
        ("mutable_value", ((C.c_float * 5) * 4)),
        ("valid", (C.c_uint8 * 5)),
    ]

class ObservedTags(C.Structure):
    _fields_ = [
        ("count", C.c_uint16),
        ("tag_id", (C.c_uint8 * 64)),
        ("orbital_hand", (C.c_uint8 * 64)),
        ("flags", (C.c_uint8 * 64)),
        ("valid", (C.c_uint8 * 64)),
    ]

class ObservedPokerHands(C.Structure):
    _fields_ = [
        ("visible", (C.c_uint8 * 12)),
        ("level", (C.c_uint32 * 12)),
        ("chips", (C.c_float * 12)),
        ("mult", (C.c_float * 12)),
        ("total_plays", (C.c_uint32 * 12)),
        ("round_plays", (C.c_uint32 * 12)),
    ]

class ObservationScalars(C.Structure):
    _fields_ = [
        ("deck_id", C.c_uint16),
        ("blind_id", C.c_uint16),
        ("next_boss_id", C.c_uint16),
        ("next_voucher_id", C.c_uint16),
        ("last_tarot_planet", C.c_uint16),
        ("stake", C.c_uint8),
        ("phase", C.c_uint8),
        ("blind_on_deck", C.c_uint8),
        ("won", C.c_uint8),
        ("terminal", C.c_uint8),
        ("blind_disabled", C.c_uint8),
        ("hand_sort_suit", C.c_uint8),
        ("most_played_hand", C.c_uint8),
        ("last_hand_type", C.c_uint8),
        ("blind_skipped_mask", C.c_uint8),
        ("blind_only_hand", C.c_uint8),
        ("boss_rerolled", C.c_uint8),
        ("free_rerolls", C.c_uint8),
        ("reroll_base", C.c_uint8),
        ("reroll_increase", C.c_uint8),
        ("discount_percent", C.c_uint8),
        ("hands_per_round", C.c_uint8),
        ("discards_per_round", C.c_uint8),
        ("base_hand_size", C.c_uint8),
        ("pack_kind", C.c_uint8),
        ("double_tag", C.c_uint8),
        ("active_tag", C.c_uint8),
        ("tag_hand_bonus", C.c_uint8),
        ("tag_force_rarity", C.c_uint8),
        ("tag_force_rarity_count", C.c_uint8),
        ("tag_force_edition", C.c_uint8),
        ("tag_force_edition_count", C.c_uint8),
        ("tag_voucher_pending", C.c_uint8),
        ("tag_coupon_pending", C.c_uint8),
        ("tag_coupon_active", C.c_uint8),
        ("tag_investment_pending", C.c_uint8),
        ("tag_d_six_pending", C.c_uint8),
        ("tag_d_six_active", C.c_uint8),
        ("ecto_penalty", C.c_uint8),
        ("gros_michel_extinct", C.c_uint8),
        ("ante", C.c_uint32),
        ("round", C.c_uint32),
        ("actions_taken", C.c_uint32),
        ("run_hands_played", C.c_uint32),
        ("hands_left", C.c_uint16),
        ("discards_left", C.c_uint16),
        ("hands_played", C.c_uint16),
        ("discards_used", C.c_uint16),
        ("hand_size", C.c_uint16),
        ("joker_slots", C.c_uint16),
        ("consumable_slots", C.c_uint16),
        ("skips", C.c_uint16),
        ("pack_choices", C.c_uint16),
        ("unused_discards", C.c_uint16),
        ("blind_hands_mask", C.c_uint16),
        ("tarots_used", C.c_uint16),
        ("planet_usage_mask", C.c_uint16),
        ("chips_number", C.c_uint8),
        ("blind_chips_number", C.c_uint8),
        ("last_hand_score_number", C.c_uint8),
        ("chips_over_blind_number", C.c_uint8),
        ("dollars", C.c_float),
        ("chips", C.c_float),
        ("blind_chips", C.c_float),
        ("last_hand_score", C.c_float),
        ("chips_over_blind", C.c_float),
        ("reroll_cost", C.c_float),
        ("round_earnings", C.c_float),
        ("interest_cap", C.c_float),
        ("interest_amount", C.c_float),
        ("blind_reward", C.c_float),
        ("joker_rate", C.c_float),
        ("tarot_rate", C.c_float),
        ("planet_rate", C.c_float),
        ("spectral_rate", C.c_float),
        ("playing_card_rate", C.c_float),
        ("edition_rate", C.c_float),
        ("redeemed_vouchers", (C.c_uint8 * 300)),
    ]

class ObservedSelection(C.Structure):
    _fields_ = [
        ("allowed_hand", C.c_uint64),
        ("required_hand", C.c_uint64),
        ("minimum", C.c_uint8),
        ("maximum", C.c_uint8),
        ("valid", C.c_uint8),
        ("reserved", C.c_uint8),
    ]

class ObservedLegality(C.Structure):
    _fields_ = [
        ("action_type", (C.c_uint8 * 23)),
        ("primary", (C.c_uint64 * 23)),
        ("play", ObservedSelection),
        ("discard", ObservedSelection),
        ("consumable", (ObservedSelection * 32)),
        ("shop", (ObservedSelection * 4)),
        ("pack", (ObservedSelection * 5)),
        ("hand_reorder_destination", (C.c_uint64 * 64)),
        ("joker_reorder_destination", (C.c_uint64 * 32)),
    ]

class Observation(C.Structure):
    _fields_ = [
        ("profile", ObservationProfile),
        ("encoded_bytes", C.c_uint32),
        ("required_capacity", C.c_uint16),
        ("truncation_reason", C.c_uint8),
        ("overflow_section", C.c_uint8),
        ("scalars", ObservationScalars),
        ("variants", PlayingCardVariants),
        ("hand", ObservedHand),
        ("owned_deck", DeckSummary),
        ("draw_pile", DeckSummary),
        ("jokers", ObservedJokers),
        ("consumables", ObservedConsumables),
        ("shop", ObservedShopMain),
        ("shop_vouchers", ObservedShopVouchers),
        ("shop_boosters", ObservedShopBoosters),
        ("pack", ObservedPackCards),
        ("tags", ObservedTags),
        ("poker_hands", ObservedPokerHands),
        ("legal", ObservedLegality),
    ]

class CompactGlobals(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("ids", (C.c_uint16 * 5)),
        ("u8", (C.c_uint8 * 39)),
        ("u32", (C.c_uint32 * 4)),
        ("u16", (C.c_uint16 * 13)),
        ("q8_8", (C.c_int16 * 16)),
        ("redeemed_vouchers", (C.c_uint8 * 38)),
    ]

class CompactVariant(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("rank", C.c_uint8),
        ("suit", C.c_uint8),
        ("enhancement", C.c_uint8),
        ("edition", C.c_uint8),
        ("seal", C.c_uint8),
        ("flags", C.c_uint8),
        ("perma_bonus_q8_8", C.c_int16),
        ("owned_count", C.c_uint16),
        ("draw_count", C.c_uint16),
        ("hand_count", C.c_uint16),
        ("discard_count", C.c_uint16),
    ]

class CompactVariants(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("count", C.c_uint16),
        ("values", (CompactVariant * 256)),
    ]

class CompactHandCard(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("variant", C.c_uint16),
        ("flags", C.c_uint8),
    ]

class CompactHand(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("count", C.c_uint16),
        ("values", (CompactHandCard * 64)),
    ]

class CompactCard(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("center_id", C.c_uint16),
        ("rank", C.c_uint8),
        ("suit", C.c_uint8),
        ("enhancement", C.c_uint8),
        ("edition", C.c_uint8),
        ("seal", C.c_uint8),
        ("flags", C.c_uint8),
        ("numeric_q8_8", (C.c_int16 * 7)),
    ]

class CompactJokers(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("count", C.c_uint16),
        ("values", (CompactCard * 32)),
    ]

class CompactConsumables(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("count", C.c_uint16),
        ("values", (CompactCard * 32)),
    ]

class CompactShopMain(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("count", C.c_uint16),
        ("values", (CompactCard * 4)),
    ]

class CompactShopVouchers(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("count", C.c_uint16),
        ("values", (CompactCard * 64)),
    ]

class CompactShopBoosters(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("count", C.c_uint16),
        ("values", (CompactCard * 2)),
    ]

class CompactPack(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("count", C.c_uint16),
        ("values", (CompactCard * 5)),
    ]

class CompactDeckSummary(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("rank", (C.c_uint16 * 13)),
        ("suit", (C.c_uint16 * 4)),
        ("rank_suit", ((C.c_uint16 * 13) * 4)),
        ("enhancement", (C.c_uint16 * 9)),
        ("edition", (C.c_uint16 * 5)),
        ("seal", (C.c_uint16 * 5)),
        ("derived", (C.c_uint16 * 14)),
    ]

class CompactTags(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("count", C.c_uint16),
        ("tag_id", (C.c_uint8 * 64)),
        ("orbital_hand", (C.c_uint8 * 64)),
        ("flags", (C.c_uint8 * 64)),
    ]

class CompactPokerHand(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("visible", C.c_uint8),
        ("level", C.c_uint32),
        ("chips_q8_8", C.c_int16),
        ("mult_q8_8", C.c_int16),
        ("total_plays", C.c_uint32),
        ("round_plays", C.c_uint32),
    ]

class CompactObservation(C.Structure):
    _pack_ = 1
    _fields_ = [
        ("version", C.c_uint16),
        ("globals", CompactGlobals),
        ("variants", CompactVariants),
        ("hand", CompactHand),
        ("owned_deck", CompactDeckSummary),
        ("draw_pile", CompactDeckSummary),
        ("jokers", CompactJokers),
        ("consumables", CompactConsumables),
        ("shop", CompactShopMain),
        ("shop_vouchers", CompactShopVouchers),
        ("shop_boosters", CompactShopBoosters),
        ("pack", CompactPack),
        ("tags", CompactTags),
        ("poker_hands", (CompactPokerHand * 12)),
    ]

class PolicyAction(C.Structure):
    _fields_ = [
        ("type", C.c_uint8),
        ("selection_count", C.c_uint8),
        ("primary", C.c_uint16),
        ("selection", (C.c_uint16 * 5)),
        ("reorder_destination", C.c_uint16),
    ]

class StepResult(C.Structure):
    _fields_ = [
        ("reward", C.c_float),
        ("sparse_reward", C.c_float),
        ("terminal", C.c_uint8),
        ("won", C.c_uint8),
        ("ante", C.c_uint8),
        ("truncated", C.c_uint8),
        ("truncation_reason", C.c_uint8),
        ("reserved", (C.c_uint8 * 3)),
    ]

class ScoreResult(C.Structure):
    _fields_ = [
        ("hand_type", C.c_uint8),
        ("scoring_mask", C.c_uint8),
        ("destroyed_mask", C.c_uint8),
        ("reserved", C.c_uint8),
        ("chips", C.c_double),
        ("mult", C.c_double),
        ("total", C.c_double),
        ("dollars", C.c_int32),
    ]

LegalMasks = ObservedLegality

__all__ = [name for name, value in globals().items() if isinstance(value, type) and issubclass(value, C.Structure)]
__all__.append("LegalMasks")
