"""Deterministic legal-action baseline for sanity-checking the game wrapper.

This is deliberately not a learned policy.  It greedily selects the legal
playing subset that leaves the most chips, takes the first sensible shop/pack
choice, and records both ante progress and full wins.
"""

from __future__ import annotations

import argparse
import ctypes as C
import json
import math
import os
from pathlib import Path

from simulatro.core import Action, BalatroCore, Config, State


PLAY, SELECT, CASH_OUT, NEXT, BUY, USE, OPEN, PICK, SKIP_PACK = 0, 2, 4, 6, 8, 11, 13, 14, 7


def clone(state: State) -> State:
    return State.from_buffer_copy(bytes(state))


def choose(core: BalatroCore, state: State, legal: list[Action]) -> Action:
    if state.phase == 0:
        return next((a for a in legal if a.type == SELECT), legal[0])
    if state.phase == 1:
        plays = [a for a in legal if a.type == PLAY]
        if plays:
            best = None
            for action in plays:
                trial = clone(state)
                try:
                    result = core.step(trial, action)
                except RuntimeError:
                    continue
                chips = float(trial.chips)
                key = (int(result.terminal and result.won),
                       -math.inf if math.isnan(chips) else chips,
                       action.selection_count, -int(result.terminal))
                if best is None or key > best[0]:
                    best = (key, action)
            if best is not None:
                return best[1]
        discards = [a for a in legal if a.type == 1]
        if discards:
            return discards[0]
    if state.phase == 2:
        # ROUND_EVAL exposes CASH_OUT; NEXT_ROUND is the shop action.
        return next((a for a in legal if a.type == CASH_OUT), legal[0])
    if state.phase == 3:
        affordable = [a for a in legal if a.type == BUY]
        if affordable and state.joker_count < state.joker_slots:
            return affordable[0]
        return next((a for a in legal if a.type == NEXT), legal[0])
    if state.phase == 4:
        return next((a for a in legal if a.type in (PICK, USE)), legal[0])
    return legal[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--games", type=int, default=100)
    parser.add_argument("--steps", type=int, default=20000)
    parser.add_argument("--win-ante", type=int, default=8,
                        help="Terminal win ante used for the baseline (default: 8).")
    parser.add_argument("--library", type=Path)
    args = parser.parse_args()
    core = BalatroCore(args.library)
    config = Config()
    core.lib.balatro_default_config(C.byref(config))
    config.win_ante = max(1, min(255, args.win_ante))
    wins = 0
    antes: list[int] = []
    steps: list[int] = []
    errors = 0
    terminals = 0
    total_reward = 0.0
    action_counts: dict[str, int] = {}
    for seed in range(args.games):
        state = core.reset(seed + 1, config)
        for step in range(args.steps):
            legal = core.legal_actions(state)
            if not legal:
                break
            try:
                action = choose(core, state, legal)
                action_counts[str(action.type)] = action_counts.get(str(action.type), 0) + 1
                result = core.step(state, action)
                total_reward += float(result.reward)
            except RuntimeError:
                errors += 1
                antes.append(int(state.ante))
                steps.append(step + 1)
                break
            if result.terminal:
                terminals += 1
                wins += int(result.won)
                antes.append(int(result.ante))
                steps.append(step + 1)
                break
        else:
            antes.append(int(state.ante))
            steps.append(args.steps)
    print(json.dumps({"games": args.games, "win_ante": int(config.win_ante), "wins": wins,
                      "win_rate": wins / args.games if args.games else 0.0,
                      "mean_ante": sum(antes) / len(antes) if antes else 0.0,
                      "max_ante": max(antes, default=0),
                      "mean_steps": sum(steps) / len(steps) if steps else 0.0,
                      "terminal_rate": terminals / args.games if args.games else 0.0,
                      "mean_reward": total_reward / args.games if args.games else 0.0,
                      "action_counts": action_counts, "errors": errors},
                     sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
