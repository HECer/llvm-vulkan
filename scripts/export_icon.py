#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Optional SVG-to-PNG/ICO export; requires Playwright, its browser and Pillow."""
import argparse
from pathlib import Path
from playwright.sync_api import sync_playwright
from PIL import Image, PngImagePlugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser-path', help='Existing Chromium/Edge executable; no browser is downloaded')
    args = parser.parse_args()
    assets = Path(__file__).resolve().parents[1] / 'docs/assets'
    svg = (assets / 'icon.svg').read_text(encoding='utf-8')
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True, executable_path=args.browser_path)
        page = browser.new_page(viewport={'width': 256, 'height': 256}, offline=True)
        page.set_content('<style>html,body{margin:0;background:transparent}svg{width:256px;height:256px;display:block}</style>' + svg)
        page.locator('svg').screenshot(path=str(assets / 'icon.png'), omit_background=True)
        browser.close()
    with Image.open(assets / 'icon.png') as original:
        image = original.copy()
    metadata = PngImagePlugin.PngInfo()
    metadata.add_text('Description', 'LLVM Vulkan project icon. AI-assisted vector design, rasterized from icon.svg. Not an official LLVM or Khronos mark.')
    image.save(assets / 'icon.png', pnginfo=metadata)
    image.save(assets / 'favicon.ico', sizes=[(16, 16), (32, 32), (48, 48), (64, 64)])
    print('Exported transparent 256px PNG and multi-size ICO from canonical SVG.')


if __name__ == '__main__':
    main()
