from __future__ import annotations

import ctypes as C
import math

import numpy as np

from simulatro.core import (
    BalatroCore,
    CompactObservation,
    Config,
    LegalMasks,
    Observation,
    PolicyAction,
    State,
    observation_tensors,
)


def configured_core() -> tuple[BalatroCore, Config]:
    core = BalatroCore()
    config = Config()
    core.lib.balatro_default_config(C.byref(config))
    return core, config


def test_public_observation_is_typed_and_excludes_hidden_order() -> None:
    core, config = configured_core()
    state = core.reset(9, config)
    observed = core.observe(state)
    assert observed.encoded_bytes == C.sizeof(Observation)
    assert observed.variants.count == 52
    assert observed.owned_deck.total == observed.draw_pile.total == 52

    hidden = State.from_buffer_copy(state)
    cards = [type(hidden.deck[0]).from_buffer_copy(hidden.deck[i]) for i in range(hidden.deck_count)]
    for index, card in enumerate(reversed(cards)):
        hidden.deck[index] = card
    hidden.rng[0].value += 0.125
    hidden.deck[0].sort_id += 1000
    reordered = core.observe(hidden)
    assert bytes(observed) == bytes(reordered)


def test_numpy_tensor_tree_is_zero_copy() -> None:
    core, config = configured_core()
    observation = core.observe(core.reset(11, config))
    tensors = observation_tensors(observation)
    rank = tensors["variants"]["rank"]
    legal_primary = tensors["legal"]["primary"]
    assert isinstance(rank, np.ndarray) and rank.shape == (256,)
    assert rank.dtype == np.uint8
    assert legal_primary.shape == (23,)
    original = observation.variants.rank[0]
    rank[0] = (original + 1) % 15
    assert observation.variants.rank[0] == rank[0]


def test_one_call_policy_step_returns_next_observation() -> None:
    core, config = configured_core()
    state = core.reset_seed_string("OBS_PY", config)
    policy = PolicyAction(type=2)
    result, observation = core.step_observe(state, policy)
    assert not result.terminal
    assert not result.truncated
    assert observation.scalars.phase == 1
    assert observation.hand.count == state.hand_count
    assert observation.legal.action_type[0]

    trusted_state = core.reset_seed_string("OBS_PY_TRUSTED", config)
    trusted_result, trusted_observation = core.step_observe(trusted_state, policy, trusted=True)
    assert not trusted_result.terminal
    assert trusted_observation.scalars.phase == 1


def test_batch_crosses_native_boundary_once() -> None:
    core, config = configured_core()
    states = (State * 2)(
        core.reset_seed_string("OBS_BATCH_A", config),
        core.reset_seed_string("OBS_BATCH_B", config),
    )
    observations, observe_status = core.observe_batch(states)
    assert list(observe_status) == [0, 0]
    assert [item.owned_deck.total for item in observations] == [52, 52]

    actions = (PolicyAction * 2)(PolicyAction(type=2), PolicyAction(type=2))
    results, next_observations, step_status = core.step_observe_batch(states, actions)
    assert list(step_status) == [0, 0]
    assert all(not result.truncated for result in results)
    assert all(item.hand.count == 8 for item in next_observations)

    trusted_states = (State * 2)(
        core.reset_seed_string("OBS_TRUSTED_A", config),
        core.reset_seed_string("OBS_TRUSTED_B", config),
    )
    _, _, trusted_status = core.step_observe_batch(trusted_states, actions, trusted=True)
    assert list(trusted_status) == [0, 0]


def test_compact_rl_step_and_batch() -> None:
    core, config = configured_core()
    state = core.reset_seed_string("OBS_RL", config)
    policy = PolicyAction(type=2, reorder_destination=65535)
    result, observation, legal = core.step_observe_rl(state, policy, trusted=True)
    assert not result.terminal
    assert isinstance(observation, CompactObservation) and observation.version == 1
    assert isinstance(legal, LegalMasks) and legal.action_type[0]
    assert core.legal_expand(legal).group_count > 0

    states = (State * 2)(
        core.reset_seed_string("OBS_RL_BATCH_A", config),
        core.reset_seed_string("OBS_RL_BATCH_B", config),
    )
    actions = (PolicyAction * 2)(policy, policy)
    _, observations, masks, status = core.step_observe_rl_batch(states, actions)
    assert list(status) == [0, 0]
    assert all(item.version == 1 for item in observations)
    assert all(item.action_type[0] for item in masks)


def test_observation_capacity_reports_distinct_overflow() -> None:
    core, config = configured_core()
    config.observation.playing_cards = 51
    state = core.reset(13, config)
    observation = Observation()
    error = core.lib.balatro_observe(C.byref(state), C.byref(observation))
    assert error == -7
    assert observation.truncation_reason == 1
    assert observation.overflow_section == 1
    assert observation.required_capacity == 52


def test_nonfinite_scores_have_finite_tensor_payloads_and_explicit_classes() -> None:
    core, config = configured_core()
    state = core.reset_seed_string("OBS_NANINF", config)
    state.chips = math.nan
    state.blind_chips = math.inf
    state.last_hand_score = -math.inf
    observation = core.observe(state)

    assert observation.scalars.chips_number == 3
    assert observation.scalars.blind_chips_number == 1
    assert observation.scalars.last_hand_score_number == 2
    assert observation.scalars.chips_over_blind_number == 3
    tensors = observation_tensors(observation)["scalars"]
    for field in ("chips", "blind_chips", "last_hand_score", "chips_over_blind"):
        assert np.isfinite(tensors[field])
