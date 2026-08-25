#pragma once

#include "kai/semantic/Type.hpp"

#include <string_view>

namespace kai::semantic {

/// SEMANTIC INSPECTION MILESTONE 1: the ONE canonical semantic Type ->
/// tooling-facing string renderer. Every current TypeKind maps to the
/// exact spelling KAI source itself already uses for that type
/// (`i8`..`i64`, `u8`..`u64`, `f32`/`f64`, `bool`, `char`), plus
/// `"unresolved"`/`"error"`/`"unit"` for the three non-source-spellable
/// internal states Type also models (see Type.hpp's own Unresolved-vs-
/// Error documentation).
///
/// This is deliberately reusable beyond SemanticInspector - it is meant
/// to become the SAME renderer any future diagnostics/LSP/refs work uses
/// for showing a Type to a human or a tool, so a Type's tooling spelling
/// never drifts by being redefined in more than one place. Never invents
/// spellings for types this milestone's semantic::Type does not yet
/// model (arrays, references, structs, `str`/String, ...) - see Type.hpp
/// itself for the complete, current, closed TypeKind vocabulary.
std::string_view typeName(Type type) noexcept;

} // namespace kai::semantic
