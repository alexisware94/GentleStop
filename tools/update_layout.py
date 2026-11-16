#!/usr/bin/env python3
"""Regenerate layout.json (and manifest.json total_package_size) for the GentleStop package.

MSFS only mounts files that are listed in layout.json, so run this after you drop
modules/gentlestop.wasm into the package (and after any other file change).

Usage:  python update_layout.py [path\\to\\alexisware94-gentlestop]
        (default: the folder next to this tools/ folder)
"""
import json, os, sys

pkg = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "alexisware94-gentlestop")
if not os.path.isfile(os.path.join(pkg, "manifest.json")):
    sys.exit("manifest.json not found in: " + pkg)

content, total = [], 0
for root, _dirs, files in os.walk(pkg):
    for name in files:
        full = os.path.join(root, name)
        rel = os.path.relpath(full, pkg).replace(os.sep, "/")
        if rel in ("layout.json", "manifest.json") or name.startswith("."):
            continue
        st = os.stat(full)
        filetime = int(st.st_mtime * 10_000_000) + 116444736000000000   # Windows FILETIME
        content.append({"path": rel.lower(), "size": st.st_size, "date": filetime})
        total += st.st_size
content.sort(key=lambda e: e["path"])

with open(os.path.join(pkg, "layout.json"), "w", encoding="utf-8") as f:
    json.dump({"content": content}, f, indent=2)
mpath = os.path.join(pkg, "manifest.json")
m = json.load(open(mpath, encoding="utf-8"))
m["total_package_size"] = str(total)
with open(mpath, "w", encoding="utf-8") as f:
    json.dump(m, f, indent=2)
print("layout.json: %d files, %d bytes" % (len(content), total))
for e in content: print("  ", e["path"])
