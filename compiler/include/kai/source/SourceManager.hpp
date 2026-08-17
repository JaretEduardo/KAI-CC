#pragma once

#include "kai/source/SourceLocation.hpp"

#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace kai {

/// Owns every source buffer loaded during a compilation and provides the
/// only supported way to go from a SourceLocation/SourceSpan back to
/// human-readable information: file name, line, column, and source text.
///
/// SourceManager treats source files as opaque byte buffers. It performs no
/// UTF-8 validation and never modifies or normalizes the underlying buffer.
/// Column numbers are 1-indexed byte offsets within a line, not Unicode
/// codepoint counts; this is documented behavior for KAI 0.1 and may be
/// revisited if the lexer later needs codepoint-accurate columns.
///
/// SourceManager does not depend on Diagnostic and does not render
/// diagnostics. It only exposes the raw information a future diagnostic
/// renderer needs (lineColumn, lineText, text, file name, buffer).
class SourceManager {
public:
    SourceManager() = default;

    SourceManager(const SourceManager&) = delete;
    SourceManager& operator=(const SourceManager&) = delete;

    /// Loads a file from disk and registers it as a new source file.
    ///
    /// Returns a std::error_code (never a Diagnostic) on failure. Turning
    /// an infrastructure failure into a compiler diagnostic is the
    /// responsibility of whatever future component drives the compiler.
    std::expected<FileId, std::error_code> loadFile(const std::filesystem::path& path);

    /// Registers in-memory source text under a display name, without
    /// touching disk. Intended for unit tests and future REPL use.
    FileId addVirtualFile(std::string displayName, std::string contents);

    /// The name the file was loaded/registered under: a path for files
    /// loaded from disk, or whatever name was passed to addVirtualFile.
    std::string_view fileName(FileId file) const;

    /// The full, unmodified contents of the file's buffer.
    std::string_view buffer(FileId file) const;

    struct LineColumn {
        std::uint32_t line;   // 1-indexed
        std::uint32_t column; // 1-indexed byte offset within the line
    };

    /// Decodes a byte offset into a 1-indexed (line, column) pair.
    LineColumn lineColumn(SourceLocation loc) const;

    /// The text of one 1-indexed line, without its line terminator. A
    /// trailing '\r' from a CRLF line ending is excluded from the returned
    /// view; the underlying buffer itself is never modified.
    std::string_view lineText(FileId file, std::uint32_t line) const;

    /// The literal source text a span covers.
    std::string_view text(SourceSpan span) const;

private:
    struct Buffer {
        std::string name;
        std::string contents;
        // Byte offset of the first character of each line. Built lazily on
        // first use, since most files never need line/column decoding.
        mutable std::vector<std::uint32_t> lineStarts;
        mutable bool lineStartsBuilt = false;
    };

    FileId registerBuffer(std::string name, std::string contents);
    const Buffer& bufferFor(FileId file) const;
    static void ensureLineIndex(const Buffer& buffer);

    std::deque<Buffer> buffers_;
};

} // namespace kai
