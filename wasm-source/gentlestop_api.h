// Declarations of the legacy named-variable (L:) API used by GentleStop.
// The MSFS 2024 SDK headers available here did not declare these (they are deprecated there), but the sim host
// still exports them from module "env". Enabled by -DGS_DECLARE_L_API (set in tools/build_wasm.ps1).
// If the compiler reports "conflicting declaration", drop that define.
#pragma once
#ifdef GS_DECLARE_L_API
#ifndef GS_L_API_DECLARED
#define GS_L_API_DECLARED
extern "C" {
int    register_named_variable(const char* name);
double get_named_variable_value(int id);
void   set_named_variable_value(int id, double value);
}
#endif
#endif
