# Simulatro

A fast C simulator of Balatro with Python bindings.

Requires [Balatrobot](https://github.com/maubious/balatrobot) commit [`c5021af66b882480d355b953ca5224c176ebc3fe`](https://github.com/maubious/balatrobot/commit/c5021af66b882480d355b953ca5224c176ebc3fe).

## Observation space

The preferred training call is `balatro_observe_rl`, which emits a versioned,
8,331-byte `BalatroCompactObservation` plus `BalatroLegalMasks` in one call.
Variable-length areas are count-prefixed AoS sections, continuous values use
signed-log2 Q8.8, and redeemed vouchers are bit-packed. Only each live prefix
is written; capacity lanes after `count` are unspecified.

`BalatroObservation` remains as a zero-padded debug layout for tooling that
benefits from readable SoA fields. Capacity overflow returns
`BALATRO_ERR_OBSERVATION_CAPACITY` rather than silently dropping state.

| Section | Contents |
|---|---|
| `scalars` | Phase, ante, round, deck, stake, blind state, money, score, hand/discard counts, slots, reroll and economy state, pack state, tag state, run counters, rates, and redeemed-voucher mask. |
| `variants` | One row per distinct owned playing-card variant: rank, suit, enhancement, edition, seal, flags, permanent bonus, and counts in the owned deck, draw pile, hand, and discard pile. |
| `hand` | Current visible hand in display order. Each slot references a row in `variants` and carries public flags such as debuffed, forced, or face-down. |
| `owned_deck` | Rank, suit, rank-by-suit, enhancement, edition, seal, face/numbered/Ace, Stone/Wild/Steel/Gold/Glass, enhanced/unmodified, and total counts across all owned playing cards. |
| `draw_pile` | The same aggregate deck summary for cards still available to draw. Draw order is hidden. |
| `jokers`, `consumables` | Ordered cards with center ID, card modifiers, flags, prices, permanent bonus, and four raw plus transformed center-specific mutable values. |
| `shop`, `shop_vouchers`, `shop_boosters` | Visible shop inventory split into stable action-indexed areas. |
| `pack` | Visible pack choices, including card modifiers and mutable values. |
| `tags` | Held/pending tags and Small/Big Blind offers, including Orbital hand when relevant. |
| `poker_hands` | Visibility, level, chips, multiplier, total plays, and current-round plays for every hand type. |
| `legal` | Masks describing every currently legal action, target, card selection, and reorder destination. |

Large signed values use `sign(x) * log2(1 + abs(x))`. Score fields additionally include a finite, positive-infinity, negative-infinity, or NaN class, so tensor payloads remain finite. The public observation excludes hidden RNG streams and exact draw order; those remain in `BalatroState` for deterministic simulation and snapshots.

Planning tools that need enumeration can call `balatro_legal_expand` on those
masks. This optional path materializes `BalatroLegalView`; RL stepping never
builds or clears its 512-group storage.

## Action space

An agent submits `BalatroPolicyAction`:

- `type`: one of the actions below.
- `primary`: the indexed Joker, consumable, shop item, voucher, booster, pack card, or card being moved.
- `selection_count` and `selection[5]`: hand indices used by plays, discards, targeted consumables, buy-and-use, and pack choices.
- `reorder_destination`: reserved; current reorder actions encode left/right in `type` and use `primary` as the source slot.

| ID | Action | Parameters |
|---:|---|---|
| 0 | Play hand | `selection` = played hand cards |
| 1 | Discard | `selection` = discarded hand cards |
| 2 | Select blind | none |
| 3 | Skip blind | none |
| 4 | Cash out | none |
| 5 | Reroll shop | none |
| 6 | Leave shop / next round | none |
| 7 | Skip pack | none |
| 8 | Buy card | `primary` = `shop` slot |
| 9 | Sell Joker | `primary` = `jokers` slot |
| 10 | Sell consumable | `primary` = `consumables` slot |
| 11 | Use consumable | `primary` = consumable; optional hand `selection` |
| 12 | Redeem voucher | `primary` = `shop_vouchers` slot |
| 13 | Open booster | `primary` = `shop_boosters` slot |
| 14 | Pick pack card | `primary` = `pack` slot; optional hand `selection` |
| 15 | Move Joker left | `primary` = Joker slot |
| 16 | Move Joker right | `primary` = Joker slot |
| 17 | Move hand card left | `primary` = hand slot |
| 18 | Move hand card right | `primary` = hand slot |
| 19 | Sort hand by rank | none |
| 20 | Sort hand by suit | none |
| 21 | Buy and immediately use | `primary` = `shop` slot; optional hand `selection` |
| 22 | Reroll Boss Blind | none |

Use `legal.action_type[type]` before choosing a type. `legal.primary[type]` is a bit mask of valid `primary` slots; actions without a target use bit 0. For actions that select hand cards, the matching `play`, `discard`, `consumable[primary]`, `shop[primary]`, or `pack[primary]` entry gives `minimum`, `maximum`, `allowed_hand`, and `required_hand`. A selection must use an allowed bit, include every required bit, and satisfy the size range. Reorder masks give the reachable destination bits for each source slot.

Shop action indices are native area-local indices, so policy and state layout
agree without remapping. `balatro_step_observe_rl` applies a policy action and
returns the next compact observation and masks; trusted and batch equivalents
are available. `balatro_score_plays` evaluates scoring-time effects in an
isolated sandbox and skips draw, cash-out, and other post-hand transitions.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Live heuristic through BalatroBot

With Balatro running the pinned BalatroBot mod, the heuristic can drive a
seeded live game while keeping the native simulator as a checked shadow state:

```sh
PYTHONPATH=python python tools/heuristic_baseline.py \
  --balatrobot http://127.0.0.1:12346 \
  --library build/libbalatro.so \
  --live-deck RED --stake WHITE --seed HEURISTIC1 --trace
```

Omit `--seed` for a random live seed and omit the URL to use the default
BalatroBot endpoint. The hook stops with a diagnostic if the live and native
states diverge. The bridge also exposes `run_policy(client, callback)` for
other action-record policies.
