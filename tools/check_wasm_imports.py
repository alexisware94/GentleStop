#!/usr/bin/env python3
"""Verify a gentlestop.wasm can be loaded by MSFS: it may only import SimConnect/MSFS host functions
and wasi_snapshot_preview1.*. Anything else (printf, fopen, exp, ...) means the C library was not linked in
and the sim will refuse to load the module.   Usage: python check_wasm_imports.py gentlestop.wasm"""
import sys
d = open(sys.argv[1], "rb").read()
if d[:4] != b"\0asm": sys.exit("not a wasm file")
def leb(i):
    r = s = 0
    while True:
        b = d[i]; i += 1; r |= (b & 0x7f) << s; s += 7
        if not b & 0x80: return r, i
def name(i):
    n, i = leb(i); return d[i:i+n].decode(), i + n
i, secs = 8, {}
while i < len(d):
    sid = d[i]; i += 1; n, i = leb(i); secs[sid] = (i, n); i += n
ALLOWED_ENV = ("SimConnect_", "register_named_variable", "get_named_variable_value", "set_named_variable_value",
               "fsEventsRegisterKeyEventHandler", "fsEventsUnregisterKeyEventHandler",
               "fsCommBusCall", "fsCommBusRegister", "fsCommBusUnregister")
bad, ok = [], []
if 2 in secs:
    i, _ = secs[2]; cnt, i = leb(i)
    for _ in range(cnt):
        m, i = name(i); f, i = name(i); k = d[i]; i += 1
        if k == 0: _, i = leb(i)
        elif k == 1: i += 1; fl = d[i]; i += 1; _, i = leb(i); (fl & 1) and leb(i)
        elif k == 2:
            fl = d[i]; i += 1; _, i = leb(i)
            if fl & 1: _, i = leb(i)
        elif k == 3: i += 2
        good = m == "wasi_snapshot_preview1" or (m == "env" and f.startswith(ALLOWED_ENV))
        (ok if good else bad).append(m + "." + f)
exports = []
if 7 in secs:
    i, _ = secs[7]; cnt, i = leb(i)
    for _ in range(cnt):
        f, i = name(i); k = d[i]; i += 1; _, i = leb(i); exports.append(f)
print("OK imports :", ", ".join(ok) or "(none)")
print("  (wasi imports are only valid if the sim implements them; if the console says an import was not found, compare with this list)")
print("exports    :", ", ".join(exports))
for need in ("module_init", "__indirect_function_table", "module_deinit"):
    if need not in exports: bad.append("MISSING EXPORT " + need)
if bad:
    print("\nPROBLEM - the sim will not be able to load this module:"); [print("  ", b) for b in bad]; sys.exit(1)
print("\nLooks loadable.")
