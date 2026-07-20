"""A deterministic, game-aware heuristic baseline.

The policy is intentionally still a heuristic (and uses no learned weights),
but it treats a run as more than a sequence of independent poker hands.  It
looks one round ahead when spending money, values the contents of packs, uses
consumables, replaces weak Jokers, and spends discards when they improve the
best available hand.
"""

from __future__ import annotations

import argparse
import base64
import ctypes as C
import hashlib
import json
import math
from pathlib import Path
import sys
from typing import Any

from simulatro.core import Action, BalatroCore, Config, State
try:
    from tools.balatrobot_bridge import BalatroBotClient, run_policy
except ModuleNotFoundError:  # Direct ``python tools/heuristic_baseline.py`` invocation.
    from balatrobot_bridge import BalatroBotClient, run_policy


PLAY, DISCARD, SELECT, SKIP_BLIND, CASH_OUT = 0, 1, 2, 3, 4
REROLL, NEXT, SKIP_PACK, BUY, SELL_JOKER = 5, 6, 7, 8, 9
USE, VOUCHER, OPEN, PICK, BUY_AND_USE = 11, 12, 13, 14, 21
MOVE_JOKER_LEFT, MOVE_JOKER_RIGHT = 15, 16

JOKER_MIN, JOKER_MAX = 75, 224
PLANETS = {21, 27, 31, 43, 47, 49, 51, 53, 54, 55, 65, 66}
PLANET_HAND = {
    31: 0, 21: 1, 53: 2, 51: 3, 47: 4, 27: 5,
    43: 6, 55: 7, 66: 8, 65: 9, 49: 10, 54: 11,
}

# Effects which are useful to a short Ante-8 run even when their benefit is
# not visible in the very next hand preview.
HIGH_VALUE_VOUCHERS = {
    268, 270, 271, 274, 278, 281, 284, 285, 286, 287, 291, 295, 298, 299,
}

PREMIUM_SKIP_TAGS = {12, 16, 18, 19, 23}  # Holographic, Negative, Polychrome, Rare, Uncommon

DECK_IDS = {
    "ABANDONED": 1, "ANAGLYPH": 2, "BLACK": 3, "BLUE": 4,
    "CHECKERED": 6, "ERRATIC": 7, "GHOST": 8, "GREEN": 9,
    "MAGIC": 10, "NEBULA": 11, "PAINTED": 12, "PLASMA": 13,
    "RED": 14, "YELLOW": 15, "ZODIAC": 16,
}
STAKE_IDS = {
    "WHITE": 1, "RED": 2, "GREEN": 3, "BLACK": 4,
    "BLUE": 5, "PURPLE": 6, "ORANGE": 7, "GOLD": 8,
}
LIVE_PHASES = {
    "BLIND_SELECT": 0, "SELECTING_HAND": 1, "ROUND_EVAL": 2,
    "SHOP": 3, "SMODS_BOOSTER_OPENED": 4, "GAME_OVER": 5,
}

TRACE_CHECKPOINT_PHASES = {"SELECTING_HAND", "SHOP"}


def clone(state: State) -> State:
    return State.from_buffer_copy(bytes(state))


def action_record(state: State, action: Action) -> dict[str, Any]:
    """Convert a native action to BalatroBot's area-local action indices."""
    record: dict[str, Any] = {
        "type": int(action.type),
        "primary": int(action.primary),
        "selection": [int(action.selection[i]) for i in range(action.selection_count)],
    }
    if action.type in (MOVE_JOKER_LEFT, MOVE_JOKER_RIGHT, 17, 18):
        count = int(state.joker_count if action.type in (MOVE_JOKER_LEFT, MOVE_JOKER_RIGHT) else state.hand_count)
        order = list(range(count))
        other = int(action.primary) + (-1 if action.type in (MOVE_JOKER_LEFT, 17) else 1)
        order[int(action.primary)], order[other] = order[other], order[int(action.primary)]
        record["order"] = order
    return record


def _native_trace_state(state: State) -> dict[str, Any]:
    """Return a lossless ABI snapshot with a small readable summary."""
    raw = bytes(state)
    return {
        "state_size": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "snapshot": base64.b64encode(raw).decode("ascii"),
        "summary": {
            "phase": int(state.phase),
            "ante": int(state.ante),
            "round": int(state.round),
            "money": int(state.dollars),
            "chips": float(state.chips),
            "blind_chips": float(state.blind_chips),
            "hands_left": int(state.hands_left),
            "discards_left": int(state.discards_left),
            "deck": int(state.deck_count),
            "hand": int(state.hand_count),
            "jokers": int(state.joker_count),
            "consumables": int(state.consumable_count),
            "shop": int(state.shop_main_count + state.shop_voucher_count + state.shop_booster_count),
            "pack": int(state.pack_count),
            "actions": int(state.actions_taken),
            "terminal": bool(state.terminal),
            "won": bool(state.won),
        },
    }


def _windows_path(path: Path) -> str:
    resolved = path.resolve()
    parts = resolved.parts
    if len(parts) >= 4 and parts[1] == "mnt" and len(parts[2]) == 1:
        return f"{parts[2].upper()}:/{'/'.join(parts[3:])}"
    return str(resolved)


def _trace_path(requested: bool | str | Path, seed: str) -> Path:
    if requested is not True and str(requested):
        return Path(requested)
    safe_seed = "".join(character if character.isalnum() or character in "-_" else "_"
                        for character in seed) or "live"
    candidate = Path("traces") / f"heuristic_{safe_seed}.json"
    suffix = 2
    while candidate.exists():
        candidate = Path("traces") / f"heuristic_{safe_seed}_{suffix}.json"
        suffix += 1
    return candidate


def _write_trace(path: Path, trace: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(trace, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def _center_for_action(state: State, action: Action) -> int:
    if action.type in (BUY, BUY_AND_USE):
        return int(state.shop_main[action.primary].center_id)
    if action.type == VOUCHER:
        return int(state.shop_vouchers[action.primary].center_id)
    if action.type == OPEN:
        return int(state.shop_boosters[action.primary].center_id)
    if action.type == PICK:
        return int(state.pack_cards[action.primary].center_id)
    if action.type == USE:
        return int(state.consumables[action.primary].center_id)
    return 0


def _safe_number(value: float) -> float:
    return -math.inf if math.isnan(value) else value


def _best_play(core: BalatroCore, state: State, legal: list[Action] | None = None) -> tuple[Action | None, float]:
    legal = legal if legal is not None else core.legal_actions(state)
    plays = [action for action in legal if action.type == PLAY]
    if not plays:
        return None, -math.inf
    # The native scorer clones internally and is substantially cheaper than a
    # Python State copy and complete transition for every subset.
    scores = core.trusted_play_scores(state, plays)
    index = max(
        range(len(plays)),
        key=lambda i: (_safe_number(float(scores[i])), plays[i].selection_count),
    )
    return plays[index], _safe_number(float(scores[index]))


def _advance_to_hand(core: BalatroCore, state: State) -> State | None:
    """Clone and advance a non-combat state to the next dealt hand."""
    trial = clone(state)
    for _ in range(12):
        if trial.terminal:
            return None
        legal = core.legal_actions(trial)
        if not legal:
            return None
        if trial.phase == 1:
            return trial
        if trial.phase == 0:
            action = next((a for a in legal if a.type == SELECT), legal[0])
        elif trial.phase == 2:
            action = next((a for a in legal if a.type == CASH_OUT), legal[0])
        elif trial.phase == 3:
            action = next((a for a in legal if a.type == NEXT), legal[0])
        elif trial.phase == 4:
            action = next((a for a in legal if a.type == SKIP_PACK), legal[0])
        else:
            return None
        core.step(trial, action)
    return None


def _combat_value(core: BalatroCore, state: State) -> tuple[float, float, float]:
    """Estimate next-round safety, returning a lexicographically sorted key."""
    hand = state if state.phase == 1 else _advance_to_hand(core, state)
    if hand is None:
        return (-math.inf, -math.inf, -math.inf)
    _, score = _best_play(core, hand)
    remaining = max(1.0, float(hand.blind_chips) - float(hand.chips))
    score = max(0.0, score)
    # A one-shot is especially valuable (money and safety), followed by the
    # conservative estimate that every remaining hand scores this much.
    one_shot = score / remaining
    round_cover = score * max(1, int(hand.hands_left)) / remaining
    return (min(one_shot, 1.0), min(round_cover, 2.0), math.log1p(score))


def _trial_value(core: BalatroCore, state: State, action: Action) -> tuple[float, float, float]:
    trial = clone(state)
    try:
        core.step(trial, action)
    except RuntimeError:
        return (-math.inf, -math.inf, -math.inf)
    return _combat_value(core, trial)


def _planet_priority(state: State, center: int) -> tuple[int, int]:
    hand = PLANET_HAND.get(center, -1)
    if hand < 0:
        return (0, 0)
    return (int(state.hand_plays[hand]), -int(state.hand_levels[hand]))


def _planet_matches_build(state: State, center: int) -> bool:
    """Only pay for levels supporting an established, repeatable hand."""
    hand = PLANET_HAND.get(center, -1)
    if hand < 0 or not state.hand_plays[hand]:
        return False
    most = max(int(state.hand_plays[i]) for i in range(len(state.hand_plays)))
    # Early on, Pair/High Card are dependable fallbacks; later, follow the
    # actual most-played hand instead of diluting money across twelve levels.
    return int(state.hand_plays[hand]) == most or (
        hand in (10, 11) and int(state.hand_plays[hand]) + 1 >= most
    )


def _choose_discard(core: BalatroCore, state: State, legal: list[Action], current_score: float) -> Action | None:
    if not state.discards_left:
        return None
    discards = [a for a in legal if a.type == DISCARD]
    if not discards:
        return None

    # Drawing more cards dominates most small discard subsets.  Retain all
    # maximum-size choices, plus choices which preserve an existing pair or
    # better through their different selection masks.
    largest = max(a.selection_count for a in discards)
    candidates = [a for a in discards if a.selection_count == largest]
    if len(candidates) > 80:
        candidates = candidates[:80]

    best: tuple[tuple[float, float], Action] | None = None
    for action in candidates:
        trial = clone(state)
        try:
            core.step(trial, action)
        except RuntimeError:
            continue
        _, next_score = _best_play(core, trial)
        key = (next_score, -float(action.selection_count))
        if best is None or key > best[0]:
            best = (key, action)
    if best is None:
        return None

    remaining = max(0.0, float(state.blind_chips) - float(state.chips))
    # Never discard a hand that wins now.  Otherwise accept a modest
    # improvement; late in a round, only a material improvement is worth the
    # draw because the current hand still contributes to the target.
    if current_score >= remaining:
        return None
    threshold = 1.08 if state.hands_left > 1 else 1.25
    if best[0][0] > current_score * threshold:
        return best[1]
    return None


def _best_pack_action(core: BalatroCore, state: State, legal: list[Action]) -> Action:
    choices = [a for a in legal if a.type in (PICK, USE)]
    if not choices:
        return next((a for a in legal if a.type == SKIP_PACK), legal[0])

    def key(action: Action) -> tuple[float, ...]:
        center = _center_for_action(state, action)
        value = _trial_value(core, state, action)
        is_joker = int(JOKER_MIN <= center <= JOKER_MAX)
        planet = _planet_priority(state, center)
        matching_planet = int(center in PLANETS and _planet_matches_build(state, center))
        edition = 0
        if action.type == PICK:
            edition = int(state.pack_cards[action.primary].edition)
        # Combat preview captures Jokers immediately; planet priority handles
        # a picked consumable which will only be used after the pack closes.
        return (*value, is_joker, matching_planet, planet[0], planet[1], edition, -action.primary)

    return max(choices, key=key)


def _best_joker_purchase(core: BalatroCore, state: State, legal: list[Action]) -> tuple[Action | None, tuple[float, float, float]]:
    purchases = [
        a for a in legal
        if a.type == BUY and JOKER_MIN <= _center_for_action(state, a) <= JOKER_MAX
    ]
    if not purchases:
        return None, (-math.inf, -math.inf, -math.inf)
    action = max(
        purchases,
        key=lambda a: (_trial_value(core, state, a), -int(state.shop_main[a.primary].cost)),
    )
    return action, _trial_value(core, state, action)


def _shop_action(core: BalatroCore, state: State, legal: list[Action]) -> Action:
    baseline = _combat_value(core, state)

    # Joker order is part of the build: flat chips/mult should generally fire
    # before xMult, and copier placement can be decisive. Hill-climb adjacent
    # legal moves using the same dealt-hand preview.
    moves = [a for a in legal if a.type in (MOVE_JOKER_LEFT, MOVE_JOKER_RIGHT)]
    if moves:
        best_move = max(moves, key=lambda a: _trial_value(core, state, a))
        if _trial_value(core, state, best_move) > baseline:
            return best_move

    # Free inventory actions first. Planets are always converted into a level;
    # other no-target consumables are taken when their preview is non-worse.
    uses = [a for a in legal if a.type == USE]
    if uses:
        planets = [a for a in uses if _center_for_action(state, a) in PLANETS]
        if planets:
            return max(planets, key=lambda a: _planet_priority(state, _center_for_action(state, a)))
        useful = max(uses, key=lambda a: _trial_value(core, state, a))
        if _trial_value(core, state, useful) >= baseline:
            return useful

    immediate = [a for a in legal if a.type == BUY_AND_USE]
    if immediate:
        planets = [a for a in immediate if _center_for_action(state, a) in PLANETS]
        planets = [a for a in planets if _planet_matches_build(state, _center_for_action(state, a))]
        if planets and state.dollars >= 15:
            return max(planets, key=lambda a: _planet_priority(state, _center_for_action(state, a)))

    joker, joker_value = _best_joker_purchase(core, state, legal)
    if joker is not None:
        # Fill empty slots aggressively. Once a build exists, preserve the
        # interest floor unless the purchase materially improves next round.
        price = int(state.shop_main[joker.primary].cost)
        improves = joker_value > baseline and joker_value[2] > baseline[2] + 0.05
        if state.joker_count < min(3, state.joker_slots) or improves or state.dollars - price >= 25:
            return joker

    # At a full rack, look through every legal sale and buy combination. This
    # prevents an early utility Joker from permanently blocking a scoring one.
    if state.joker_count >= state.joker_slots:
        best_swap: tuple[tuple[float, float, float], Action] | None = None
        for sell in (a for a in legal if a.type == SELL_JOKER):
            trial = clone(state)
            core.step(trial, sell)
            buy, value = _best_joker_purchase(core, trial, core.legal_actions(trial))
            if buy is not None and (best_swap is None or value > best_swap[0]):
                best_swap = (value, sell)
        if best_swap is not None and best_swap[0][2] > baseline[2] + 0.10:
            return best_swap[1]

    vouchers = [a for a in legal if a.type == VOUCHER]
    if vouchers:
        best = max(vouchers, key=lambda a: (_trial_value(core, state, a), _center_for_action(state, a) in HIGH_VALUE_VOUCHERS))
        center = _center_for_action(state, best)
        price = int(state.shop_vouchers[best.primary].cost)
        if center in HIGH_VALUE_VOUCHERS and (state.dollars - price >= 10 or center in {274, 281, 286, 287, 299}):
            return best

    # Buffoon packs efficiently fill an incomplete rack. Celestial packs are
    # speculative until opened, so only buy them from a healthy cash reserve.
    packs = [a for a in legal if a.type == OPEN]
    if packs:
        buffoon = [a for a in packs if 241 <= _center_for_action(state, a) <= 244]
        if buffoon and state.joker_count < min(3, state.joker_slots) and state.dollars >= 15:
            return min(buffoon, key=lambda a: int(state.shop_boosters[a.primary].cost))
        celestial = [a for a in packs if 245 <= _center_for_action(state, a) <= 252]
        if celestial and state.dollars >= 22 and baseline[1] < 1.5:
            return min(celestial, key=lambda a: int(state.shop_boosters[a.primary].cost))

    # Reroll only while the build cannot conservatively cover the next blind.
    reroll = next((a for a in legal if a.type == REROLL), None)
    if reroll is not None and state.dollars - state.reroll_cost >= 15 and baseline[1] < 1.15:
        return reroll

    return next((a for a in legal if a.type == NEXT), legal[0])


def choose(core: BalatroCore, state: State, legal: list[Action]) -> Action:
    if state.phase == 0:
        skip = next((a for a in legal if a.type == SKIP_BLIND), None)
        if skip is not None and state.blind_on_deck < 2:
            tag = int(state.blind_tags[state.blind_on_deck])
            should_skip = tag in PREMIUM_SKIP_TAGS
            should_skip |= tag == 13 and state.ante <= 2  # Investment
            should_skip |= tag in {2, 22} and state.joker_count < min(3, state.joker_slots)
            if tag == 17:  # Orbital: three levels for a named poker hand.
                hand = int(state.orbital_hands[state.blind_on_deck])
                most = max(int(state.hand_plays[i]) for i in range(len(state.hand_plays)))
                should_skip |= bool(state.hand_plays[hand] and state.hand_plays[hand] == most)
            if should_skip:
                return skip
        return next((a for a in legal if a.type == SELECT), legal[0])
    if state.phase == 1:
        play, score = _best_play(core, state, legal)
        if play is not None:
            # Use planets immediately and targeted consumables when the best
            # legal target improves the current hand. This also makes Tarot
            # packs useful instead of merely occupying consumable slots.
            uses = [a for a in legal if a.type == USE]
            planets = [a for a in uses if _center_for_action(state, a) in PLANETS]
            if planets:
                return max(planets, key=lambda a: _planet_priority(state, _center_for_action(state, a)))
            if uses:
                best_use: tuple[float, Action] | None = None
                for action in uses:
                    trial = clone(state)
                    try:
                        core.step(trial, action)
                    except RuntimeError:
                        continue
                    _, trial_score = _best_play(core, trial)
                    if best_use is None or trial_score > best_use[0]:
                        best_use = (trial_score, action)
                if best_use is not None and best_use[0] > score * 1.02:
                    return best_use[1]
            discard = _choose_discard(core, state, legal, score)
            return discard if discard is not None else play
        discards = [a for a in legal if a.type == DISCARD]
        if discards:
            return discards[0]
    if state.phase == 2:
        return next((a for a in legal if a.type == CASH_OUT), legal[0])
    if state.phase == 3:
        return _shop_action(core, state, legal)
    if state.phase == 4:
        return _best_pack_action(core, state, legal)
    return legal[0]


def _verify_live_state(native: State, live: dict[str, Any]) -> None:
    """Fail early if the live run no longer matches its native shadow."""
    phase_name = str(live.get("state", "UNKNOWN"))
    expected_phase = LIVE_PHASES.get(phase_name)
    checks = {
        "phase": (int(native.phase), expected_phase),
        "ante": (int(native.ante), live.get("ante_num")),
        "round": (int(native.round), live.get("round_num")),
        "money": (int(native.dollars), live.get("money")),
        "jokers": (int(native.joker_count), (live.get("jokers") or {}).get("count")),
        "consumables": (int(native.consumable_count), (live.get("consumables") or {}).get("count")),
    }
    live_joker_cards = (live.get("jokers") or {}).get("cards")
    if live_joker_cards is not None and len(live_joker_cards) == native.joker_count:
        checks["joker_debuffs"] = (
            [bool(native.jokers[index].flags & 1) for index in range(native.joker_count)],
            [bool((card.get("state") or {}).get("debuff"))
             if isinstance(card.get("state"), dict) else False
             for card in live_joker_cards],
        )
    round_state = live.get("round") or {}
    if phase_name == "SELECTING_HAND":
        checks.update({
            "chips": (float(native.chips), round_state.get("chips")),
            "hands_left": (int(native.hands_left), round_state.get("hands_left")),
            "discards_left": (int(native.discards_left), round_state.get("discards_left")),
            "hand": (int(native.hand_count), (live.get("hand") or {}).get("count")),
        })
    mismatches = {
        name: {"native": native_value, "live": live_value}
        for name, (native_value, live_value) in checks.items()
        if live_value is not None and native_value != live_value
    }
    if mismatches:
        raise RuntimeError(f"BalatroBot/native state diverged: {json.dumps(mismatches, sort_keys=True)}")


def play_balatrobot(
    core: BalatroCore,
    client: BalatroBotClient,
    config: Config,
    *,
    deck: str = "RED",
    stake: str = "WHITE",
    seed: str | None = None,
    max_steps: int = 20_000,
    trace: bool | str | Path = False,
) -> dict[str, Any]:
    """Start and play one live BalatroBot run using a native shadow state."""
    deck = deck.upper()
    stake = stake.upper()
    if deck not in DECK_IDS:
        raise ValueError(f"unknown Balatro deck {deck!r}")
    if stake not in STAKE_IDS:
        raise ValueError(f"unknown Balatro stake {stake!r}")

    client.call("menu", {})
    start_params: dict[str, Any] = {"deck": deck, "stake": stake}
    if seed:
        start_params["seed"] = seed
    live = client.call("start", start_params)
    if not isinstance(live, dict) or not live.get("seed"):
        raise RuntimeError("BalatroBot start did not return a seeded game state")

    if config.observation.playing_cards == 0:
        default_config = getattr(getattr(core, "lib", None), "balatro_default_config", None)
        if default_config is not None:
            default_config(C.byref(config))
    config.deck = DECK_IDS[deck]
    config.stake = STAKE_IDS[stake]
    config.win_ante = 8  # Standard live Balatro runs always finish at Ante 8.
    native = core.reset_seed_string(str(live["seed"]), config)
    pending: Action | None = None
    pending_record: dict[str, Any] | None = None
    pending_pre_live: dict[str, Any] | None = None
    pending_pre_native: dict[str, Any] | None = None
    trace_path = _trace_path(trace, str(live["seed"])) if trace else None
    trace_data: dict[str, Any] | None = None
    checkpoint_antes: set[int] = set()

    if trace_path is not None:
        trace_data = {
            "status": "running",
            "seed": str(live["seed"]),
            "deck": deck,
            "stake": stake,
            "endpoint": client.endpoint,
            "initial": {
                "live": live,
                "native": _native_trace_state(native),
            },
            "baseline_actions": [],
            "transitions": [],
            "checkpoints": [],
        }
        _write_trace(trace_path, trace_data)
        print(f"trace: {trace_path}", file=sys.stderr, flush=True)

    def persist_checkpoint(current: dict[str, Any]) -> None:
        if trace_path is None or trace_data is None:
            return
        phase = str(current.get("state", "UNKNOWN"))
        ante = int(current.get("ante_num", 0))
        if phase not in TRACE_CHECKPOINT_PHASES or ante in checkpoint_antes:
            return
        checkpoint_antes.add(ante)
        number = len(trace_data["checkpoints"]) + 1
        directory = trace_path.parent / f"{trace_path.stem}_checkpoints"
        path = directory / f"{trace_path.stem}_checkpoint_{number:03d}.jkr"
        path.parent.mkdir(parents=True, exist_ok=True)
        client.call("save", {"path": _windows_path(path)})
        if not path.exists():
            raise RuntimeError(f"BalatroBot reported a save but did not create {path}")
        trace_data["checkpoints"].append({
            "number": number,
            "path": str(path.resolve()),
            "state": phase,
            "ante": ante,
            "round": int(current.get("round_num", 0)),
            "baseline_actions": list(trace_data["baseline_actions"]),
            "native": _native_trace_state(native),
        })
        _write_trace(trace_path, trace_data)

    try:
        _verify_live_state(native, live)
        persist_checkpoint(live)
    except BaseException as error:
        if trace_path is not None and trace_data is not None:
            trace_data["status"] = (
                "diverged" if str(error).startswith("BalatroBot/native state diverged:")
                else "interrupted" if isinstance(error, KeyboardInterrupt)
                else "error"
            )
            trace_data["stage"] = "start"
            trace_data["error"] = f"{type(error).__name__}: {error}"
            trace_data["final"] = {"live": live, "native": _native_trace_state(native)}
            _write_trace(trace_path, trace_data)
        raise

    def policy(current: dict[str, Any]) -> dict[str, Any]:
        nonlocal pending, pending_record, pending_pre_live, pending_pre_native
        legal = [a for a in core.legal_actions(native) if a.type != BUY_AND_USE]
        if not legal:
            raise RuntimeError("native shadow exposed no BalatroBot-compatible action")
        pending = choose(core, native, legal)
        pending_record = action_record(native, pending)
        pending_pre_live = current
        pending_pre_native = _native_trace_state(native) if trace_data is not None else None
        return pending_record

    def stepped(record: dict[str, Any], current: dict[str, Any]) -> None:
        nonlocal pending, pending_record, pending_pre_live, pending_pre_native
        if pending is None:
            raise RuntimeError("BalatroBot step completed without a pending native action")
        core.step(native, pending)
        if trace_data is not None:
            trace_data["baseline_actions"].append(record)
            trace_data["transitions"].append({
                "step": len(trace_data["baseline_actions"]),
                "action": record,
                "pre_live": pending_pre_live,
                "pre_native": pending_pre_native,
                "live": current,
                "native": _native_trace_state(native),
            })
            _write_trace(trace_path, trace_data)  # type: ignore[arg-type]
        try:
            _verify_live_state(native, current)
        except BaseException as error:
            if trace_data is not None:
                trace_data["status"] = "diverged"
                trace_data["stage"] = "baseline"
                trace_data["step"] = len(trace_data["baseline_actions"])
                trace_data["action"] = record
                trace_data["error"] = f"{type(error).__name__}: {error}"
                trace_data["final"] = {
                    "live": current,
                    "native": _native_trace_state(native),
                }
                _write_trace(trace_path, trace_data)  # type: ignore[arg-type]
            raise
        persist_checkpoint(current)
        if trace_data is not None:
            print(json.dumps({"action": record, "ante": int(native.ante),
                              "phase": int(native.phase), "money": int(native.dollars)}))
        pending = None
        pending_record = None
        pending_pre_live = None
        pending_pre_native = None

    try:
        final = run_policy(client, policy, state=live, max_steps=max_steps, on_step=stepped)
    except BaseException as error:
        if trace_path is not None and trace_data is not None and trace_data["status"] == "running":
            trace_data["status"] = (
                "interrupted" if isinstance(error, KeyboardInterrupt) else "error"
            )
            trace_data["stage"] = "baseline"
            trace_data["step"] = len(trace_data["baseline_actions"]) + 1
            trace_data["attempted_action"] = pending_record
            trace_data["pre_live"] = pending_pre_live
            trace_data["pre_native"] = pending_pre_native
            trace_data["error"] = f"{type(error).__name__}: {error}"
            _write_trace(trace_path, trace_data)
        raise
    result = {
        "seed": str(live["seed"]),
        "won": bool(final.get("won", native.won)),
        "ante": int(final.get("ante_num", native.ante)),
        "round": int(final.get("round_num", native.round)),
        "money": int(final.get("money", native.dollars)),
        "state": final.get("state"),
        "actions": int(native.actions_taken),
    }
    if trace_path is not None and trace_data is not None:
        trace_data["status"] = "matched"
        trace_data["outcome"] = "won" if result["won"] else "game_over"
        trace_data["baseline_steps"] = len(trace_data["baseline_actions"])
        trace_data["final"] = {"live": final, "native": _native_trace_state(native)}
        _write_trace(trace_path, trace_data)
        result["trace"] = str(trace_path)
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--games", type=int, default=100)
    parser.add_argument("--steps", type=int, default=20000)
    parser.add_argument("--seed-start", type=int, default=1,
                        help="First numeric seed in the evaluation range (default: 1).")
    parser.add_argument("--deck", type=int, default=14,
                        help="Deck center ID (default: 14, Red Deck).")
    parser.add_argument("--balatrobot", nargs="?", const="http://127.0.0.1:12346",
                        help="Play one live run through BalatroBot at the optional endpoint.")
    parser.add_argument("--live-deck", choices=sorted(DECK_IDS), default="RED")
    parser.add_argument("--stake", choices=sorted(STAKE_IDS), default="WHITE")
    parser.add_argument("--seed", help="Optional Balatro seed for --balatrobot mode.")
    parser.add_argument("--timeout", type=float, default=15.0,
                        help="BalatroBot request timeout in seconds (default: 15).")
    parser.add_argument(
        "--trace", nargs="?", const=True, metavar="PATH",
        help="Persist a full live trace (default: traces/heuristic_<seed>.json).",
    )
    parser.add_argument("--win-ante", type=int, default=8,
                        help="Terminal win ante used for the baseline (default: 8).")
    parser.add_argument("--library", type=Path)
    args = parser.parse_args()
    core = BalatroCore(args.library)
    config = Config()
    core.lib.balatro_default_config(C.byref(config))
    config.win_ante = max(1, min(255, args.win_ante))
    if args.balatrobot:
        client = BalatroBotClient(args.balatrobot, timeout=args.timeout)
        result = play_balatrobot(
            core, client, config, deck=args.live_deck, stake=args.stake,
            seed=args.seed, max_steps=args.steps, trace=args.trace,
        )
        print(json.dumps(result, sort_keys=True))
        return 0

    config.deck = max(1, min(16, args.deck))
    wins = 0
    antes: list[int] = []
    steps: list[int] = []
    errors = 0
    terminals = 0
    total_reward = 0.0
    action_counts: dict[str, int] = {}
    for offset in range(args.games):
        state = core.reset(args.seed_start + offset, config)
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
    print(json.dumps({"games": args.games, "seed_start": args.seed_start,
                      "deck": int(config.deck),
                      "win_ante": int(config.win_ante), "wins": wins,
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
