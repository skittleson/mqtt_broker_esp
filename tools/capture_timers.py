#!/usr/bin/env python3
"""
Capture /timers UX screenshots for v0.8.0 review.

Captures:
  - /timers              (list view, has timer 1 configured + 15 empty)
  - /timers/edit?n=1     (populated edit form)
  - /timers/edit?n=2     (empty slot edit form)
  - /timers/edit?n=1&saved=1  (saved banner state)

Each in desktop (1024x900) and mobile (390x844 @2x).

Usage:
    PORTAL_URL=http://192.168.22.100 PORTAL_AUTH=user:pass \\
        python3 tools/capture_timers.py
"""
import os
import sys
import time
from pathlib import Path
from playwright.sync_api import sync_playwright

BASE = os.environ.get("PORTAL_URL", "http://192.168.22.100").rstrip("/")
AUTH = os.environ.get("PORTAL_AUTH", "").strip()
OUT = Path(__file__).resolve().parent.parent / "docs" / "screenshots" / "timers"
OUT.mkdir(parents=True, exist_ok=True)

PAGES = [
    ("/timers",                       "list",          400),
    ("/timers/edit?n=1",              "edit_populated", 400),
    ("/timers/edit?n=2",              "edit_empty",     400),
    ("/timers/edit?n=1&saved=1",      "edit_saved",     400),
]

VIEWPORTS = [
    ("desktop", 1024, 900,  1.0),
    ("mobile",   390, 844,  2.0),
]


def grab(pw, viewport_name, width, height, dsf):
    browser = pw.chromium.launch()
    ctx_kwargs = {
        "viewport": {"width": width, "height": height},
        "device_scale_factor": dsf,
        "ignore_https_errors": True,
    }
    if AUTH and ":" in AUTH:
        u, _, p = AUTH.partition(":")
        ctx_kwargs["http_credentials"] = {"username": u, "password": p}
    ctx = browser.new_context(**ctx_kwargs)
    page = ctx.new_page()
    js_errors = []
    page.on("pageerror", lambda exc: js_errors.append(str(exc)))
    page.on("console", lambda msg: js_errors.append(f"console.{msg.type}: {msg.text}") if msg.type == "error" else None)

    for url_path, slug, wait_ms in PAGES:
        url = BASE + url_path
        try:
            resp = page.goto(url, wait_until="domcontentloaded", timeout=10000)
            page.wait_for_timeout(wait_ms)
            target = OUT / f"{slug}_{viewport_name}.png"
            page.screenshot(path=str(target), full_page=True)
            size = target.stat().st_size
            print(f"  {viewport_name:7s} {url_path:32s} -> {target.relative_to(OUT.parent.parent.parent)} HTTP {resp.status} ({size//1024} KB)")
        except Exception as e:
            print(f"  {viewport_name:7s} {url_path:32s} FAILED: {e}", file=sys.stderr)

    if js_errors:
        print(f"  {viewport_name:7s} JS errors observed:")
        for e in js_errors:
            print(f"    - {e}")
    ctx.close()
    browser.close()


def main():
    print(f"Capturing /timers UX at {BASE}")
    t0 = time.time()
    with sync_playwright() as pw:
        for name, w, h, dsf in VIEWPORTS:
            print(f"[{name} {w}x{h} @{dsf}x]")
            grab(pw, name, w, h, dsf)
    print(f"Done in {time.time()-t0:.1f}s -> {OUT}")


if __name__ == "__main__":
    main()
