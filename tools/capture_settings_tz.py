#!/usr/bin/env python3
"""Capture /settings focused on the new Timezone dropdown (0.8.2)."""
import os
from pathlib import Path
from playwright.sync_api import sync_playwright

BASE = os.environ.get("PORTAL_URL", "http://192.168.22.100").rstrip("/")
AUTH = os.environ.get("PORTAL_AUTH", "").strip()
OUT = Path(__file__).resolve().parent.parent / "docs" / "screenshots" / "timers"
OUT.mkdir(parents=True, exist_ok=True)

with sync_playwright() as pw:
    for name, w, h, dsf in [("desktop", 1024, 1400, 1.0), ("mobile", 390, 1200, 2.0)]:
        browser = pw.chromium.launch()
        kw = {"viewport": {"width": w, "height": h}, "device_scale_factor": dsf}
        if AUTH and ":" in AUTH:
            u, _, p = AUTH.partition(":")
            kw["http_credentials"] = {"username": u, "password": p}
        ctx = browser.new_context(**kw)
        page = ctx.new_page()
        page.goto(f"{BASE}/settings", wait_until="domcontentloaded", timeout=10000)
        page.wait_for_timeout(400)
        # Scroll the TZ field into view + open the dropdown so we see the options
        try:
            page.locator("#tz_preset").scroll_into_view_if_needed(timeout=2000)
            page.wait_for_timeout(200)
        except Exception as e:
            print(f"  WARN: {e}")
        target = OUT / f"settings_tz_{name}.png"
        page.screenshot(path=str(target), full_page=False, clip={
            "x": 0,
            "y": max(0, page.locator("#tz_preset").bounding_box()["y"] - 100),
            "width": w,
            "height": min(500, h),
        })
        print(f"  {name:7s} -> {target.relative_to(OUT.parent.parent.parent)} ({target.stat().st_size//1024} KB)")
        ctx.close(); browser.close()
