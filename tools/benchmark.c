#include "balatro.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct BenchStats {
    unsigned episodes;
    unsigned steps;
    unsigned scored_hands;
    unsigned shops;
    unsigned packs;
    unsigned buys;
    unsigned rerolls;
    unsigned discards;
    unsigned consumable_uses;
    unsigned sells;
    unsigned vouchers;
    unsigned skipped_blinds;
    unsigned selected_blinds;
    unsigned skipped_packs;
    unsigned next_rounds;
    unsigned terminals;
    unsigned wins;
    unsigned max_ante;
    unsigned action_overflows;
    unsigned observation_calls;
    unsigned observation_overflows;
    unsigned max_observed_cards;
    unsigned max_observed_variants;
    unsigned max_observed_hand;
    uint64_t observation_bytes;
    double sparse_reward;
    double shaped_reward;
} BenchStats;

typedef enum BenchMode {
    BENCH_LIFECYCLE,
    BENCH_LEGAL,
    BENCH_CLONE,
    BENCH_PLAY,
    BENCH_OBSERVE,
    BENCH_OBSERVE_COMPLEX
} BenchMode;

static volatile uint64_t benchmark_sink;

static uint8_t legal_group_type(const BalatroLegalGroup *group) {
    return group->kind == BALATRO_LEGAL_SELECTION ?
        group->selection.type : group->action.type;
}

static int find_view_action(const BalatroLegalView *view, uint8_t type,
    BalatroAction *out) {
    for (uint16_t i = 0; i < view->group_count; ++i) {
        if (legal_group_type(&view->groups[i]) != type) continue;
        return balatro_legal_group_action(&view->groups[i], 0, out);
    }
    return BALATRO_ERR_ACTION;
}

static int choose_lifecycle_view_action(const BalatroState *state,
    const BalatroLegalView *view, unsigned episode, BalatroAction *out) {
    if (state->phase == BALATRO_PHASE_BLIND_SELECT) {
        if (episode % 11 == 0 &&
            find_view_action(view, BALATRO_ACTION_SKIP_BLIND, out) == BALATRO_OK)
            return BALATRO_OK;
        return find_view_action(view, BALATRO_ACTION_SELECT_BLIND, out);
    }
    if (state->phase == BALATRO_PHASE_SELECTING_HAND) {
        if (state->actions_taken % 13 == 0 &&
            find_view_action(view, BALATRO_ACTION_USE_CONSUMABLE, out) == BALATRO_OK)
            return BALATRO_OK;
        if (state->discards_left && state->actions_taken % 5 == 0 &&
            find_view_action(view, BALATRO_ACTION_DISCARD, out) == BALATRO_OK)
            return BALATRO_OK;
        for (uint16_t i = 0; i < view->group_count; ++i) {
            const BalatroLegalGroup *group = &view->groups[i];
            if (legal_group_type(group) != BALATRO_ACTION_PLAY_HAND) continue;
            BalatroLegalGroup maximum = *group;
            maximum.selection.minimum = maximum.selection.maximum;
            return balatro_legal_group_action(&maximum, 0, out);
        }
        return find_view_action(view, BALATRO_ACTION_DISCARD, out);
    }
    if (state->phase == BALATRO_PHASE_ROUND_EVAL)
        return find_view_action(view, BALATRO_ACTION_CASH_OUT, out);
    if (state->phase == BALATRO_PHASE_PACK_OPENING) {
        if (state->actions_taken % 9 == 0 &&
            find_view_action(view, BALATRO_ACTION_SKIP_PACK, out) == BALATRO_OK)
            return BALATRO_OK;
        if (find_view_action(view, BALATRO_ACTION_PICK_PACK_CARD, out) == BALATRO_OK)
            return BALATRO_OK;
        return find_view_action(view, BALATRO_ACTION_SKIP_PACK, out);
    }
    if (state->phase == BALATRO_PHASE_SHOP) {
        if (state->actions_taken % 17 == 0 &&
            find_view_action(view, BALATRO_ACTION_SELL_JOKER, out) == BALATRO_OK)
            return BALATRO_OK;
        if (state->actions_taken % 7 == 0 &&
            find_view_action(view, BALATRO_ACTION_REROLL, out) == BALATRO_OK)
            return BALATRO_OK;
        if (state->actions_taken % 3 == 0 &&
            find_view_action(view, BALATRO_ACTION_REDEEM_VOUCHER, out) == BALATRO_OK)
            return BALATRO_OK;
        if (find_view_action(view, BALATRO_ACTION_BUY_CARD, out) == BALATRO_OK)
            return BALATRO_OK;
        if (find_view_action(view, BALATRO_ACTION_OPEN_BOOSTER, out) == BALATRO_OK)
            return BALATRO_OK;
        return find_view_action(view, BALATRO_ACTION_NEXT_ROUND, out);
    }
    return BALATRO_ERR_ACTION;
}

static int first_mask_index(uint64_t mask) {
    for (int index = 0; index < 64; ++index)
        if (mask & (UINT64_C(1) << index)) return index;
    return -1;
}

static int policy_selection(const BalatroObservedSelection *legal, uint8_t wanted, BalatroPolicyAction *out) {
    if (!legal->valid) return BALATRO_ERR_ACTION;
    uint8_t count = wanted < legal->minimum ? legal->minimum : wanted > legal->maximum ? legal->maximum : wanted;
    uint64_t selected = legal->required_hand;
    uint8_t selected_count = 0;
    for (uint8_t i = 0; i < 64; ++i)
        if (selected & (UINT64_C(1) << i)) selected_count++;
    for (uint8_t i = 0; i < 64 && selected_count < count; ++i)
        if ((legal->allowed_hand & (UINT64_C(1) << i)) && !(selected & (UINT64_C(1) << i))) {
            selected |= UINT64_C(1) << i;
            selected_count++;
        }
    if (selected_count != count || count > BALATRO_MAX_SELECTION) return BALATRO_ERR_ACTION;
    out->selection_count = count;
    uint8_t written = 0;
    for (uint8_t i = 0; i < 64; ++i)
        if (selected & (UINT64_C(1) << i)) out->selection[written++] = i;
    return BALATRO_OK;
}

static int find_observation_action(const BalatroObservation *observation, uint8_t type, BalatroPolicyAction *out, int maximum_selection) {
    if (type >= BALATRO_ACTION_TYPE_COUNT || !observation->legal.action_type[type]) return BALATRO_ERR_ACTION;
    *out = (BalatroPolicyAction){.type = type, .reorder_destination = UINT16_MAX};
    int primary = first_mask_index(observation->legal.primary[type]);
    if (primary < 0) return BALATRO_ERR_ACTION;
    out->primary = (uint16_t)primary;
    const BalatroObservedSelection *selection = NULL;
    if (type == BALATRO_ACTION_PLAY_HAND)
        selection = &observation->legal.play;
    else if (type == BALATRO_ACTION_DISCARD)
        selection = &observation->legal.discard;
    else if (type == BALATRO_ACTION_USE_CONSUMABLE)
        selection = &observation->legal.consumable[primary];
    else if (type == BALATRO_ACTION_BUY_AND_USE)
        selection = &observation->legal.shop[primary];
    else if (type == BALATRO_ACTION_PICK_PACK_CARD)
        selection = &observation->legal.pack[primary];
    if (selection && selection->valid)
        return policy_selection(selection, maximum_selection ? selection->maximum : selection->minimum, out);
    return BALATRO_OK;
}

static int choose_lifecycle_observation_action(const BalatroState *state, const BalatroObservation *observation, unsigned episode,
                                               BalatroPolicyAction *out) {
    if (state->phase == BALATRO_PHASE_BLIND_SELECT) {
        if (episode % 11 == 0 && find_observation_action(observation, BALATRO_ACTION_SKIP_BLIND, out, 0) == BALATRO_OK)
            return BALATRO_OK;
        return find_observation_action(observation, BALATRO_ACTION_SELECT_BLIND, out, 0);
    }
    if (state->phase == BALATRO_PHASE_SELECTING_HAND) {
        if (state->actions_taken % 13 == 0 && find_observation_action(observation, BALATRO_ACTION_USE_CONSUMABLE, out, 0) == BALATRO_OK)
            return BALATRO_OK;
        if (state->discards_left && state->actions_taken % 5 == 0 &&
            find_observation_action(observation, BALATRO_ACTION_DISCARD, out, 0) == BALATRO_OK)
            return BALATRO_OK;
        if (find_observation_action(observation, BALATRO_ACTION_PLAY_HAND, out, 1) == BALATRO_OK) return BALATRO_OK;
        return find_observation_action(observation, BALATRO_ACTION_DISCARD, out, 0);
    }
    if (state->phase == BALATRO_PHASE_ROUND_EVAL)
        return find_observation_action(observation, BALATRO_ACTION_CASH_OUT, out, 0);
    if (state->phase == BALATRO_PHASE_PACK_OPENING) {
        if (state->actions_taken % 9 == 0 && find_observation_action(observation, BALATRO_ACTION_SKIP_PACK, out, 0) == BALATRO_OK)
            return BALATRO_OK;
        if (find_observation_action(observation, BALATRO_ACTION_PICK_PACK_CARD, out, 0) == BALATRO_OK) return BALATRO_OK;
        return find_observation_action(observation, BALATRO_ACTION_SKIP_PACK, out, 0);
    }
    if (state->phase == BALATRO_PHASE_SHOP) {
        if (state->actions_taken % 17 == 0 && find_observation_action(observation, BALATRO_ACTION_SELL_JOKER, out, 0) == BALATRO_OK)
            return BALATRO_OK;
        if (state->actions_taken % 7 == 0 && find_observation_action(observation, BALATRO_ACTION_REROLL, out, 0) == BALATRO_OK)
            return BALATRO_OK;
        if (state->actions_taken % 3 == 0 && find_observation_action(observation, BALATRO_ACTION_REDEEM_VOUCHER, out, 0) == BALATRO_OK)
            return BALATRO_OK;
        if (find_observation_action(observation, BALATRO_ACTION_BUY_CARD, out, 0) == BALATRO_OK) return BALATRO_OK;
        if (find_observation_action(observation, BALATRO_ACTION_OPEN_BOOSTER, out, 0) == BALATRO_OK) return BALATRO_OK;
        return find_observation_action(observation, BALATRO_ACTION_NEXT_ROUND, out, 0);
    }
    return BALATRO_ERR_ACTION;
}

static unsigned parse_u32(const char *value, const char *name) {
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (!value[0] || (end && *end) || parsed > 1000000000UL) {
        fprintf(stderr, "invalid %s: %s\n", name, value);
        exit(2);
    }
    return (unsigned)parsed;
}

static void account_observation(BenchStats *stats, const BalatroObservation *observation) {
    stats->observation_calls++;
    stats->observation_bytes += observation->encoded_bytes;
    if (observation->owned_deck.total > stats->max_observed_cards) stats->max_observed_cards = observation->owned_deck.total;
    if (observation->variants.count > stats->max_observed_variants) stats->max_observed_variants = observation->variants.count;
    if (observation->hand.count > stats->max_observed_hand) stats->max_observed_hand = observation->hand.count;
    benchmark_sink += observation->encoded_bytes;
}

static void prepare_selecting_state(BalatroState *state,
    const BalatroConfig *config) {
    if (balatro_init_seed_string(state, config, "BENCHMARK") != BALATRO_OK)
        exit(1);
    BalatroAction action = {.type = BALATRO_ACTION_SELECT_BLIND};
    BalatroStepResult result;
    if (balatro_step(state, &action, &result) != BALATRO_OK) exit(1);
}

static int materialize_legal_actions(const BalatroState *state,
    BalatroAction *out, size_t capacity) {
    BalatroLegalView view;
    int error = balatro_legal_view(state, &view);
    if (error) return error;
    uint32_t written = 0;
    for (uint16_t i = 0; i < view.group_count; ++i) {
        uint32_t count = balatro_legal_group_count(&view.groups[i]);
        size_t room = written < capacity ? capacity - written : 0;
        size_t group_capacity = count < room ? count : room;
        error = balatro_legal_group_actions(
            &view.groups[i], group_capacity ? &out[written] : NULL,
            group_capacity);
        if (error < 0) return error;
        written += count;
    }
    return (int)written;
}

static int benchmark_legal(const BalatroConfig *config, unsigned iterations) {
    BalatroState state;
    prepare_selecting_state(&state, config);
    BalatroAction actions[BALATRO_MAX_LEGAL_ACTIONS];
    for (uint8_t hand_count = 5; hand_count <= BALATRO_MAX_HAND; ++hand_count) {
        state.hand_count = hand_count;
        for (uint8_t i = 8; i < hand_count; ++i)
            state.hand[i] = (BalatroCard){.suit = i & 3, .rank = 2 + i % 13};
        clock_t begin = clock();
        uint64_t generated = 0;
        int count = 0;
        for (unsigned i = 0; i < iterations; ++i) {
            count = materialize_legal_actions(&state, actions,
                BALATRO_MAX_LEGAL_ACTIONS);
            if (count < 0) return 1;
            generated += (uint64_t)count;
            benchmark_sink += actions[(unsigned)count / 2].selection_count;
        }
        double elapsed = (double)(clock() - begin) / CLOCKS_PER_SEC;
        printf("legal hand=%u: %d actions, %.3f us/call, %.0f actions/s\n",
            hand_count, count, elapsed * 1e6 / iterations,
            generated / elapsed);
    }
    return 0;
}

static int benchmark_clone(const BalatroConfig *config, unsigned iterations) {
    BalatroState source, destination;
    prepare_selecting_state(&source, config);
    clock_t begin = clock();
    for (unsigned i = 0; i < iterations; ++i) {
        balatro_clone_state(&destination, &source);
        benchmark_sink += destination.hand_count;
    }
    double elapsed = (double)(clock() - begin) / CLOCKS_PER_SEC;
    printf("%u semantic clones in %.3f s (%.0f clones/s, %.3f ns/clone)\n",
        iterations, elapsed, iterations / elapsed, elapsed * 1e9 / iterations);
    return balatro_state_hash(&source) == balatro_state_hash(&destination) ? 0 : 1;
}

static int benchmark_play(const BalatroConfig *config, unsigned iterations) {
    BalatroState state;
    prepare_selecting_state(&state, config);
    BalatroAction all[BALATRO_MAX_LEGAL_ACTIONS];
    BalatroAction plays[BALATRO_MAX_LEGAL_ACTIONS];
    double scores[BALATRO_MAX_LEGAL_ACTIONS];
    int count = materialize_legal_actions(
        &state, all, BALATRO_MAX_LEGAL_ACTIONS);
    size_t play_count = 0;
    for (int i = 0; i < count; ++i)
        if (all[i].type == BALATRO_ACTION_PLAY_HAND)
            plays[play_count++] = all[i];
    clock_t begin = clock();
    for (unsigned i = 0; i < iterations; ++i) {
        int error = balatro_score_play_actions_trusted(
            &state, plays, play_count, scores);
        if (error) return 1;
        double score = scores[i % play_count];
        benchmark_sink += isfinite(score) ? (uint64_t)fmod(score, 4294967291.0) : 0;
    }
    double elapsed = (double)(clock() - begin) / CLOCKS_PER_SEC;
    double branches = (double)iterations * play_count;
    printf("%.0f trusted play branches in %.3f s (%.0f branches/s; %zu actions/batch)\n",
        branches, elapsed, branches / elapsed, play_count);
    return 0;
}

static int benchmark_observation(const BalatroConfig *config, unsigned iterations, int complex_deck) {
    BalatroState state;
    prepare_selecting_state(&state, config);
    if (complex_deck) {
        unsigned ordinal = 0;
        BalatroCard *areas[] = {state.deck, state.hand, state.discard};
        const uint16_t counts[] = {state.deck_count, state.hand_count, state.discard_count};
        for (size_t area = 0; area < sizeof(areas) / sizeof(areas[0]); ++area)
            for (uint16_t i = 0; i < counts[area]; ++i, ++ordinal) {
                BalatroCard *card = &areas[area][i];
                card->enhancement = (uint8_t)(ordinal % (BALATRO_ENHANCEMENT_LUCKY + 1));
                card->edition = (uint8_t)(ordinal % (BALATRO_EDITION_NEGATIVE + 1));
                card->seal = (uint8_t)(ordinal % (BALATRO_SEAL_PURPLE + 1));
                card->perma_bonus = (int16_t)((int)(ordinal % 15) - 7);
                card->state[3] = (int32_t)(ordinal & 1u);
            }
    }
    BalatroObservation observation;
    clock_t begin = clock();
    for (unsigned i = 0; i < iterations; ++i) {
        if (balatro_observe(&state, &observation) != BALATRO_OK) return 1;
        benchmark_sink += observation.variants.count + observation.legal.action_type[i % BALATRO_ACTION_TYPE_COUNT];
    }
    double elapsed = (double)(clock() - begin) / CLOCKS_PER_SEC;
    printf("%u %s observations in %.3f s (%.0f observations/s, %.3f us/observation)\n", iterations,
           complex_deck ? "heterogeneous" : "ordinary", elapsed, iterations / elapsed, elapsed * 1e6 / iterations);
    return 0;
}

int main(int argc, char **argv) {
    BalatroConfig config;
    balatro_default_config(&config);
    BalatroStepResult result;
    BenchStats stats = {0};
    unsigned episodes = 10000;
    unsigned max_steps = 512;
    unsigned seed_base = 1;
    unsigned iterations = 0;
    int observe = 0;
    int trusted = 0;
    BenchMode mode = BENCH_LIFECYCLE;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--observe")) {
            observe = 1;
            continue;
        }
        if (!strcmp(argv[i], "--trusted")) {
            trusted = 1;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", argv[i]);
            return 2;
        }
        if (!strcmp(argv[i], "--episodes")) episodes = parse_u32(argv[++i], "episodes");
        else if (!strcmp(argv[i], "--max-steps")) max_steps = parse_u32(argv[++i], "max-steps");
        else if (!strcmp(argv[i], "--iterations")) iterations = parse_u32(argv[++i], "iterations");
        else if (!strcmp(argv[i], "--mode")) {
            const char *value = argv[++i];
            if (!strcmp(value, "lifecycle")) mode = BENCH_LIFECYCLE;
            else if (!strcmp(value, "legal")) mode = BENCH_LEGAL;
            else if (!strcmp(value, "clone")) mode = BENCH_CLONE;
            else if (!strcmp(value, "play")) mode = BENCH_PLAY;
            else if (!strcmp(value, "observe")) mode = BENCH_OBSERVE;
            else if (!strcmp(value, "observe-complex")) mode = BENCH_OBSERVE_COMPLEX;
            else {
                fprintf(stderr, "invalid mode: %s\n", value);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--win-ante")) {
            unsigned value = parse_u32(argv[++i], "win-ante");
            config.win_ante = (uint8_t)(value < 1 ? 1 : value > 255 ? 255 : value);
        } else if (!strcmp(argv[i], "--seed")) seed_base = parse_u32(argv[++i], "seed");
        else {
            fprintf(stderr, "usage: %s [--mode lifecycle|legal|clone|play|observe|observe-complex] [--iterations N] [--observe] [--trusted] [--episodes N] [--max-steps N] [--win-ante N] [--seed N]\n", argv[0]);
            return 2;
        }
    }
    if (mode == BENCH_LEGAL)
        return benchmark_legal(&config, iterations ? iterations : 10000);
    if (mode == BENCH_CLONE)
        return benchmark_clone(&config, iterations ? iterations : 5000000);
    if (mode == BENCH_PLAY)
        return benchmark_play(&config, iterations ? iterations : 10000);
    if (mode == BENCH_OBSERVE || mode == BENCH_OBSERVE_COMPLEX)
        return benchmark_observation(&config, iterations ? iterations : 1000000, mode == BENCH_OBSERVE_COMPLEX);
    clock_t begin = clock();

    for (unsigned episode = 0; episode < episodes; ++episode) {
        BalatroState state;
        if (balatro_init(&state, &config, (uint64_t)seed_base + episode) != BALATRO_OK) return 1;
        stats.episodes++;
        BalatroObservation observation;
        if (observe) {
            int observation_error = balatro_observe(&state, &observation);
            if (observation_error != BALATRO_OK) return 1;
            account_observation(&stats, &observation);
        }
        for (unsigned step = 0; step < max_steps && !state.terminal; ++step) {
            BalatroAction chosen;
            int error;
            if (observe) {
                BalatroPolicyAction policy;
                if (choose_lifecycle_observation_action(&state, &observation, episode, &policy)) {
                    fprintf(stderr, "no observation action: episode=%u step=%u phase=%u\n", episode, step, state.phase);
                    return 1;
                }
                chosen = (BalatroAction){.type = policy.type};
                error = trusted ? balatro_step_observe_trusted(&state, &policy, &result, &observation)
                                : balatro_step_observe(&state, &policy, &result, &observation);
                if (!error) account_observation(&stats, &observation);
            } else {
                BalatroLegalView view;
                int view_error = balatro_legal_view(&state, &view);
                if (view_error) {
                    stats.action_overflows++;
                    break;
                }
                if (choose_lifecycle_view_action(&state, &view, episode, &chosen)) {
                    fprintf(stderr, "no action: episode=%u step=%u phase=%u count=%u groups=%u\n",
                        episode, step, state.phase, view.action_count, view.group_count);
                    return 1;
                }
                error = balatro_step(&state, &chosen, &result);
            }
            if (error != BALATRO_OK) {
                fprintf(stderr, "step error %d: episode=%u step=%u phase=%u action=%u\n", error, episode, step, state.phase, chosen.type);
                return 1;
            }
            if (result.truncated) {
                stats.observation_overflows++;
                break;
            }
            stats.steps++;
            stats.sparse_reward += result.sparse_reward;
            stats.shaped_reward += result.reward;
            switch (chosen.type) {
            case BALATRO_ACTION_PLAY_HAND: stats.scored_hands++; break;
            case BALATRO_ACTION_DISCARD: stats.discards++; break;
            case BALATRO_ACTION_USE_CONSUMABLE: stats.consumable_uses++; break;
            case BALATRO_ACTION_CASH_OUT: stats.shops++; break;
            case BALATRO_ACTION_OPEN_BOOSTER: stats.packs++; break;
            case BALATRO_ACTION_BUY_CARD: stats.buys++; break;
            case BALATRO_ACTION_REROLL: stats.rerolls++; break;
            case BALATRO_ACTION_SELL_JOKER: stats.sells++; break;
            case BALATRO_ACTION_REDEEM_VOUCHER: stats.vouchers++; break;
            case BALATRO_ACTION_SKIP_BLIND: stats.skipped_blinds++; break;
            case BALATRO_ACTION_SELECT_BLIND: stats.selected_blinds++; break;
            case BALATRO_ACTION_SKIP_PACK: stats.skipped_packs++; break;
            case BALATRO_ACTION_NEXT_ROUND: stats.next_rounds++; break;
            default: break;
            }
        }
        if (state.terminal) {
            stats.terminals++;
            stats.wins += state.won;
            if (state.ante > stats.max_ante) stats.max_ante = state.ante;
        }
    }

    double elapsed = (double)(clock() - begin) / CLOCKS_PER_SEC;
    printf("%u lifecycle episodes in %.3f s (%.0f episodes/s, %.0f steps/s; win_ante=%u; observe=%s; trusted=%s)\n",
        stats.episodes, elapsed, stats.episodes / elapsed, stats.steps / elapsed,
        config.win_ante, observe ? "on" : "off", trusted ? "on" : "off");
    printf("accepted steps: %u; selects: %u; skips: %u; scored hands: %u; discards: %u; uses: %u; cashouts: %u; next rounds: %u; packs: %u; skipped packs: %u; buys: %u; sells: %u; vouchers: %u; rerolls: %u; terminals: %u; wins: %u; max ante: %u; action overflows: %u\n",
        stats.steps, stats.selected_blinds, stats.skipped_blinds, stats.scored_hands, stats.discards,
        stats.consumable_uses, stats.shops, stats.next_rounds, stats.packs, stats.skipped_packs, stats.buys, stats.sells,
        stats.vouchers, stats.rerolls, stats.terminals, stats.wins, stats.max_ante, stats.action_overflows);
    printf("reward totals: sparse=%.3f shaped=%.3f; terminal_rate=%.3f; win_rate=%.3f\n",
        stats.sparse_reward, stats.shaped_reward,
        stats.episodes ? (double)stats.terminals / stats.episodes : 0.0,
        stats.episodes ? (double)stats.wins / stats.episodes : 0.0);
    if (observe)
        printf("observation profile: cards=%u variants=%u hand=%u jokers=%u consumables=%u tags=%u vouchers=%u; bytes=%zu; calls=%u; overflows=%u; observed maxima: cards=%u variants=%u hand=%u\n",
            config.observation.playing_cards, config.observation.playing_variants, config.observation.hand, config.observation.jokers,
            config.observation.consumables, config.observation.tags, config.observation.shop_vouchers, sizeof(BalatroObservation),
            stats.observation_calls, stats.observation_overflows, stats.max_observed_cards, stats.max_observed_variants,
            stats.max_observed_hand);
    return 0;
}
