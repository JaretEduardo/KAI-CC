#pragma once

#include "kai/ast/SourceFile.hpp"
#include "kai/parser/ParseError.hpp"
#include "kai/source/SourceManager.hpp"

#include <filesystem>
#include <ostream>
#include <string>

namespace kai::cli {

/// Prints a deterministic, human-readable, ASCII-only tree of `file` to
/// `out`: one AST node per line, 2 spaces of indentation per depth level,
/// using each node's concrete class name and its most useful syntactic
/// fields (names via SourceManager::text(), literal lexemes via
/// escapeLexeme()). This is a debugging facility only: it performs no
/// semantic analysis and is not a serialization format.
void printAst(std::ostream& out, const SourceManager& sources, const ast::SourceFile& file);

/// Renders a ParseError as a single deterministic line, e.g.:
///   kaicc: parse error at 2:5: expected RightParen, got EndOfFile
///
/// This is temporary CLI-only formatting, not a Diagnostic: no colors,
/// snippets, notes, or diagnostic codes.
std::string formatParseError(const SourceManager& sources, const parser::ParseError& error);

/// Loads `path`, parses it with a real Parser, and either prints the
/// resulting AST (via printAst) to `out` or a parse-error line (via
/// formatParseError) to `err`.
///
/// Returns the process exit code for the `--ast` command:
///   0 - file loaded and parsed successfully (the AST was printed to out)
///   2 - the file failed to load (a message is written to err)
///   4 - file loaded, but parsing failed (a message is written to err)
int runAstCommand(SourceManager& sources, const std::filesystem::path& path, std::ostream& out, std::ostream& err);

} // namespace kai::cli
