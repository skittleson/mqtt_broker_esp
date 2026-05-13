#!/usr/bin/env python3
"""
Capture portal screenshots for UX review and README.

Usage:
    PORTAL_URL=http://192.168.22.100 python3 tools/capture_portal.py

Writes desktop (1024x) and mobile (390x844) PNGs into docs/screenshots/.
Also refreshes the legacy flat names used by README.md.
"""
import os
import sys
import time
from pathlib import Path
from playwright.sync_api import sync_playwright

BASE = os.environ.get("PORTAL_URL", "http://192.168.22.100").rstrip("/")
OUT = Path(__file__).resolve().parent.parent / "docs" / "screenshots"
AUDIT = OUT / "ux-audit"
OUT.mkdir(parents=True, exist_ok=True)
AUDIT.mkdir(parents=True, exist_ok=True)

# (path, slug, legacy_name_or_None, wait_ms, scroll_into_view_selector)
PAGES = [
    ("/",            "dashboard",       "dashboard.png",       300, None),
    ("/config",      "wifi_config",     "wifi_config.png",     300, None),
    ("/settings",    "settings",        "settings.png",        300, None),
    ("/clients",     "clients",         "clients.png",         600, None),
    ("/tester",      "tester",          None,                 1200, None),
    ("/information", "information",     "information.png",     300, None),
    ("/update",      "firmware_update", "firmware_update.png", 300, None),
]

VIEWPORTS = [
    ("desktop", 1024, 900,  1.0),
    ("mobile",   390, 844,  2.0),
]


def grab(pw, viewport_name, width, height, dsf):
    browser = pw.chromium.launch()
    ctx = browser.new_context(
        viewport={"width": width, "height": height},
        device_scale_factor=dsf,
        ignore_https_errors=True,
    )
    page = ctx.new_page()
    results = []
    for url_path, slug, legacy, wait_ms, _sel in PAGES:
        url = BASE + url_path
        try:
            page.goto(url, wait_until="domcontentloaded", timeout=10000)
            page.wait_for_timeout(wait_ms)
            target = AUDIT / f"{slug}_{viewport_name}.png"
            page.screenshot(path=str(target), full_page=True)
            # Refresh legacy flat name from the desktop capture only.
            if legacy and viewport_name == "desktop":
                page.screenshot(path=str(OUT / legacy), full_page=True)
            size = target.stat().st_size
            print(f"  {viewport_name:7s} {url_path:13s} -> {target.relative_to(OUT.parent.parent)} ({size//1024} KB)")
            results.append((slug, viewport_name, target))
        except Exception as e:
            print(f"  {viewport_name:7s} {url_path:13s} FAILED: {e}", file=sys.stderr)
    ctx.close()
    browser.close()
    return results


def main():
    print(f"Capturing portal at {BASE}")
    t0 = time.time()
    with sync_playwright() as pw:
        for name, w, h, dsf in VIEWPORTS:
            print(f"[{name} {w}x{h} @{dsf}x]")
            grab(pw, name, w, h, dsf)
    print(f"Done in {time.time()-t0:.1f}s -> {AUDIT}")


if __name__ == "__main__":
    main()
