from __future__ import annotations

from tools.balatrobot_bridge import action_to_rpc, replay


class FakeClient:
    def __init__(self) -> None:
        self.calls: list[tuple[str, dict]] = []

    def call(self, method: str, params: dict) -> None:
        self.calls.append((method, params))

    def gamestate(self) -> dict:
        return {"step": len(self.calls)}


def test_action_mapping_uses_balatrobot_parameter_names() -> None:
    assert action_to_rpc(7) == ("pack", {"skip": True})
    assert action_to_rpc(9, primary=2) == ("sell", {"joker": 2})
    assert action_to_rpc(10, primary=1) == ("sell", {"consumable": 1})
    assert action_to_rpc(11, primary=1, selection=[0, 3]) == (
        "use", {"consumable": 1, "cards": [0, 3]}
    )
    assert action_to_rpc(12, primary=0) == ("buy", {"voucher": 0})
    assert action_to_rpc(13, primary=1) == ("buy", {"pack": 1})
    assert action_to_rpc(22) == ("reroll_boss", {})


def test_rearrange_mapping_requires_and_preserves_permutation() -> None:
    assert action_to_rpc(15, order=[2, 0, 1]) == ("rearrange", {"jokers": [2, 0, 1]})
    assert action_to_rpc(18, order=[1, 0]) == ("rearrange", {"hand": [1, 0]})
    assert action_to_rpc(19) == ("rearrange", {"sort": "rank"})
    assert action_to_rpc(20) == ("rearrange", {"sort": "suit"})

    try:
        action_to_rpc(15)
    except ValueError as error:
        assert "order" in str(error)
    else:
        raise AssertionError("rearrange without an order must be rejected")


def test_replay_executes_jsonl_records_and_collects_states() -> None:
    client = FakeClient()
    states = replay(client, [
        {"type": 2},
        {"type": 0, "selection": [0, 3]},
        {"type": 15, "order": [1, 0]},
    ])
    assert client.calls == [
        ("select", {}),
        ("play", {"cards": [0, 3]}),
        ("rearrange", {"jokers": [1, 0]}),
    ]
    assert states == [{"step": 1}, {"step": 2}, {"step": 3}]
