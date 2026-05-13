#!/usr/bin/env python3
"""
Capture the reboot-countdown page as it appears after a /save-settings POST,
and the /settings form with a mocked-up overlay of the confirm() prompt.

Both captures avoid actually rebooting the device:
  - The countdown view reuses the read-only /rebooting endpoint and rewrites
    the title/subtitle via DOM so they match what the save flow emits.
  - The confirm view captures the page with a synthesized overlay of the
    native confirm() text (browsers don't let Playwright screenshot real
    JS dialogs).

Usage: PORTAL_URL=http://192.168.22.100 python3 tools/capture_save_reboot.py
"""
import os
from pathlib import Path
from playwright.sync_api import sync_playwright

BASE = os.environ.get("PORTAL_URL", "http://192.168.22.100").rstrip("/")
AUTH = os.environ.get("PORTAL_AUTH", "").strip()
OUT = Path(__file__).resolve().parent.parent / "docs" / "screenshots" / "ux-audit"
OUT.mkdir(parents=True, exist_ok=True)

CONFIRM_MSG = (
    "Save settings and reboot? The device will restart and should be "
    "reachable again in about 10 seconds."
)


def main():
    with sync_playwright() as pw:
        b = pw.chromium.launch()
        if AUTH and ":" in AUTH:
            u, _, p = AUTH.partition(":")
            ctx = b.new_context(
                viewport={"width": 1024, "height": 1050},
                http_credentials={"username": u, "password": p},
            )
        else:
            ctx = b.new_context(viewport={"width": 1024, "height": 1050})
        page = ctx.new_page()

        # 1. Countdown view, retitled to match the /save-settings flow.
        # Block /api/ping (new in 0.6.4 -- the unauthenticated liveness
        # endpoint that replaced /api/status for polling).
        page.route("**/api/ping", lambda r: r.abort())
        page.goto(BASE + "/rebooting", wait_until="domcontentloaded")
        page.evaluate(
            "document.title='Saving and rebooting';"
            "document.querySelector('h2').textContent='Saving and rebooting';"
            "document.querySelector('.sub').textContent="
            "'Saved. Polling device \\u2014 will redirect home when it comes back online.';"
        )
        page.wait_for_timeout(2200)
        page.screenshot(path=str(OUT / "save_reboot_countdown.png"), full_page=True)
        print(f"  save_reboot_countdown.png written")

        # 2. Settings form with synthesized confirm-dialog overlay.
        page.unroute("**/api/ping")
        page.goto(BASE + "/settings", wait_until="domcontentloaded")
        page.wait_for_timeout(300)
        # Scroll the Save button into view so the overlay covers it
        # contextually.
        page.evaluate(
            "document.querySelector('button[type=submit]').scrollIntoView({block:'center'});"
        )
        # Append an overlay that mimics a browser confirm() dialog so the
        # screenshot conveys the wording. Real native dialogs are out of
        # Playwright's capture surface.
        page.evaluate(
            """(msg) => {
                const o = document.createElement('div');
                o.style.cssText =
                  'position:fixed;top:32%;left:50%;transform:translateX(-50%);'
                  +'background:#fff;color:#222;padding:18px 22px;border-radius:6px;'
                  +'box-shadow:0 8px 32px rgba(0,0,0,.6);font-family:sans-serif;'
                  +'max-width:440px;z-index:9999;font-size:0.95em;line-height:1.45';
                o.innerHTML =
                  '<div style="color:#666;font-size:0.78em;margin-bottom:8px">'
                  +'Browser confirm() prompt</div>'
                  +'<div></div>'
                  +'<div style="text-align:right;margin-top:16px">'
                  +'<span style="padding:6px 14px;background:#eee;'
                  +'border-radius:4px;margin-right:6px">Cancel</span>'
                  +'<span style="padding:6px 14px;background:#1fa3ec;color:#fff;'
                  +'border-radius:4px">OK</span></div>';
                o.children[1].textContent = msg;
                document.body.appendChild(o);
            }""",
            CONFIRM_MSG,
        )
        page.wait_for_timeout(200)
        page.screenshot(path=str(OUT / "save_reboot_confirm.png"), full_page=True)
        print(f"  save_reboot_confirm.png written")

        ctx.close()
        b.close()


if __name__ == "__main__":
    main()
