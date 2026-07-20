"""Small ctypes binding for differential tests and analysis.

The native Puffer adapter links the C API directly. This module deliberately
does not duplicate game rules.
"""

from __future__ import annotations

import ctypes as C
import os
from pathlib import Path

from ._structs import (
    Action,
    Card,
    CompactObservation,
    Config,
    LegalGroup,
    LegalMasks,
    LegalView,
    Observation,
    ObservationProfile,
    PolicyAction,
    State,
    StepResult,
)

MAX_DECK = 256
MAX_HAND = 64
MAX_JOKERS = 32
MAX_CONSUMABLES = 32
MAX_SHOP_CARDS = 70
MAX_PACK_CARDS = 5
MAX_SELECTION = 5
MAX_LEGAL_ACTIONS = 20000
MAX_LEGAL_GROUPS = 512
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


class BalatroCore:
    def __init__(self, library: str | Path | None = None):
        root = Path(__file__).resolve().parents[2]
        configured = os.environ.get("BALATRO_LIB")
        path = Path(library or configured) if (library or configured) else root / "build" / "libbalatro.so"
        self.lib = C.CDLL(str(path))
        self.trusted_steps = 0
        self.batched_play_actions = 0
        self.lib.balatro_default_config.argtypes = [C.POINTER(Config)]
        self.lib.balatro_state_size.restype = C.c_size_t
        self.lib.balatro_observation_size.restype = C.c_size_t
        self.lib.balatro_compact_observation_size.restype = C.c_size_t
        self.lib.balatro_legal_masks_size.restype = C.c_size_t
        self.lib.balatro_default_observation_profile.argtypes = [C.POINTER(ObservationProfile)]
        self.lib.balatro_init.argtypes = [C.POINTER(State), C.POINTER(Config), C.c_uint64]
        self.lib.balatro_init.restype = C.c_int
        self.lib.balatro_init_seed_string.argtypes = [C.POINTER(State), C.POINTER(Config), C.c_char_p]
        self.lib.balatro_init_seed_string.restype = C.c_int
        self.lib.balatro_step.argtypes = [C.POINTER(State), C.POINTER(Action), C.POINTER(StepResult)]
        self.lib.balatro_step.restype = C.c_int
        self.lib.balatro_observe.argtypes = [C.POINTER(State), C.POINTER(Observation)]
        self.lib.balatro_observe.restype = C.c_int
        self.lib.balatro_observe_rl.argtypes = [
            C.POINTER(State), C.POINTER(CompactObservation), C.POINTER(LegalMasks),
        ]
        self.lib.balatro_observe_rl.restype = C.c_int
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
        self.lib.balatro_step_observe_rl.argtypes = [
            C.POINTER(State), C.POINTER(PolicyAction), C.POINTER(StepResult),
            C.POINTER(CompactObservation), C.POINTER(LegalMasks),
        ]
        self.lib.balatro_step_observe_rl.restype = C.c_int
        self.lib.balatro_step_observe_rl_trusted.argtypes = self.lib.balatro_step_observe_rl.argtypes
        self.lib.balatro_step_observe_rl_trusted.restype = C.c_int
        self.lib.balatro_step_observe_rl_batch.argtypes = [
            C.POINTER(State), C.POINTER(PolicyAction), C.c_size_t,
            C.POINTER(StepResult), C.POINTER(CompactObservation),
            C.POINTER(LegalMasks), C.POINTER(C.c_int8), C.c_int,
        ]
        self.lib.balatro_step_observe_rl_batch.restype = C.c_int
        self.lib.balatro_clone_state.argtypes = [C.POINTER(State), C.POINTER(State)]
        self.lib.balatro_step_trusted.argtypes = [
            C.POINTER(State), C.POINTER(Action), C.POINTER(StepResult),
        ]
        self.lib.balatro_step_trusted.restype = C.c_int
        self.lib.balatro_score_play_actions_trusted.argtypes = [
            C.POINTER(State), C.POINTER(Action), C.c_size_t, C.POINTER(C.c_double),
        ]
        self.lib.balatro_score_play_actions_trusted.restype = C.c_int
        self.lib.balatro_score_plays.argtypes = self.lib.balatro_score_play_actions_trusted.argtypes
        self.lib.balatro_score_plays.restype = C.c_int
        self.lib.balatro_legal_masks.argtypes = [C.POINTER(State), C.POINTER(LegalMasks)]
        self.lib.balatro_legal_masks.restype = C.c_int
        self.lib.balatro_legal_expand.argtypes = [C.POINTER(LegalMasks), C.POINTER(LegalView)]
        self.lib.balatro_legal_expand.restype = C.c_int
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
        if self.lib.balatro_compact_observation_size() != C.sizeof(CompactObservation):
            raise RuntimeError("Python CompactObservation layout does not match balatro_core ABI")
        if self.lib.balatro_legal_masks_size() != C.sizeof(LegalMasks):
            raise RuntimeError("Python LegalMasks layout does not match balatro_core ABI")

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

    def observe_rl(self, state: State) -> tuple[CompactObservation, LegalMasks]:
        observation = CompactObservation()
        legal = LegalMasks()
        error = self.lib.balatro_observe_rl(
            C.byref(state), C.byref(observation), C.byref(legal)
        )
        if error:
            raise RuntimeError(f"balatro_observe_rl failed: {error}")
        return observation, legal

    def legal_masks(self, state: State) -> LegalMasks:
        legal = LegalMasks()
        error = self.lib.balatro_legal_masks(C.byref(state), C.byref(legal))
        if error:
            raise RuntimeError(f"balatro_legal_masks failed: {error}")
        return legal

    def legal_expand(self, legal: LegalMasks) -> LegalView:
        view = LegalView()
        error = self.lib.balatro_legal_expand(C.byref(legal), C.byref(view))
        if error:
            raise RuntimeError(f"balatro_legal_expand failed: {error}")
        return view

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

    def step_observe_rl(
        self, state: State, policy: PolicyAction, *, trusted: bool = False,
    ) -> tuple[StepResult, CompactObservation, LegalMasks]:
        result = StepResult()
        observation = CompactObservation()
        legal = LegalMasks()
        step = self.lib.balatro_step_observe_rl_trusted if trusted else self.lib.balatro_step_observe_rl
        error = step(
            C.byref(state), C.byref(policy), C.byref(result),
            C.byref(observation), C.byref(legal),
        )
        if error:
            raise RuntimeError(f"balatro_step_observe_rl failed: {error}")
        return result, observation, legal

    def step_observe_rl_batch(
        self, states: C.Array[State], actions: C.Array[PolicyAction], *, trusted: bool = False,
    ) -> tuple[C.Array[StepResult], C.Array[CompactObservation], C.Array[LegalMasks], C.Array[C.c_int8]]:
        count = len(states)
        if len(actions) != count:
            raise ValueError("states and actions must have the same length")
        results = (StepResult * count)()
        observations = (CompactObservation * count)()
        legal = (LegalMasks * count)()
        status = (C.c_int8 * count)()
        error = self.lib.balatro_step_observe_rl_batch(
            states, actions, count, results, observations, legal, status, int(trusted)
        )
        if error:
            raise RuntimeError(f"balatro_step_observe_rl_batch failed: {error}")
        return results, observations, legal, status

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
        error = self.lib.balatro_score_plays(
            C.byref(state), action_array, len(actions), scores
        )
        if error:
            raise RuntimeError(f"balatro_score_plays failed: {error}")
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
