"""Reproducible native Puffer/ROCm smoke training and held-out evaluation.

The validator deliberately uses a short run by default.  It proves that the
Balatro environment can be compiled, trained, checkpointed, and evaluated by
the pinned Puffer fork; longer experiments can pass ``--steps`` explicitly.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import time
from pathlib import Path


def dashboard_metrics(output: str) -> dict[str, float]:
    """Extract the final native dashboard user metrics from a train log."""
    # Puffer's table may contain ANSI colour escapes and places several user
    # stats on one line, so anchor only the metric token rather than the line.
    clean = re.sub(r"\x1b\[[0-9;]*m", "", output)
    metrics: dict[str, float] = {}
    for key in ("score", "wins", "ante", "invalid_actions", "reward"):
        matches = re.findall(rf"(?<![A-Za-z0-9_]){key}\s+(-?\d+(?:\.\d+)?)", clean)
        if matches:
            metrics[key] = float(matches[-1])
    return metrics


def run(command: list[str], cwd: Path, timeout: float) -> tuple[int, str, float]:
    started = time.perf_counter()
    env = os.environ.copy()
    # The ROCm fork owns the Torch/hipify environment.  When this script is
    # launched via Simulatro's `uv run`, its venv is first on PATH and
    # hides the fork's Torch installation, making build.sh fail before C++
    # compilation. Prefer the sibling Puffer venv when it exists.
    puffer_venv_bin = cwd.parent / "PufferLib" / ".venv" / "bin"
    if (puffer_venv_bin / "python").exists():
        env["PATH"] = f"{puffer_venv_bin}:{env.get('PATH', '')}"
        env["VIRTUAL_ENV"] = str(puffer_venv_bin.parent)
    proc = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout, check=False, env=env)
    return proc.returncode, proc.stdout, time.perf_counter() - started


def latest_checkpoint(root: Path) -> Path:
    candidates = sorted(root.glob("**/*.bin"), key=lambda path: path.stat().st_mtime)
    if not candidates:
        raise FileNotFoundError(f"no checkpoint under {root}")
    return candidates[-1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--puffer-root", type=Path, required=True)
    parser.add_argument("--balatro-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--steps", type=int, default=1024)
    parser.add_argument("--agents", type=int, default=16)
    parser.add_argument("--eval-games", type=int, default=4)
    parser.add_argument("--checkpoint-interval", type=int, default=1,
                        help="Checkpoint interval passed to Puffer (default: 1).")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--checkpoint", type=Path)
    parser.add_argument("--reward-mode", choices=("progress", "sparse"), default="progress",
                        help="Training reward: progress potential (default) or sparse win-only.")
    parser.add_argument("--train-win-ante", type=int,
                        help="Optional training-only win ante override for curriculum experiments.")
    parser.add_argument("--eval-win-ante", type=int,
                        help="Optional held-out evaluation win ante override.")
    parser.add_argument("--learning-rate", type=float,
                        help="Optional PPO learning-rate override.")
    parser.add_argument("--entropy-coef", type=float,
                        help="Optional PPO entropy-coefficient override.")
    args = parser.parse_args()
    puffer = args.puffer_root.resolve()
    balatro = args.balatro_root.resolve()
    record: dict[str, object] = {"steps": args.steps, "agents": args.agents,
                                 "eval_games": args.eval_games,
                                 "reward_mode": args.reward_mode}

    if not args.skip_build:
        # The masked 4,096-way Balatro head is numerically unstable in the
        # fork's default BF16 ROCm build (the first PPO loss becomes inf).
        # Float32 keeps the masked logits/ratios finite and is the correctness
        # baseline for this validator; callers can still pass --skip-build to
        # evaluate another prebuilt binary explicitly.
        code, output, elapsed = run(["bash", "build.sh", "balatro", "--rocm", "--float"], puffer, args.timeout)
        record["build_seconds"] = elapsed
        record["build_returncode"] = code
        if code:
            record["build_output_tail"] = output[-4000:]
            return _finish(record, args.output, 1)

    checkpoint = args.checkpoint.resolve() if args.checkpoint else None
    if checkpoint is None:
        checkpoint_dir = puffer / "checkpoints" / "balatro-validation"
        checkpoint_dir.mkdir(parents=True, exist_ok=True)
        command = ["./puffer", "train", "balatro",
                   f"train.total_timesteps={args.steps}",
                   f"vec.total_agents={args.agents}",
                   "train.horizon=64", "train.minibatch_size=1024",
                   f"base.checkpoint_interval={args.checkpoint_interval}",
                   f"base.checkpoint_dir={checkpoint_dir}"]
        command.append(f"env.shaped_reward={int(args.reward_mode == 'progress')}")
        if args.train_win_ante is not None:
            command.append(f"env.win_ante={args.train_win_ante}")
        if args.learning_rate is not None:
            command.append(f"train.learning_rate={args.learning_rate}")
        if args.entropy_coef is not None:
            command.append(f"train.ent_coef={args.entropy_coef}")
        code, output, elapsed = run(command, puffer, args.timeout)
        record["train_seconds"] = elapsed
        record["train_returncode"] = code
        if code:
            record["train_output_tail"] = output[-4000:]
            return _finish(record, args.output, 1)
        record["train_metrics"] = dashboard_metrics(output)
        checkpoint = latest_checkpoint(checkpoint_dir)
    record["checkpoint"] = str(checkpoint)

    eval_command = ["./puffer", "eval_bot", "balatro",
                    f"base.load_model_path={checkpoint}",
                    f"base.num_games={args.eval_games}",
                    f"base.eval_agents={args.eval_games}",
                    f"vec.total_agents={args.eval_games}", "vec.num_buffers=2",
                    "train.horizon=2048", "train.minibatch_size=8192"]
    eval_command.append(f"env.shaped_reward={int(args.reward_mode == 'progress')}")
    if args.eval_win_ante is not None:
        eval_command.append(f"env.win_ante={args.eval_win_ante}")
    code, output, elapsed = run(eval_command, puffer, args.timeout)
    record["eval_seconds"] = elapsed
    record["eval_returncode"] = code
    match = re.findall(r"bot_eval=(\d+)/(\d+).*?score=([0-9.+-]+)", output)
    if match:
        reported, denominator, score = match[-1]
        reported_count = int(reported)
        reported_denominator = int(denominator)
        record.update({"reported_bot_eval": reported_count,
                       "reported_bot_eval_denominator": reported_denominator,
                       "mean_score": float(score)})
        # eval_bot aggregates across workers, so bot_eval can exceed the
        # requested game count.  Preserve the raw report and only publish a
        # scored_games field when its denominator is internally consistent.
        if 0 <= reported_count <= reported_denominator == args.eval_games:
            record["scored_games"] = reported_count
    else:
        record["eval_output_tail"] = output[-4000:]
    return _finish(record, args.output, code)


def _finish(record: dict[str, object], output: Path | None, code: int) -> int:
    encoded = json.dumps(record, indent=2, sort_keys=True)
    print(encoded)
    if output:
        output.write_text(encoded + "\n", encoding="utf-8")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
