#pragma once

#include "kai/semantic/SemanticModel.hpp"
#include "kai/source/SourceManager.hpp"

#include <string>

namespace kai::cli {

/// Renders a SemanticError as a single deterministic line, e.g.:
///   kaicc: error at 2:5: unknown identifier
///
/// This is temporary CLI-only formatting, not a Diagnostic - the same
/// "not yet a real Diagnostic" spirit as this file's own sibling,
/// AstPrinter.hpp's formatParseError() (no message string exists on
/// SemanticError itself; see SemanticModel.hpp's own class comment - this
/// only renders the structured kind/location SemanticError does carry).
///
/// Shared by every CLI command that runs the semantic passes and needs
/// to report their errors (CompileCommand, InspectCommand, ...) so this
/// rendering is never duplicated across them.
///
/// `model` MUST be the SAME SemanticModel `error` was recorded against
/// (every call site already has it in scope, from the same frontend run
/// that produced `error` in the first place) - needed as of KAI LANGUAGE
/// M7A so a TypeMismatch/LiteralOutOfRange/IncompatibleArrayElementType
/// error whose expected/actual Type is a fixed-size array can render it
/// as "[i32; 3]" via semantic::typeName() rather than a bare, structure-
/// less type name.
std::string formatSemanticError(const SourceManager& sources, const semantic::SemanticError& error,
                                 const semantic::SemanticModel& model);

} // namespace kai::cli
