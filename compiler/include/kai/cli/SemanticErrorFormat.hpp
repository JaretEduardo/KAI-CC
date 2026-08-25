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
std::string formatSemanticError(const SourceManager& sources, const semantic::SemanticError& error);

} // namespace kai::cli
