// velocity_curve.cpp — velocity → gain curve (stub for now).
//
// Spec: spec_env.md; Ghidra: FUN_180006cb0.
// SUBAGENT B: implement the velocity curve mapping.

// velocity_curve.cpp — amp velocity → gain factor.
//
// Spec: spec_env.md §4.2; Ghidra: FUN_180037bc0, FUN_180037f60 (param 30).
// The curve is implemented inline in velocity_curve.h (header-only); this
// translation unit exists so the build has a .cpp for the velocity stage.

#include "velocity_curve.h"

namespace synth1::env {

// (curve is inline in the header — spec_env.md §4.2)

}  // namespace synth1::env
