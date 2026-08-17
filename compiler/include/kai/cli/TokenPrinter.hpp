#pragma once

#include "kai/lexer/Token.hpp"
#include "kai/source/SourceManager.hpp"

#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>

namespace kai::cli {

/// Renders `text` as a single-line, ASCII-safe, byte-oriented debug
/// representation: '\n' '\r' '\t' '\\' '"' get their usual C-style two-
/// character escapes, printable ASCII (0x20-0x7E) passes through
/// unchanged, and every other byte (including 0x80-0xFF) becomes an
/// uppercase "\xHH" escape.
///
/// This is CLI debug formatting only, not KAI string-literal decoding: it
/// never validates or interprets UTF-8.
std::string escapeLexeme(std::string_view text);

/// Prints one `--tokens` debug line for `token` to `out`: its
/// tokenKindName(), its exact source lexeme (via
/// sources.text(token.span())) escaped and quoted, and its 1-indexed
/// line:column. EndOfFile has no lexeme (its span is zero-width) and
/// prints only its kind and location.
void printToken(std::ostream& out, const SourceManager& sources, Token token);

/// Loads `path` into `sources`, lexes it fully, and prints one line per
/// token (via printToken) to `out` until and including EndOfFile.
///
/// Returns the process exit code for the `--tokens` command:
///   0 - file loaded and lexed with no Invalid tokens
///   2 - the file failed to load (a message is written to `err`)
///   3 - file loaded and lexed, but at least one Invalid token occurred
int runTokensCommand(SourceManager& sources, const std::filesystem::path& path, std::ostream& out,
                      std::ostream& err);

} // namespace kai::cli
