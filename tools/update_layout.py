#!/usr/bin/env python3
"""Validate the GentleStop package layout, or deliberately regenerate it.

The default operation is READ-ONLY validation. It parses manifest.json and layout.json,
checks that every layout entry resolves to a real package file, compares the recorded sizes,
reports path-capitalization differences, and reports package files that layout.json does not
list. It never writes anything.

Regenerating layout.json / manifest.json requires an explicit write request:

    python tools/update_layout.py --write --confirm-write
    python tools/update_layout.py --write --confirm-write D:\\scratch\\my-package-copy

Write mode is refused for the canonical package in this repository unless
--allow-canonical-write is also given, because gentlestop-stage-panel\\layout.json and
gentlestop-stage-panel\\manifest.json are reviewed release artifacts.

Path-casing policy: entries that already exist in layout.json keep their exact recorded path
string when the corresponding file is still present. Only new entries use the on-disk spelling.
On-disk paths are matched case-insensitively, because the reviewed layout intentionally uses
lowercase strings that differ from the Git-tracked file names; that difference is reported but
never normalized automatically.

Exit codes:
    0  validation passed (warnings may have been reported)
    1  validation failed, or a write operation could not complete
    2  usage error, or the requested package directory is not usable
"""

import argparse
import json
import os
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
CANONICAL_PACKAGE = os.path.join(REPO_ROOT, "gentlestop-stage-panel")

# layout.json / manifest.json describe the package; they are not package assets themselves.
SKIPPED_ROOT_FILES = ("layout.json", "manifest.json")
REQUIRED_MANIFEST_KEYS = ("title", "creator", "package_version", "export_type")


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _norm(path):
    return path.replace("\\", "/")


def _same_path(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def resolve_package(argument):
    """Return the package directory to inspect.

    With no argument the canonical package from this repository is used, resolved from the
    script location so the tool behaves the same from any current directory. An explicit
    relative argument is resolved against the current directory first and then against the
    repository root.
    """
    if not argument:
        return CANONICAL_PACKAGE
    candidate = os.path.abspath(os.path.expanduser(argument))
    if os.path.isdir(candidate):
        return candidate
    from_repo = os.path.abspath(os.path.join(REPO_ROOT, argument))
    if os.path.isdir(from_repo):
        return from_repo
    return candidate


def list_package_files(pkg):
    """Every non-hidden file in the package, as sorted forward-slash relative paths."""
    files = []
    for root, dirs, names in os.walk(pkg):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(names):
            if name.startswith("."):
                continue
            full = os.path.join(root, name)
            rel = os.path.relpath(full, pkg).replace(os.sep, "/")
            files.append(rel)
    files.sort(key=str.lower)
    return files


def read_json(path, label, errors):
    try:
        # utf-8-sig tolerates the BOM that some Windows editors add; the canonical files have none.
        with open(path, "r", encoding="utf-8-sig") as handle:
            return json.load(handle)
    except json.JSONDecodeError as exc:
        errors.append("%s: invalid JSON (%s)" % (label, exc))
    except UnicodeDecodeError as exc:
        errors.append("%s: not valid UTF-8 text (%s)" % (label, exc))
    except OSError as exc:
        errors.append("%s: cannot read (%s)" % (label, exc))
    return None


def validate(pkg):
    """Read-only inspection. Returns a result dictionary; never writes."""
    result = {
        "package": pkg,
        "fatal": False,
        "errors": [],
        "warnings": [],
        "entries": [],
        "manifest": None,
        "layout": None,
        "actual_files": [],
    }

    def err(message):
        result["errors"].append(message)

    def warn(message):
        result["warnings"].append(message)

    if not os.path.isdir(pkg):
        err("package directory not found: %s" % pkg)
        result["fatal"] = True
        return result

    actual_files = list_package_files(pkg)
    result["actual_files"] = actual_files
    exact = set(actual_files)
    lower_actual = {}
    for rel in actual_files:
        lower_actual.setdefault(rel.lower(), rel)

    # --- manifest.json -----------------------------------------------------------------
    manifest_path = os.path.join(pkg, "manifest.json")
    manifest = None
    if not os.path.isfile(manifest_path):
        err("manifest.json not found in %s" % pkg)
    else:
        manifest = read_json(manifest_path, "manifest.json", result["errors"])
    result["manifest"] = manifest
    if isinstance(manifest, dict):
        for key in REQUIRED_MANIFEST_KEYS:
            value = manifest.get(key)
            if not isinstance(value, str) or not value.strip():
                err("manifest.json: required property %r is missing or is not a non-empty string" % key)
        total_property = manifest.get("total_package_size")
        if total_property is not None and not isinstance(total_property, str):
            warn("manifest.json: total_package_size is %s, expected a string" % type(total_property).__name__)
    elif manifest is not None:
        err("manifest.json: top-level value must be a JSON object")

    # --- layout.json -------------------------------------------------------------------
    layout_path = os.path.join(pkg, "layout.json")
    layout = None
    if not os.path.isfile(layout_path):
        err("layout.json not found in %s" % pkg)
    else:
        layout = read_json(layout_path, "layout.json", result["errors"])
    result["layout"] = layout

    content = None
    if isinstance(layout, dict):
        content = layout.get("content")
        if not isinstance(content, list):
            err("layout.json: property 'content' is missing or is not a list")
            content = None
    elif layout is not None:
        err("layout.json: top-level value must be a JSON object")

    seen = {}
    if content is not None:
        for index, entry in enumerate(content):
            prefix = "layout.json content[%d]" % index
            if not isinstance(entry, dict):
                err("%s: entry is not a JSON object" % prefix)
                continue
            path = entry.get("path")
            size = entry.get("size")
            date = entry.get("date")
            entry_ok = True
            if not isinstance(path, str) or not path.strip():
                err("%s: 'path' is missing or is not a non-empty string" % prefix)
                entry_ok = False
            if not _is_int(size) or size < 0:
                err("%s: 'size' is missing or is not a non-negative integer" % prefix)
                entry_ok = False
            if not _is_int(date):
                err("%s: 'date' is missing or is not an integer" % prefix)
                entry_ok = False
            if not entry_ok:
                continue

            normalized = _norm(path)
            if normalized.lower() in seen:
                warn("%s: duplicate entry for %s" % (prefix, path))
            seen[normalized.lower()] = True

            status = {"path": path, "recorded_size": size, "state": "ok", "actual_path": normalized,
                      "actual_size": None}
            if normalized in exact:
                status["actual_path"] = normalized
            else:
                matched = lower_actual.get(normalized.lower())
                if matched:
                    status["state"] = "case"
                    status["actual_path"] = matched
                    warn("case-only path difference: layout records %r, on disk %r "
                         "(preserved, not normalized; a case-insensitive lookup does not prove "
                         "MSFS runtime compatibility)" % (path, matched))
                else:
                    status["state"] = "missing"
                    status["actual_path"] = None
                    err("listed file not found: %s" % path)
            if status["actual_path"]:
                full = os.path.join(pkg, *status["actual_path"].split("/"))
                try:
                    actual_size = os.path.getsize(full)
                except OSError as exc:
                    err("cannot read %s (%s)" % (status["actual_path"], exc))
                    actual_size = None
                status["actual_size"] = actual_size
                if actual_size is not None and actual_size != size:
                    err("size mismatch: %s records %d bytes, file has %d bytes"
                        % (path, size, actual_size))
            result["entries"].append(status)

        listed = set(seen)
        for rel in actual_files:
            if rel in SKIPPED_ROOT_FILES:
                continue
            if rel.lower() not in listed:
                warn("file not listed in layout.json: %s" % rel)

        if isinstance(manifest, dict):
            total_property = manifest.get("total_package_size")
            if isinstance(total_property, str):
                try:
                    recorded_total = int(total_property)
                except ValueError:
                    warn("manifest.json: total_package_size %r is not an integer string" % total_property)
                else:
                    computed_total = sum(entry["recorded_size"] for entry in result["entries"]
                                         if "recorded_size" in entry)
                    if recorded_total != computed_total:
                        warn("manifest.json: total_package_size %d does not match the sum of "
                             "layout.json sizes %d" % (recorded_total, computed_total))

    return result


def print_report(result):
    pkg = result["package"]
    entries = result["entries"]
    manifest = result["manifest"]
    layout = result["layout"]

    print("Package      : %s" % pkg)
    print("Canonical    : %s" % ("yes" if _same_path(pkg, CANONICAL_PACKAGE) else
                                 "no (not this repository's gentlestop-stage-panel)"))
    if isinstance(manifest, dict):
        print("Manifest     : OK  title=%r creator=%r package_version=%r export_type=%r"
              % (manifest.get("title"), manifest.get("creator"),
                 manifest.get("package_version"), manifest.get("export_type")))
    else:
        print("Manifest     : INVALID or missing")
    if isinstance(layout, dict) and isinstance(layout.get("content"), list):
        print("Layout       : OK  %d entr%s" % (len(layout["content"]),
                                                "y" if len(layout["content"]) == 1 else "ies"))
    else:
        print("Layout       : INVALID or missing")

    if entries:
        print("Entries      :")
        for entry in entries:
            if entry["state"] == "ok":
                print("  OK     %s (%d bytes)" % (entry["path"], entry["recorded_size"]))
            elif entry["state"] == "case":
                print("  CASE   %s -> %s (%d bytes)" % (entry["path"], entry["actual_path"],
                                                        entry["recorded_size"]))
            else:
                print("  MISS   %s (not found on disk)" % entry["path"])
            if entry["state"] != "missing" and entry["actual_size"] is not None \
                    and entry["actual_size"] != entry["recorded_size"]:
                print("         size mismatch: layout %d, file %d"
                      % (entry["recorded_size"], entry["actual_size"]))

    if result["warnings"]:
        print("Warnings     : %d" % len(result["warnings"]))
        for warning in result["warnings"]:
            print("  WARN  %s" % warning)
    if result["errors"]:
        print("Errors       : %d" % len(result["errors"]))
        for error in result["errors"]:
            print("  ERROR %s" % error)


def write_json_files(pairs):
    """Write several JSON files as safely as practical.

    Every payload is first serialized to a temporary file in the destination directory; only when
    all temporaries exist are they moved into place with os.replace. A serialization failure
    therefore leaves the originals untouched.
    """
    temporaries = []
    try:
        for path, data in pairs:
            directory = os.path.dirname(os.path.abspath(path)) or "."
            handle, temp_path = tempfile.mkstemp(prefix=".gentlestop-tmp-", dir=directory)
            temporaries.append((temp_path, path))
            with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
                json.dump(data, stream, indent=2)
        for temp_path, path in temporaries:
            os.replace(temp_path, path)
        temporaries = []
    finally:
        for temp_path, _path in temporaries:
            try:
                os.unlink(temp_path)
            except OSError:
                pass


def generate(result, allow_canonical_write):
    """Regenerate layout.json and manifest.json total_package_size.

    Returns an exit code. Structural problems abort before any file is touched.
    """
    pkg = result["package"]

    if _same_path(pkg, CANONICAL_PACKAGE) and not allow_canonical_write:
        print("Refusing to write the canonical package (%s)." % pkg)
        print("gentlestop-stage-panel\\layout.json and manifest.json are reviewed release artifacts.")
        print("Test write mode on a copy outside the repository, or pass --allow-canonical-write "
              "after an explicit release review.")
        return 2

    manifest = result["manifest"]
    if not isinstance(manifest, dict):
        print("Cannot write: manifest.json is missing or is not a JSON object.")
        return 1
    for key in REQUIRED_MANIFEST_KEYS:
        value = manifest.get(key)
        if not isinstance(value, str) or not value.strip():
            print("Cannot write: manifest.json property %r is missing or invalid." % key)
            return 1

    for entry in result["entries"]:
        if entry["state"] == "missing":
            print("Cannot write: layout.json lists a file that is not in the package: %s" % entry["path"])
            print("Restore or remove that file deliberately before regenerating the layout.")
            return 1
    structural_errors = []
    for error in result["errors"]:
        if error.startswith("layout.json not found"):
            continue  # write mode may create a first layout.json
        if error.startswith("layout.json") or error.startswith("manifest.json"):
            structural_errors.append(error)
    if structural_errors:
        print("Cannot write: the package metadata is structurally invalid:")
        for error in structural_errors:
            print("  ERROR %s" % error)
        return 1

    existing_layout = result["layout"] if isinstance(result["layout"], dict) else {}
    existing_entries = existing_layout.get("content") if isinstance(existing_layout.get("content"), list) else []
    existing_by_lower = {}
    for entry in existing_entries:
        if isinstance(entry, dict) and isinstance(entry.get("path"), str):
            existing_by_lower.setdefault(entry["path"].lower(), entry["path"])

    content = []
    total = 0
    for rel in result["actual_files"]:
        if rel in SKIPPED_ROOT_FILES:
            continue
        full = os.path.join(pkg, *rel.split("/"))
        stat = os.stat(full)
        filetime = int(stat.st_mtime * 10_000_000) + 116444736000000000  # Windows FILETIME
        recorded_path = existing_by_lower.get(rel.lower(), rel)
        content.append({"path": recorded_path, "size": stat.st_size, "date": filetime})
        total += stat.st_size
    content.sort(key=lambda entry: entry["path"].lower())

    old_by_lower = {}
    for entry in existing_entries:
        if isinstance(entry, dict) and isinstance(entry.get("path"), str):
            old_by_lower.setdefault(entry["path"].lower(), entry)
    new_lower = {entry["path"].lower() for entry in content}
    added = [entry for entry in content if entry["path"].lower() not in old_by_lower]
    removed = [entry for entry in old_by_lower.values() if entry["path"].lower() not in new_lower]
    resized = []
    for entry in content:
        previous = old_by_lower.get(entry["path"].lower())
        if previous is not None and previous.get("size") != entry["size"]:
            resized.append((entry["path"], previous.get("size"), entry["size"]))

    old_total = None
    try:
        old_total = int(manifest.get("total_package_size"))
    except (TypeError, ValueError):
        pass

    print("Write target : %s" % pkg)
    print("The findings above identify the problems this write resolves; structural problems abort it.")
    print("This modifies layout.json and manifest.json (total_package_size) only.")
    print("  entries    : %d (%d added, %d removed, %d size changes)"
          % (len(content), len(added), len(removed), len(resized)))
    for entry in added:
        print("  ADD    %s (%d bytes)" % (entry["path"], entry["size"]))
    for entry in removed:
        print("  REMOVE %s" % entry["path"])
    for path, old_size, new_size in resized:
        print("  SIZE   %s: %s -> %d" % (path, old_size, new_size))
    print("  total      : %s -> %d bytes" % (old_total if old_total is not None else "?", total))

    new_manifest = dict(manifest)
    new_manifest["total_package_size"] = str(total)
    try:
        write_json_files([
            (os.path.join(pkg, "layout.json"), {"content": content}),
            (os.path.join(pkg, "manifest.json"), new_manifest),
        ])
    except OSError as exc:
        print("Write failed: %s" % exc)
        return 1

    print("layout.json: written (%d entries, %d bytes total)" % (len(content), total))
    print("manifest.json: total_package_size updated")
    print("Existing path strings were preserved; new entries use their on-disk spelling.")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Validate the GentleStop package layout (read-only by default), or "
                    "deliberately regenerate layout.json and manifest.json.",
        epilog="Examples:\n"
               "  python tools/update_layout.py\n"
               "  python tools/update_layout.py gentlestop-stage-panel --validate\n"
               "  python tools/update_layout.py --write --confirm-write D:\\scratch\\package-copy\n",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("package", nargs="?",
                        help="package directory (default: this repository's gentlestop-stage-panel)")
    parser.add_argument("--validate", action="store_true",
                        help="read-only validation (the default)")
    parser.add_argument("--write", action="store_true",
                        help="regenerate layout.json and manifest.json total_package_size")
    parser.add_argument("--confirm-write", action="store_true",
                        help="required together with --write")
    parser.add_argument("--allow-canonical-write", action="store_true",
                        help="required before write mode may touch this repository's canonical package")
    args = parser.parse_args(argv)

    if args.write and args.validate:
        parser.error("--write and --validate are mutually exclusive")
    if args.write and not args.confirm_write:
        parser.error("--write requires --confirm-write; nothing was changed")
    if args.allow_canonical_write and not args.write:
        parser.error("--allow-canonical-write is only meaningful with --write")

    pkg = resolve_package(args.package)
    result = validate(pkg)
    print_report(result)

    if result["fatal"]:
        return 2

    if args.write:
        return generate(result, args.allow_canonical_write)

    if result["errors"]:
        print("Result       : INVALID (%d error(s)); no files were written" % len(result["errors"]))
        return 1
    print("Result       : VALID (read-only; no files were written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
