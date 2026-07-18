from __future__ import annotations

import base64
import json
from pathlib import Path

from simulatro.core import Action, Config, State, StepResult
from tools.heuristic_baseline import _verify_live_state, action_record, play_balatrobot


def make_action(kind: int, primary: int = 0) -> Action:
    action = Action()
    action.type = kind
    action.primary = primary
    return action


def test_action_record_maps_combined_shop_indices_to_live_areas() -> None:
    state = State()
    state.shop_count = 4
    state.shop_cards[0].center_id = 146  # Main shop Joker.
    state.shop_cards[1].center_id = 274  # Voucher.
    state.shop_cards[2].center_id = 241  # Booster.
    state.shop_cards[3].center_id = 86   # Main card appended after voucher.

    assert action_record(state, make_action(8, 3))["primary"] == 1
    assert action_record(state, make_action(12, 1))["primary"] == 0
    assert action_record(state, make_action(13, 2))["primary"] == 0


def test_action_record_expands_adjacent_reorder_to_a_permutation() -> None:
    state = State()
    state.joker_count = 3
    assert action_record(state, make_action(16, 1))["order"] == [0, 2, 1]
    assert action_record(state, make_action(15, 1))["order"] == [1, 0, 2]


def test_live_verification_compares_which_joker_is_debuffed() -> None:
    state = State()
    state.phase = 1
    state.joker_count = 2
    state.jokers[0].flags = 1
    live = {
        "state": "SELECTING_HAND",
        "jokers": {
            "count": 2,
            "cards": [
                {"state": []},
                {"state": {"debuff": True}},
            ],
        },
    }
    try:
        _verify_live_state(state, live)
    except RuntimeError as error:
        assert "joker_debuffs" in str(error)
    else:
        raise AssertionError("different Joker debuffs must diverge")


def test_play_balatrobot_hooks_policy_to_a_seeded_shadow_run(tmp_path: Path) -> None:
    class FakeCore:
        def reset_seed_string(self, seed: str, config: Config) -> State:
            assert seed == "HOOKTEST"
            state = State()
            state.phase = 0
            state.ante = 1
            state.dollars = 4
            return state

        def legal_actions(self, state: State) -> list[Action]:
            return [make_action(2)]

        def step(self, state: State, action: Action) -> StepResult:
            assert action.type == 2
            state.phase = 5
            state.ante = 9
            state.round = 24
            state.won = state.terminal = 1
            state.actions_taken += 1
            return StepResult()

    class FakeClient:
        def __init__(self) -> None:
            self.endpoint = "http://127.0.0.1:12346"
            self.calls: list[tuple[str, dict]] = []

        def call(self, method: str, params: dict) -> dict:
            self.calls.append((method, params))
            if method == "start":
                return {
                    "state": "BLIND_SELECT", "seed": "HOOKTEST", "ante_num": 1,
                    "round_num": 0, "money": 4, "jokers": {"count": 0},
                    "consumables": {"count": 0},
                }
            if method == "select":
                return {
                    "state": "GAME_OVER", "seed": "HOOKTEST", "won": True,
                    "ante_num": 9, "round_num": 24, "money": 4,
                    "jokers": {"count": 0}, "consumables": {"count": 0},
                }
            return {"state": "MENU"}

    client = FakeClient()
    trace_path = tmp_path / "heuristic.json"
    result = play_balatrobot(
        FakeCore(), client, Config(), seed="HOOKTEST", trace=trace_path,
    )  # type: ignore[arg-type]
    assert client.calls == [
        ("menu", {}),
        ("start", {"deck": "RED", "stake": "WHITE", "seed": "HOOKTEST"}),
        ("select", {}),
    ]
    assert result["won"] is True
    assert result["actions"] == 1
    assert result["trace"] == str(trace_path)

    trace = json.loads(trace_path.read_text())
    assert trace["status"] == "matched"
    assert trace["baseline_actions"] == [
        {"primary": 0, "selection": [], "type": 2},
    ]
    assert len(trace["transitions"]) == 1
    assert trace["transitions"][0]["pre_live"]["state"] == "BLIND_SELECT"
    assert trace["transitions"][0]["live"]["state"] == "GAME_OVER"
    native = trace["transitions"][0]["native"]
    assert len(base64.b64decode(native["snapshot"])) == native["state_size"]
    assert native["summary"]["actions"] == 1


def test_play_balatrobot_persists_the_diverging_transition(tmp_path: Path) -> None:
    class FakeCore:
        def reset_seed_string(self, seed: str, config: Config) -> State:
            state = State()
            state.phase = 0
            state.ante = 1
            state.dollars = 4
            return state

        def legal_actions(self, state: State) -> list[Action]:
            return [make_action(2)]

        def step(self, state: State, action: Action) -> StepResult:
            state.phase = 5
            state.ante = 2
            state.dollars = 4
            state.actions_taken = 1
            return StepResult()

    class FakeClient:
        endpoint = "http://127.0.0.1:12346"

        def call(self, method: str, params: dict) -> dict:
            if method == "start":
                return {
                    "state": "BLIND_SELECT", "seed": "DIVERGE", "ante_num": 1,
                    "round_num": 0, "money": 4, "jokers": {"count": 0},
                    "consumables": {"count": 0},
                }
            if method == "select":
                return {
                    "state": "GAME_OVER", "seed": "DIVERGE", "ante_num": 3,
                    "round_num": 0, "money": 4, "jokers": {"count": 0},
                    "consumables": {"count": 0},
                }
            return {"state": "MENU"}

    trace_path = tmp_path / "divergence.json"
    try:
        play_balatrobot(
            FakeCore(), FakeClient(), Config(), seed="DIVERGE", trace=trace_path,
        )  # type: ignore[arg-type]
    except RuntimeError as error:
        assert "diverged" in str(error)
    else:
        raise AssertionError("the mismatched ante must diverge")

    trace = json.loads(trace_path.read_text())
    assert trace["status"] == "diverged"
    assert trace["stage"] == "baseline"
    assert trace["step"] == 1
    assert trace["action"]["type"] == 2
    assert trace["transitions"][0]["pre_live"]["ante_num"] == 1
    assert trace["transitions"][0]["live"]["ante_num"] == 3
    assert trace["final"]["native"]["summary"]["ante"] == 2
