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

Optional auth (when the portal has Basic Auth enabled):
    PORTAL_URL=... PORTAL_AUTH=user:password python3 tools/capture_reboot.py

The AUTH env var is read in-process only -- never written to commits or
image files.
"""
import os, time
from pathlib import Path
from playwright.sync_api import sync_playwright

BASE = os.environ.get("PORTAL_URL", "http://192.168.22.100").rstrip("/")
AUTH = os.environ.get("PORTAL_AUTH", "").strip()
OUT = Path(__file__).resolve().parent.parent / "docs" / "screenshots" / "ux-audit"
OUT.mkdir(parents=True, exist_ok=True)


def main():
    with sync_playwright() as pw:
        b = pw.chromium.launch()
        if AUTH and ":" in AUTH:
            u, _, p = AUTH.partition(":")
            ctx = b.new_context(
                viewport={"width": 1024, "height": 700},
                http_credentials={"username": u, "password": p},
            )
        else:
            ctx = b.new_context(viewport={"width": 1024, "height": 700})
        page = ctx.new_page()

        # /api/ping is the new liveness endpoint used by the countdown JS
        # (was /api/status before 0.6.4 -- changed to bypass Basic Auth).
        # Block it to keep the page in 'Device offline...' for the offline
        # capture; unblock for the back-online capture.
        page.route("**/api/ping", lambda r: r.abort())
        page.goto(BASE + "/rebooting", wait_until="domcontentloaded")
        # Let a few aborted polls flow through; the catch handler
        # flips the text to 'Device offline, will resume when it returns...'
        page.wait_for_timeout(2500)
        page.screenshot(path=str(OUT / "rebooting_offline.png"), full_page=True)
        print(f"  rebooting_offline.png written")

        # Unblock /api/ping. After the existing 'seenOffline=true' flag, the
        # very next successful response triggers backOnline() + the 400ms
        # auto-redirect to /. Capture mid-transition before the redirect
        # fires by intercepting navigation.
        page.unroute("**/api/ping")
        # Snapshot the brief 'Back online - redirecting...' state by racing
        # against the 400ms redirect timer. Polling is on a 1s cadence, so
        # the next poll arrives within ~1s.
        page.wait_for_timeout(1200)
        try:
            page.screenshot(path=str(OUT / "rebooting_backonline.png"), full_page=True)
            print(f"  rebooting_backonline.png written")
        except Exception as e:
            # Likely already redirected to /. That IS the back-online state
            # we want to document, so capture wherever we landed.
            page.screenshot(path=str(OUT / "rebooting_backonline.png"), full_page=True)
            print(f"  rebooting_backonline.png written (post-redirect)")

        ctx.close()
        b.close()


if __name__ == "__main__":
    main()
