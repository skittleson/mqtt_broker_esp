#!/usr/bin/env python3
"""
Capture an animated portal tour as a GIF for the README hero.

Usage:
    PORTAL_URL=http://192.168.22.100 \
    PORTAL_AUTH=user:password \
    python3 tools/capture_demo.py

Records a Playwright video of a scripted walk-through across the major
portal pages, then post-processes via ffmpeg into a looping GIF at
docs/screenshots/demo.gif (target ~2-4 MB).

Requires: playwright (with chromium installed) + ffmpeg on PATH.
"""
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from playwright.sync_api import sync_playwright

BASE = os.environ.get("PORTAL_URL", "http://192.168.22.100").rstrip("/")
AUTH = os.environ.get("PORTAL_AUTH", "").strip()
ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "docs" / "screenshots"
VIDEO_DIR = ROOT / "build" / "demo-video"
GIF_PATH = OUT_DIR / "demo.gif"

WIDTH, HEIGHT = 1024, 700
FPS = 10
GIF_WIDTH = 640  # downscale for README

# (path, dwell_ms, optional pre-action callable(page) -> None)
TOUR = [
    ("/",            1600, None),
    ("/clients",     2000, None),
    ("/timers",      1800, None),
    ("/timers/edit?n=1", 2000, None),
    ("/tester",      1600, None),
    ("/settings",    2000, None),
    ("/update",      1400, None),
    ("/",             900, None),
]


def need(cmd):
    if shutil.which(cmd) is None:
        sys.exit(f"error: '{cmd}' not found on PATH")


def gentle_scroll(page, dwell_ms):
    """Within dwell_ms, scroll smoothly to bottom then back to top."""
    half = max(dwell_ms // 2, 600)
    page.evaluate("""
        (dur) => new Promise(r => {
            const start = performance.now();
            const max = document.documentElement.scrollHeight - window.innerHeight;
            function step(t) {
                const k = Math.min(1, (t - start) / dur);
                window.scrollTo(0, max * k);
                if (k < 1) requestAnimationFrame(step); else r();
            }
            requestAnimationFrame(step);
        })
    """, half)
    page.wait_for_timeout(200)
    page.evaluate("window.scrollTo({top: 0, behavior: 'smooth'})")
    page.wait_for_timeout(half - 200)


def main():
    need("ffmpeg")
    if VIDEO_DIR.exists():
        shutil.rmtree(VIDEO_DIR)
    VIDEO_DIR.mkdir(parents=True, exist_ok=True)
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    print(f"Recording tour at {BASE}  ({WIDTH}x{HEIGHT})")
    t0 = time.time()
    with sync_playwright() as pw:
        browser = pw.chromium.launch()
        ctx_kwargs = {
            "viewport": {"width": WIDTH, "height": HEIGHT},
            "device_scale_factor": 1.0,
            "ignore_https_errors": True,
            "record_video_dir": str(VIDEO_DIR),
            "record_video_size": {"width": WIDTH, "height": HEIGHT},
        }
        if AUTH and ":" in AUTH:
            u, _, p = AUTH.partition(":")
            ctx_kwargs["http_credentials"] = {"username": u, "password": p}
        ctx = browser.new_context(**ctx_kwargs)
        page = ctx.new_page()

        for url_path, dwell_ms, pre in TOUR:
            url = BASE + url_path
            print(f"  -> {url_path}  ({dwell_ms} ms)")
            try:
                page.goto(url, wait_until="domcontentloaded", timeout=10000)
                page.wait_for_timeout(400)
                if pre:
                    pre(page)
                # If page is taller than viewport, do a gentle scroll so
                # the GIF reveals content below the fold.
                tall = page.evaluate(
                    "document.documentElement.scrollHeight > window.innerHeight + 40"
                )
                if tall:
                    gentle_scroll(page, dwell_ms)
                else:
                    page.wait_for_timeout(dwell_ms)
            except Exception as e:
                print(f"     FAILED: {e}", file=sys.stderr)

        ctx.close()
        browser.close()

    # Find the produced .webm
    webms = sorted(VIDEO_DIR.glob("*.webm"))
    if not webms:
        sys.exit("error: no .webm produced by Playwright")
    src = webms[-1]
    print(f"Recorded {src.name} ({src.stat().st_size//1024} KB) in {time.time()-t0:.1f}s")

    # Convert to GIF via the two-pass palette technique.
    palette = VIDEO_DIR / "palette.png"
    print("Generating palette...")
    subprocess.run([
        "ffmpeg", "-y", "-i", str(src),
        "-vf", f"fps={FPS},scale={GIF_WIDTH}:-1:flags=lanczos,palettegen=stats_mode=diff",
        str(palette),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print(f"Encoding GIF -> {GIF_PATH.relative_to(ROOT)}")
    subprocess.run([
        "ffmpeg", "-y", "-i", str(src), "-i", str(palette),
        "-lavfi",
        f"fps={FPS},scale={GIF_WIDTH}:-1:flags=lanczos[v];[v][1:v]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle",
        "-loop", "0",
        str(GIF_PATH),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    size_kb = GIF_PATH.stat().st_size // 1024
    print(f"Done: {GIF_PATH.relative_to(ROOT)} ({size_kb} KB)")
    if size_kb > 6000:
        print("warning: GIF >6 MB. Consider lowering FPS or GIF_WIDTH.", file=sys.stderr)

    # Also emit an MP4 alongside (smaller, plays inline on GitHub via <video>).
    mp4_path = OUT_DIR / "demo.mp4"
    print(f"Encoding MP4 -> {mp4_path.relative_to(ROOT)}")
    subprocess.run([
        "ffmpeg", "-y", "-i", str(src),
        "-vf", f"fps={FPS},scale={GIF_WIDTH}:-2:flags=lanczos",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "26",
        "-movflags", "+faststart", "-an",
        str(mp4_path),
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"Done: {mp4_path.relative_to(ROOT)} ({mp4_path.stat().st_size//1024} KB)")


if __name__ == "__main__":
    main()
