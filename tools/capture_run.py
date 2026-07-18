"""Control BalatroBot's manual UI action recorder."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.balatrobot_bridge import BalatroBotClient


def windows_path(path: Path) -> str:
    """Translate a WSL /mnt/<drive> path for the Windows Balatro process."""
    resolved = path.resolve()
    parts = resolved.parts
    if len(parts) >= 4 and parts[1] == "mnt" and len(parts[2]) == 1:
        return f"{parts[2].upper()}:/{'/'.join(parts[3:])}"
    return str(resolved)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("start", "stop", "status"))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=12346)
    parser.add_argument("--output", type=Path, default=Path("traces/ante1_manual.json"))
    parser.add_argument("--seed")
    parser.add_argument("--deck", default="RED")
    parser.add_argument("--stake", default="WHITE")
    args = parser.parse_args()

    client = BalatroBotClient(f"http://{args.host}:{args.port}", timeout=15.0)
    if args.command == "status":
        print(json.dumps(client.call("record", {}), indent=2, sort_keys=True))
        return 0
    if args.command == "stop":
        print(json.dumps(client.call("record", {"enabled": False}), indent=2, sort_keys=True))
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.seed:
        client.call("menu", {})
        client.call("start", {"seed": args.seed, "deck": args.deck, "stake": args.stake})
    result = client.call(
        "record", {"enabled": True, "path": windows_path(args.output)}
    )
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
