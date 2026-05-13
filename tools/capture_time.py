#!/usr/bin/env python3
"""
Capture the /time portal page (Phase 3 of plan-ntp-server.md).

Trigger a few SNTP queries first so the recent-clients table has content
to show. Captures desktop and mobile viewports.

Usage: PORTAL_URL=http://192.168.22.100 PORTAL_AUTH=user:password \
       python3 tools/capture_time.py
"""
import os, socket, time
from pathlib import Path
from playwright.sync_api import sync_playwright

BASE = os.environ.get("PORTAL_URL", "http://192.168.22.100").rstrip("/")
AUTH = os.environ.get("PORTAL_AUTH", "").strip()
OUT = Path(__file__).resolve().parent.parent / "docs" / "screenshots" / "ux-audit"
OUT.mkdir(parents=True, exist_ok=True)

# 1. Generate some SNTP traffic so the recent-clients table is non-empty.
host = BASE.split("://", 1)[-1].split("/")[0].split(":")[0]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(1.0)
sent = 0
for _ in range(4):
    pkt = bytearray(48)
    pkt[0] = 0x23  # LI=0 VN=4 Mode=3
    try:
        s.sendto(bytes(pkt), (host, 123))
        s.recvfrom(64)
        sent += 1
    except Exception:
        pass
s.close()
print(f"  sent {sent} SNTP queries to {host}:123 to populate recent-clients")

# 2. Capture in both viewports.
with sync_playwright() as pw:
    b = pw.chromium.launch()
    for tag, w, h, dsf in [("desktop", 1024, 900, 1.0),
                            ("mobile",   390, 844, 2.0)]:
        if AUTH and ":" in AUTH:
            u, _, p = AUTH.partition(":")
            ctx = b.new_context(
                viewport={"width": w, "height": h},
                device_scale_factor=dsf,
                http_credentials={"username": u, "password": p},
            )
        else:
            ctx = b.new_context(
                viewport={"width": w, "height": h},
                device_scale_factor=dsf,
            )
        page = ctx.new_page()
        page.goto(BASE + "/time", wait_until="domcontentloaded", timeout=10000)
        page.wait_for_timeout(400)
        target = OUT / f"time_{tag}.png"
        page.screenshot(path=str(target), full_page=True)
        print(f"  {tag}  -> {target.relative_to(OUT.parent.parent)} ({target.stat().st_size // 1024} KB)")
        ctx.close()
    b.close()
