#!/usr/bin/env python3
"""
SmartPhone host bridge — web UI launcher.

Owns the STM32 USART1 COM port (close Termite first). Opens a local browser
console for /start, lock, time sync, live piano, song upload (txt/mp3), SMS.

Usage:
  pip install -r host_tools/requirements.txt
  python host_tools/sms_bridge.py COM3
  python host_tools/sms_bridge.py COM3 --time-sync
  python host_tools/sms_bridge.py --no-browser   # connect port from the UI

Then open http://127.0.0.1:8765 if the browser does not open automatically.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Allow `python host_tools/sms_bridge.py` without installing the package.
_HOST_TOOLS = Path(__file__).resolve().parent
if str(_HOST_TOOLS) not in sys.path:
    sys.path.insert(0, str(_HOST_TOOLS))


def _load_dotenv(path: Path) -> None:
    """Minimal KEY=VALUE loader (no python-dotenv dependency)."""
    import os

    if not path.is_file():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key, val = key.strip(), val.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = val


def main() -> int:
    _load_dotenv(_HOST_TOOLS / ".env")
    from bridge import BAUD, DEFAULT_HOST, DEFAULT_PORT
    from bridge.server import run_server

    parser = argparse.ArgumentParser(
        description="SmartPhone Bridge web UI (UART + SMS + song translator)"
    )
    parser.add_argument(
        "port",
        nargs="?",
        default=None,
        help="Optional serial port to auto-connect (e.g. COM3)",
    )
    parser.add_argument("--baud", type=int, default=BAUD)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--http-port", type=int, default=DEFAULT_PORT)
    parser.add_argument(
        "--time-sync",
        action="store_true",
        help="After auto-connect, push /time-{epoch} once",
    )
    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="Do not open a browser tab automatically",
    )
    args = parser.parse_args()

    print("[bridge] SmartPhone web bridge", flush=True)
    if args.port:
        print(f"[bridge] will auto-connect {args.port} @ {args.baud}", flush=True)
    print(f"[bridge] UI http://{args.host}:{args.http_port}/", flush=True)

    run_server(
        host=args.host,
        port=args.http_port,
        open_browser=not args.no_browser,
        connect_port=args.port,
        time_sync=args.time_sync,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
