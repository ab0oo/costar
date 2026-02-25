#!/usr/bin/env python3
import argparse
import asyncio
import json
import sys
from typing import Any

try:
    import websockets
except Exception as exc:  # pragma: no cover
    print(f"error: websockets package is required ({exc})", file=sys.stderr)
    print("hint: pip install websockets", file=sys.stderr)
    sys.exit(2)


def compact(obj: Any) -> str:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def pretty(obj: Any) -> str:
    return json.dumps(obj, indent=2, ensure_ascii=False, sort_keys=False)


def print_tx(label: str, obj: Any) -> str:
    wire = compact(obj)
    print(f"\n>> TX {label} bytes={len(wire)}")
    print(pretty(obj))
    return wire


def print_rx(label: str, raw: str, obj: Any) -> None:
    print(f"\n<< RX {label} bytes={len(raw)}")
    print(pretty(obj))


async def run(url: str, token: str, entity_id: str, max_events: int, timeout_s: float) -> None:
    req_id = 1
    template = (
        "{% set s = states[entity_id] %}"
        "{{ {'entity_id': entity_id,'state': (s.state if s else ''),'attributes': (s.attributes if s else {})} | tojson }}"
    )

    async with websockets.connect(url, ping_interval=30, ping_timeout=30, close_timeout=5) as ws:
        print(f"connected url={url}")

        raw = await asyncio.wait_for(ws.recv(), timeout=timeout_s)
        msg = json.loads(raw)
        print_rx("auth_required", raw, msg)
        if msg.get("type") != "auth_required":
            raise RuntimeError(f"unexpected first message: {msg}")

        tx = {"type": "auth", "access_token": token}
        raw_tx = print_tx("auth", tx)
        await ws.send(raw_tx)

        raw = await asyncio.wait_for(ws.recv(), timeout=timeout_s)
        msg = json.loads(raw)
        print_rx("auth_result", raw, msg)
        if msg.get("type") != "auth_ok":
            raise RuntimeError(f"auth failed: {msg}")

        req_id += 1
        tx = {
            "id": req_id,
            "type": "subscribe_trigger",
            "trigger": [{"platform": "state", "entity_id": entity_id}],
        }
        raw_tx = print_tx("subscribe_trigger", tx)
        await ws.send(raw_tx)
        sub_id = req_id

        req_id += 1
        tx = {
            "id": req_id,
            "type": "render_template",
            "template": template,
            "report_errors": True,
            "variables": {"entity_id": entity_id},
        }
        raw_tx = print_tx("render_template", tx)
        await ws.send(raw_tx)
        render_id = req_id

        received = 0
        while received < max_events:
            raw = await asyncio.wait_for(ws.recv(), timeout=timeout_s)
            received += 1
            try:
                msg = json.loads(raw)
            except Exception:
                print(f"\n<< RX [{received}] non-json bytes={len(raw)} payload={raw[:200]!r}")
                continue

            mtype = msg.get("type")
            mid = msg.get("id")
            if mtype == "result" and mid == sub_id:
                print_rx(f"[{received}] subscribe_result", raw, msg)
                continue
            if mtype == "result" and mid == render_id:
                print_rx(f"[{received}] bootstrap_result", raw, msg)
                continue
            if mtype == "event" and mid == render_id:
                print_rx(f"[{received}] bootstrap_event", raw, msg)
                continue
            if mtype == "event" and mid == sub_id:
                print_rx(f"[{received}] trigger_event", raw, msg)
                continue
            print_rx(f"[{received}] type={mtype} id={mid}", raw, msg)


def main() -> int:
    parser = argparse.ArgumentParser(description="Probe Home Assistant WS using the same flow as ESP runtime.")
    parser.add_argument("--url", default="wss://homeassistant.edh.gorkos.net/api/websocket")
    parser.add_argument("--token", required=True, help="Long-lived access token")
    parser.add_argument("--entity", default="light.john_s_lamp")
    parser.add_argument("--max-events", type=int, default=40)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    try:
        asyncio.run(run(args.url, args.token, args.entity, args.max_events, args.timeout))
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
