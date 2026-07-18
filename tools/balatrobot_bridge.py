"""Small, dependency-free BalatroBot JSON-RPC bridge.

The live adapter is intentionally kept separate from the simulator ABI.  It
can execute a recorded RPC action stream for replay/inspection, or poll a
running BalatroBot endpoint while a caller supplies a policy callback.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Callable, Iterable


@dataclass
class BalatroBotClient:
    endpoint: str = "http://127.0.0.1:12346"
    timeout: float = 5.0
    _request_id: int = 0

    def call(self, method: str, params: dict[str, Any] | None = None) -> Any:
        self._request_id += 1
        payload = {"jsonrpc": "2.0", "method": method, "id": self._request_id}
        if params:
            payload["params"] = params
        request = urllib.request.Request(
            self.endpoint,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                result = json.loads(response.read().decode("utf-8"))
        except (urllib.error.URLError, TimeoutError) as exc:
            raise RuntimeError(f"BalatroBot unavailable at {self.endpoint}: {exc}") from exc
        if "error" in result:
            raise RuntimeError(f"BalatroBot {method} failed: {result['error']}")
        return result.get("result")

    def health(self) -> Any:
        return self.call("health")

    def gamestate(self) -> dict[str, Any]:
        return self.call("gamestate")


def action_to_rpc(action_type: int, *, selection: Iterable[int] = (), primary: int = 0,
                  order: Iterable[int] | None = None) -> tuple[str, dict[str, Any]]:
    """Translate a BalatroAction enum into a BalatroBot method/params pair."""
    selected = list(selection)
    table = {
        0: ("play", {"cards": selected}),
        1: ("discard", {"cards": selected}),
        2: ("select", {}),
        3: ("skip", {}),
        4: ("cash_out", {}),
        5: ("reroll", {}),
        6: ("next_round", {}),
        7: ("pack", {"skip": True}),
        8: ("buy", {"card": primary}),
        9: ("sell", {"joker": primary}),
        10: ("sell", {"consumable": primary}),
        11: ("use", {"consumable": primary, "cards": selected}),
        12: ("buy", {"voucher": primary}),
        13: ("buy", {"pack": primary}),
        14: ("pack", {"card": primary, "targets": selected}),
        22: ("reroll_boss", {}),
    }
    if action_type in (19, 20):
        return "rearrange", {"sort": "rank" if action_type == 19 else "suit"}
    if action_type in (15, 16, 17, 18):
        if order is None:
            raise ValueError("rearrange actions require the full post-action order")
        field = "jokers" if action_type in (15, 16) else "hand"
        return "rearrange", {field: list(order)}
    try:
        return table[action_type]
    except KeyError as exc:
        raise ValueError(f"unsupported live action type {action_type}") from exc


def replay(client: BalatroBotClient, actions: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    """Execute JSON action records and return state snapshots after each call."""
    states: list[dict[str, Any]] = []
    for record in actions:
        method, params = action_to_rpc(
            int(record["type"]), selection=record.get("selection", ()),
            primary=int(record.get("primary", 0)), order=record.get("order"),
        )
        client.call(method, params)
        states.append(client.gamestate())
    return states


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--endpoint", default="http://127.0.0.1:12346")
    parser.add_argument("--health", action="store_true")
    parser.add_argument("--actions", type=argparse.FileType("r"),
                        help="JSONL BalatroAction records to replay")
    args = parser.parse_args()
    client = BalatroBotClient(args.endpoint)
    if args.health or args.actions is None:
        print(json.dumps(client.health(), sort_keys=True))
    if args.actions is not None:
        records = (json.loads(line) for line in args.actions if line.strip())
        print(json.dumps(replay(client, records), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
