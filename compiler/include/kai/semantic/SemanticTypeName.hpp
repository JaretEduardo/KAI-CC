#pragma once

#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Type.hpp"

#include <string>

namespace kai::semantic {

/// SEMANTIC INSPECTION MILESTONE 1: the ONE canonical semantic Type ->
/// tooling-facing string renderer. Every primitive TypeKind maps to the
/// exact spelling KAI source itself already uses for that type
/// (`i8`..`i64`, `u8`..`u64`, `f32`/`f64`, `bool`, `char`), plus
/// `"unresolved"`/`"error"`/`"unit"` for the three non-source-spellable
/// internal states Type also models (see Type.hpp's own Unresolved-vs-
/// Error documentation). KAI LANGUAGE M7A adds Array, rendered as
/// `"[ElementName; N]"` (e.g. `"[i32; 3]"`) - see Type.hpp's own
/// Array/CompoundTypeId documentation for why that requires `model`
/// (an Array Type's element type and length are NOT stored inline in
/// Type itself; they live in the issuing SemanticModel's own compound-
/// type interner, reached here via arrayElementType()/arrayLength(),
/// never via a raw CompoundTypeId).
///
/// `model` MUST be the SAME SemanticModel that produced/resolved `type`
/// - the same lifetime contract CompoundTypeId's own class comment
/// documents. Passing a Type from a different SemanticModel is
/// undefined by that contract, exactly like resolving a SymbolId against
/// the wrong model would be. Every primitive TypeKind ignores `model`
/// entirely (it carries no compound payload), so this is always safe to
/// call for a primitive Type regardless of which model produced it.
///
/// This is deliberately reusable beyond SemanticInspector - it is meant
/// to become the SAME renderer any future diagnostics/LSP/refs work uses
/// for showing a Type to a human or a tool, so a Type's tooling spelling
/// never drifts by being redefined in more than one place. Never invents
/// spellings for types this milestone's semantic::Type does not yet
/// model (references, structs, `str`/String, generics, ...) - see
/// Type.hpp itself for the complete, current, closed TypeKind vocabulary.
///
/// Returns an OWNED std::string, not std::string_view: unlike every
/// primitive spelling (a reference to a static string literal, cheap to
/// return by view), an array's rendering is built dynamically (its
/// element name plus its length), so there is no static storage a view
/// could safely point into.
std::string typeName(Type type, const SemanticModel& model);

} // namespace kai::semantic
