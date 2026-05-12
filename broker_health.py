#!/usr/bin/env python3
"""
broker_health.py — Long-term health monitor for the ESP32 MQTT broker.

Polls /api/status and /api/clients periodically, tracks longest uptime,
detects reboots, watches free heap trend, and flags stale/missing clients.

Usage:
    python3 broker_health.py 192.168.22.100
    python3 broker_health.py 192.168.22.100 --interval 60 --state ~/.mqtt_broker_health.json
    python3 broker_health.py 192.168.22.100 --once          # one-shot check

Logs to stdout AND to broker_health.log (rotating, 1 MB × 5 files).
Exit code 0 = healthy, 1 = warnings, 2 = critical.
"""
from __future__ import annotations

import argparse
import json
import logging
import logging.handlers
import os
import sys
import time
from dataclasses import dataclass, asdict, field
from pathlib import Path
from typing import Any

import urllib.request
import urllib.error

# ---------- Thresholds ---------------------------------------------------------

HEAP_WARN_KB        = 4_000     # warn below this
HEAP_CRIT_KB        = 2_000     # critical below this
HEAP_DROP_WARN_KB   = 1_000     # warn if free heap drops this much vs. baseline
CLIENT_STALE_S      = 300       # client last_active older than this = stale
EXPECTED_CLIENTS    = None      # set via --expected; warn if fewer connect
HTTP_TIMEOUT_S      = 8

# ---------- State persistence --------------------------------------------------

@dataclass
class State:
    longest_uptime_s: int = 0
    longest_uptime_at: str = ""
    boots_seen: int = 0
    last_uptime_s: int = 0
    baseline_heap_kb: int = 0
    known_clients: dict[str, dict[str, Any]] = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path) -> "State":
        if path.exists():
            try:
                data = json.loads(path.read_text())
                if not data.get("known_clients"):
                    data["known_clients"] = {}
                return cls(**data)
            except Exception as e:
                logging.warning("Could not load state %s: %s", path, e)
        return cls()

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(asdict(self), indent=2, default=str))
        tmp.replace(path)

# ---------- HTTP helpers -------------------------------------------------------

def fetch_json(url: str) -> dict:
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_S) as r:
        return json.loads(r.read().decode("utf-8"))

# ---------- Health logic -------------------------------------------------------

def fmt_uptime(s: int) -> str:
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    parts = []
    if d: parts.append(f"{d}d")
    if h or d: parts.append(f"{h}h")
    parts.append(f"{m}m")
    return " ".join(parts)

def check_once(host: str, state: State, expected_clients: int | None) -> int:
    """Returns severity: 0 ok, 1 warn, 2 crit."""
    base = f"http://{host}"
    sev = 0

    try:
        status = fetch_json(f"{base}/api/status")
        clients = fetch_json(f"{base}/api/clients")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        logging.critical("UNREACHABLE: %s (%s)", host, e)
        return 2
    except Exception as e:
        logging.critical("Bad response from %s: %s", host, e)
        return 2

    uptime_s = int(status["system"]["uptime_s"])
    heap_kb  = int(status["system"]["free_heap_kb"])
    fw       = status["firmware"]["version"]
    n_mqtt   = len(clients.get("mqtt", []))
    n_max    = int(status["broker"]["max_clients"])
    n_subs   = int(status["broker"]["subs"])
    n_ret    = int(status["broker"]["retained"])

    # --- Boot / uptime tracking ---
    if uptime_s < state.last_uptime_s:
        state.boots_seen += 1
        logging.warning(
            "REBOOT DETECTED: uptime went from %s to %s (boot #%d)",
            fmt_uptime(state.last_uptime_s), fmt_uptime(uptime_s), state.boots_seen
        )
        sev = max(sev, 1)
    state.last_uptime_s = uptime_s

    if uptime_s > state.longest_uptime_s:
        state.longest_uptime_s = uptime_s
        state.longest_uptime_at = time.strftime("%Y-%m-%d %H:%M:%S")

    # --- Heap ---
    if state.baseline_heap_kb == 0 and uptime_s > 600:
        state.baseline_heap_kb = heap_kb
        logging.info("Heap baseline set: %d KB", heap_kb)

    if heap_kb < HEAP_CRIT_KB:
        logging.critical("HEAP CRITICAL: %d KB free (< %d)", heap_kb, HEAP_CRIT_KB)
        sev = 2
    elif heap_kb < HEAP_WARN_KB:
        logging.warning("HEAP LOW: %d KB free", heap_kb)
        sev = max(sev, 1)

    if state.baseline_heap_kb and (state.baseline_heap_kb - heap_kb) > HEAP_DROP_WARN_KB:
        logging.warning(
            "HEAP DRIFT: down %d KB vs baseline (%d → %d). Possible leak/fragmentation.",
            state.baseline_heap_kb - heap_kb, state.baseline_heap_kb, heap_kb
        )
        sev = max(sev, 1)

    # --- Clients ---
    if expected_clients is not None and n_mqtt < expected_clients:
        logging.warning("CLIENT COUNT LOW: %d connected (expected ≥ %d)", n_mqtt, expected_clients)
        sev = max(sev, 1)
    if n_mqtt >= n_max:
        logging.critical("CLIENT CAP REACHED: %d / %d", n_mqtt, n_max)
        sev = 2

    # Stale clients + churn detection
    seen_ids = set()
    for c in clients.get("mqtt", []):
        cid = c["client_id"]
        seen_ids.add(cid)
        last = int(c.get("last_active_s", 0))
        keep = int(c.get("keep_alive", 60))
        # last_active > 2× keepalive + grace = suspicious
        if last > max(CLIENT_STALE_S, keep * 2 + 30):
            logging.warning("STALE CLIENT: %s @ %s — last active %ds ago (keepalive %ds)",
                            cid, c.get("ip"), last, keep)
            sev = max(sev, 1)
        prev = state.known_clients.get(cid, {})
        prev_conn = prev.get("connected_s", 0)
        # If a previously long-connected client suddenly reset its connection counter
        if prev_conn > 600 and int(c["connected_s"]) < prev_conn - 60:
            logging.warning("CLIENT RECONNECTED: %s — was up %ds, now %ds",
                            cid, prev_conn, c["connected_s"])
            sev = max(sev, 1)
        state.known_clients[cid] = {
            "ip": c.get("ip"),
            "connected_s": int(c["connected_s"]),
            "last_seen": time.time(),
        }

    # Mark clients we used to know but didn't see now
    missing = []
    for cid, info in list(state.known_clients.items()):
        if cid in seen_ids:
            continue
        age = time.time() - info.get("last_seen", 0)
        if age < 3600:
            missing.append((cid, info.get("ip"), int(age)))
        elif age > 7 * 86400:
            # forget after a week
            del state.known_clients[cid]
    if missing:
        for cid, ip, age in missing:
            logging.warning("CLIENT MISSING: %s @ %s — last seen %ds ago", cid, ip, age)
        sev = max(sev, 1)

    # --- Healthy summary line ---
    logging.info(
        "OK fw=%s up=%s clients=%d/%d subs=%d retained=%d heap=%dKB longest_up=%s boots=%d",
        fw, fmt_uptime(uptime_s), n_mqtt, n_max, n_subs, n_ret, heap_kb,
        fmt_uptime(state.longest_uptime_s), state.boots_seen,
    )
    return sev

# ---------- Logging setup ------------------------------------------------------

def setup_logging(log_path: Path, verbose: bool) -> None:
    fmt = logging.Formatter("%(asctime)s %(levelname)-8s %(message)s",
                             "%Y-%m-%d %H:%M:%S")
    root = logging.getLogger()
    root.setLevel(logging.DEBUG if verbose else logging.INFO)

    sh = logging.StreamHandler(sys.stdout)
    sh.setFormatter(fmt)
    root.addHandler(sh)

    fh = logging.handlers.RotatingFileHandler(
        log_path, maxBytes=1_000_000, backupCount=5
    )
    fh.setFormatter(fmt)
    root.addHandler(fh)

# ---------- Main ---------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host", help="Broker IP or hostname (e.g. 192.168.22.100)")
    p.add_argument("--interval", type=int, default=60,
                   help="Seconds between checks (default 60). Ignored with --once.")
    p.add_argument("--once", action="store_true", help="Run a single check and exit")
    p.add_argument("--state", type=Path,
                   default=Path.home() / ".mqtt_broker_health.json",
                   help="State file path (default ~/.mqtt_broker_health.json)")
    p.add_argument("--log", type=Path, default=Path("broker_health.log"),
                   help="Log file path (default ./broker_health.log)")
    p.add_argument("--expected", type=int, default=None,
                   help="Minimum expected client count (warn if below)")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()

    setup_logging(args.log, args.verbose)
    logging.info("broker_health monitoring %s (state=%s)", args.host, args.state)

    state = State.load(args.state)
    last_sev = 0
    try:
        while True:
            try:
                last_sev = check_once(args.host, state, args.expected)
            except Exception as e:
                logging.exception("Unhandled error during check: %s", e)
                last_sev = 2
            state.save(args.state)
            if args.once:
                return last_sev
            time.sleep(args.interval)
    except KeyboardInterrupt:
        logging.info("Stopped by user. longest_uptime=%s boots_seen=%d",
                     fmt_uptime(state.longest_uptime_s), state.boots_seen)
        return last_sev

if __name__ == "__main__":
    sys.exit(main())
