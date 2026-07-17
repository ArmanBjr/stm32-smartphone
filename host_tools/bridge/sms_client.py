"""Melipayamak SMS HTTP client used when the MCU emits SMS_SEND|..."""

from __future__ import annotations

import json
import os
from typing import Tuple

try:
    import requests
except ImportError as exc:  # pragma: no cover
    raise ImportError("pip install requests") from exc

# Credentials come from the environment — never commit real tokens.
#   MELIPAYAMAK_FROM   sender number (e.g. 5000…)
#   MELIPAYAMAK_TOKEN  path segment after /api/send/simple/
FROM = os.environ.get("MELIPAYAMAK_FROM", "").strip()
TOKEN = os.environ.get("MELIPAYAMAK_TOKEN", "").strip()


def send_sms(to: str, text: str) -> Tuple[str, str]:
    """POST to melipayamak. Returns (ok|err, payload) for SMS_RESULT|..."""
    if not TOKEN or not FROM:
        return "ERR", "missing MELIPAYAMAK_TOKEN/FROM env"
    if not to:
        return "ERR", "empty to"

    url = f"https://console.melipayamak.com/api/send/simple/{TOKEN}"
    data = {"from": FROM, "to": to, "text": text}
    try:
        resp = requests.post(url, json=data, timeout=15)
    except requests.RequestException as exc:
        return "ERR", exc.__class__.__name__

    if resp.status_code < 200 or resp.status_code >= 300:
        return "ERR", str(resp.status_code)

    try:
        body = resp.json()
    except json.JSONDecodeError:
        return "ERR", "bad-json"

    rec_id = body.get("recId")
    status = body.get("status")
    try:
        rec_num = int(rec_id) if rec_id is not None and str(rec_id).strip() != "" else 0
    except (TypeError, ValueError):
        rec_num = 0

    if rec_num > 0:
        return "OK", str(rec_num)

    if status is not None and str(status).strip() != "":
        safe = str(status).replace("|", "/").replace("\n", " ").replace("\r", " ")
    elif rec_id is not None:
        safe = f"recId={rec_id}"
    else:
        safe = "no-recId"
    return "ERR", safe
