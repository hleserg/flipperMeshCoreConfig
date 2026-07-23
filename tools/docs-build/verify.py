#!/usr/bin/env python3
"""
Verify the built site against the promises it makes.

The site's whole job is to open on a phone with no signal, so "no external
requests" is not a style preference — one stray CDN link and the page is blank
in a field. This checks that mechanically instead of by eye.

    python tools/docs-build/verify.py
"""

from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOCS = os.path.join(ROOT, "docs")

PAGES = ["index.html", "app-guide.html", "guide-sergey.html", "guide-mark.html"]

failures: list[str] = []
checks = 0


def check(condition: bool, description: str, detail: str = "") -> None:
    global checks
    checks += 1
    mark = "ok  " if condition else "FAIL"
    if not condition:
        failures.append(description)
    print(f"  {mark} {description}{(' -> ' + detail) if detail else ''}")


def read(name: str) -> str:
    with open(os.path.join(DOCS, name), encoding="utf-8") as handle:
        return handle.read()


def main() -> int:
    print("docs site verification\n")

    for name in PAGES:
        check(os.path.exists(os.path.join(DOCS, name)), f"{name} exists")

    for name in ["sw.js", "manifest.webmanifest", "icon.svg", "meshlog.py", ".nojekyll"]:
        check(os.path.exists(os.path.join(DOCS, name)), f"{name} exists")

    print()

    # --- the offline promise: nothing may be *loaded* from outside.
    # Links a reader can tap are fine; resources the page fetches are not.
    resource_attrs = re.compile(
        r'<(?:script|img|iframe|source|video|audio)\b[^>]*\bsrc\s*=\s*["\']([^"\']+)',
        re.I,
    )
    stylesheet = re.compile(
        r'<link\b[^>]*\brel\s*=\s*["\'](?:stylesheet|preload|prefetch)["\'][^>]*\bhref\s*=\s*["\']([^"\']+)',
        re.I,
    )
    css_import = re.compile(r"@import\b|url\(\s*['\"]?https?:", re.I)

    for name in PAGES:
        text = read(name)
        external = [u for u in resource_attrs.findall(text) if u.startswith(("http://", "https://", "//"))]
        external += [u for u in stylesheet.findall(text) if u.startswith(("http://", "https://", "//"))]
        check(not external, f"{name}: no external resources", str(external[:3]) if external else "")
        check(not css_import.search(text), f"{name}: no @import or remote url() in CSS")
        # Everything must be inlined, so there should be no local resource
        # references either apart from the manifest and the download link.
        check("<style>" in text, f"{name}: CSS is inlined")
        check("<script>" in text, f"{name}: JS is inlined")

    print()

    # --- copy button on every code block
    for name in PAGES:
        text = read(name)
        blocks = len(re.findall(r"<pre>", text))
        has_js = "document.querySelectorAll('pre')" in text
        check(has_js, f"{name}: copy-button script present", f"{blocks} code blocks")

    print()

    # --- service worker caches what the site offers
    sw = read("sw.js")
    for path in ["./index.html", "./app-guide.html", "./guide-sergey.html", "./guide-mark.html", "./meshlog.py"]:
        check(f'"{path}"' in sw, f"service worker caches {path}")
    check('scope: \'./\'' in read("index.html"), "service worker registered with a relative scope")
    check('"scope": "./"' in read("manifest.webmanifest"), "manifest scope is relative")
    check('"start_url": "./index.html"' in read("manifest.webmanifest"), "manifest start_url is relative")

    print()

    # --- the hub reaches everything
    index = read("index.html")
    for target in ["app-guide.html", "guide-sergey.html", "guide-mark.html", "meshlog.py"]:
        check(f'href="{target}"' in index, f"hub links {target}")

    # --- no broken internal links
    internal = set()
    for name in PAGES:
        for href in re.findall(r'href="([^"#][^"]*)"', read(name)):
            if href.startswith(("http://", "https://", "mailto:")):
                continue
            internal.add(href.split("#")[0])
    missing = [h for h in sorted(internal) if h and not os.path.exists(os.path.join(DOCS, h))]
    check(not missing, "no broken internal links", str(missing) if missing else "")

    print()

    # --- Mark's guide must keep pointing at the official flasher
    mark = read("guide-mark.html")
    check("flasher.meshcore.io" in mark, "Mark's guide still points at flasher.meshcore.io")
    check(
        "meshcore-node-fw" not in mark,
        "Mark's guide does NOT point at our flasher",
    )
    check('href="meshlog.py"' in mark, "Mark's guide offers meshlog.py for download")

    # Sergey's guide splits the two flashers deliberately.
    sergey = read("guide-sergey.html")
    check("meshcore-node-fw" in sergey, "Sergey's guide points at our flasher for the rovers")
    check("flasher.meshcore.io" in sergey, "Sergey's guide points at the official one for repeaters")
    check("Trace Path" in sergey, "Sergey's guide keeps the honest Trace Path gap")

    print()

    # --- content survived the rendering, counted against the source rather
    # than against a guessed floor: a section quietly lost in conversion is
    # exactly the failure this is here to catch.
    src_dir = os.path.join(ROOT, "docs-src")
    for page_name, source_name in [
        ("guide-mark.html", "guide-mark.md"),
        ("guide-sergey.html", "guide-sergey.md"),
        ("app-guide.html", "app-guide.md"),
    ]:
        with open(os.path.join(src_dir, source_name), encoding="utf-8") as handle:
            source = handle.read()
        text = read(page_name)

        src_tables = len(re.findall(r"^[ \t]*\|[ :|\-]+\|[ \t]*$", source, re.M))
        out_tables = len(re.findall(r"<table>", text))
        check(
            out_tables == src_tables,
            f"{page_name}: every table survived",
            f"{out_tables}/{src_tables}",
        )

        src_headings = len(re.findall(r"^#{1,4} ", source, re.M))
        out_headings = len(re.findall(r"<h[1-4]\b", text))
        check(
            out_headings == src_headings,
            f"{page_name}: every heading survived",
            f"{out_headings}/{src_headings}",
        )

        # Fences also occur inside blockquotes, where the marker is preceded by
        # "> " -- counting only column-zero fences undercounts and makes a
        # correct render look broken.
        src_code = len(re.findall(r"^>?[ \t]*```", source, re.M)) // 2
        out_code = len(re.findall(r"<pre>", text))
        check(
            out_code == src_code,
            f"{page_name}: every code block survived",
            f"{out_code}/{src_code}",
        )

    print(f"\n{checks} checks, {len(failures)} failures")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
