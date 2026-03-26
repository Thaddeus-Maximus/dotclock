#!/usr/bin/env python3
"""Compress webpage.html into a C header for embedding."""

import gzip
import sys
from pathlib import Path

try:
    import minify_html
    HAS_MINIFY = True
except ImportError:
    HAS_MINIFY = False

def main():
    src = Path("webpage.html")
    if not src.exists():
        print(f"Error: {src} not found")
        sys.exit(1)

    html = src.read_text(encoding="utf-8")
    original = len(html.encode("utf-8"))

    if HAS_MINIFY:
        html = minify_html.minify(html, minify_js=True, minify_css=True,
                                  keep_comments=False, keep_closing_tags=True,
                                  remove_processing_instructions=True)

    raw = html.encode("utf-8")
    compressed = gzip.compress(raw, compresslevel=9)

    hex_bytes = ",".join(f"0x{b:02x}" for b in compressed)
    out = (
        f"// Auto-generated from webpage.html - do not edit\n"
        f"const char html_content[] = {{{hex_bytes}}};\n\n"
        f"const unsigned int html_content_len = {len(compressed)};\n"
    )
    Path("webpage.h").write_text(out)

    print(f"webpage.html: {original} -> {len(raw)} minified -> {len(compressed)} gzip -> webpage.h")

if __name__ == "__main__":
    main()
