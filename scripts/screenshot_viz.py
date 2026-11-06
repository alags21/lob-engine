#!/usr/bin/env python3
"""Take a headless-browser screenshot of the visualizer (Edge or Chrome/Chromium, whichever is found).

  python scripts/screenshot_viz.py docs/img/viz-sample.png [--at 0.5] [--width 1300] [--height 1500]

Loads viz/index.html over file:// at a fixed playback position (paused) and saves a PNG. Used only to
check that the page renders in a real browser; the automated checks are in viz/test/viz_test.js.
"""
import argparse, os, shutil, subprocess, sys, time, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe", r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe", "google-chrome", "chromium", "chromium-browser", "chrome", "msedge",
]


def find_browser():
    for c in CANDIDATES:
        p = c if os.path.isabs(c) and os.path.exists(c) else shutil.which(c)
        if p:
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--at", default="0.5")
    ap.add_argument("--width", type=int, default=1300)
    ap.add_argument("--height", type=int, default=1500)
    a = ap.parse_args()
    b = find_browser()
    if not b:
        sys.exit("no Chromium-based browser found")
    out = pathlib.Path(a.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    url = (ROOT / "viz" / "index.html").as_uri() + f"#at={a.at}&play=0"
    subprocess.run([b, "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--window-size={a.width},{a.height}",
                    "--virtual-time-budget=4000", f"--screenshot={out}", url], check=True, timeout=120)
    for _ in range(50):  # the browser may finish writing the file just after it exits
        if out.exists() and out.stat().st_size > 0:
            break
        time.sleep(0.2)
    if not out.exists():
        sys.exit("screenshot was not written")
    print("wrote", out, out.stat().st_size, "bytes using", b)


if __name__ == "__main__":
    main()
