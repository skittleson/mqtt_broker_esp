#!/usr/bin/env python3
"""
Capture the new reboot-countdown UI in three states:
  1. Initial:        page loaded, first poll hasn't run yet
  2. Waiting:        device offline (polling failed)
  3. Back online:    polling succeeded after the offline edge

State 2/3 are simulated by intercepting /api/status on the Playwright page so
we don't actually need to reboot the device. The page DOM at the moment of
each transition is screenshotted.

Usage: PORTAL_URL=http://192.168.22.100 python3 tools/capture_reboot.py
"""
import os, time
from pathlib import Path
from playwright.sync_api import sync_playwright

BASE = os.environ.get("PORTAL_URL", "http://192.168.22.100").rstrip("/")
OUT = Path(__file__).resolve().parent.parent / "docs" / "screenshots" / "ux-audit"
OUT.mkdir(parents=True, exist_ok=True)


def main():
    with sync_playwright() as pw:
        b = pw.chromium.launch()
        ctx = b.new_context(viewport={"width": 1024, "height": 700})
        page = ctx.new_page()

        # State 1 — initial render of /rebooting. Block /api/status so the
        # spinner stays in 'Waiting for device...' instead of immediately
        # detecting the device is still up.
        page.route("**/api/status", lambda r: r.abort())
        page.goto(BASE + "/rebooting", wait_until="domcontentloaded")
        page.wait_for_timeout(500)
        page.screenshot(path=str(OUT / "rebooting_initial.png"), full_page=True)
        print(f"  rebooting_initial.png written")

        # State 2 — let a few aborted polls flow through; the catch handler
        # flips the text to 'Device offline, will resume when it returns...'
        page.wait_for_timeout(2500)
        page.screenshot(path=str(OUT / "rebooting_offline.png"), full_page=True)
        print(f"  rebooting_offline.png written")

        # State 3 — unblock /api/status. After the existing 'seenOffline=true'
        # flag, the very next successful response should flip the UI green.
        page.unroute("**/api/status")
        page.wait_for_timeout(1500)
        page.screenshot(path=str(OUT / "rebooting_backonline.png"), full_page=True)
        print(f"  rebooting_backonline.png written")

        ctx.close()
        b.close()


if __name__ == "__main__":
    main()
