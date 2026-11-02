#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Build the project's editable SVG monogram and light/dark wordmarks."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'docs' / 'assets'


def mark():
    return '''<rect x="1" y="1" width="254" height="254" rx="46" fill="#0b1020" stroke="#25304b" stroke-width="2"/>
<path d="M32 64h32v98h48l12 32H32z" fill="#6484ff"/>
<path d="M96 64h36l28 79 28-79h36l-48 130h-32z" fill="#20d3b0"/>'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    description = 'Geometric LV monogram for LLVM Vulkan. AI-assisted vector design; not an official LLVM or Khronos mark.'
    icon = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" role="img" aria-labelledby="title desc">
<title id="title">LLVM Vulkan icon</title><desc id="desc">{description}</desc>
{mark()}
</svg>\n'''
    (OUT / 'icon.svg').write_text(icon, encoding='utf-8', newline='\n')
    for theme, ink, secondary in [('dark', '#eef3ff', '#a5b3d2'), ('light', '#101a35', '#40516d')]:
        svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 560 192" role="img" aria-labelledby="title desc">
<title id="title">LLVM Vulkan</title><desc id="desc">{description}</desc>
<g transform="scale(.75)">{mark()}</g>
<g font-family="Segoe UI,Arial,sans-serif"><text x="224" y="91" font-size="76" font-weight="700" letter-spacing="-3" fill="{ink}">LLVM</text>
<text x="228" y="143" font-size="35" font-weight="600" letter-spacing="7" fill="{secondary}">VULKAN</text></g>
</svg>\n'''
        (OUT / f'logo-{theme}.svg').write_text(svg, encoding='utf-8', newline='\n')
    print('Built editable icon and two wordmarks.')


if __name__ == '__main__':
    main()
