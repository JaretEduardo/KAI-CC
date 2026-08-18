#pragma once

#include <cassert>
#include <cstdint>
#include <limits>

namespace kai {

class SourceManager;

/// Opaque handle to a source file registered with a SourceManager.
///
/// FileId values are only ever produced by SourceManager. Callers should
/// treat them as opaque tokens and never construct or index them manually.
class FileId {
public:
    constexpr FileId() noexcept = default;

    constexpr bool isValid() const noexcept { return id_ != kInvalidId; }

    friend constexpr bool operator==(FileId lhs, FileId rhs) noexcept = default;

private:
    friend class SourceManager;

    constexpr explicit FileId(std::uint32_t id) noexcept : id_(id) {}

    constexpr std::uint32_t rawId() const noexcept { return id_; }

    static constexpr std::uint32_t kInvalidId = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t id_ = kInvalidId;
};

/// A single position in a source file, expressed as a byte offset.
///
/// SourceLocation intentionally stores nothing but a file and a byte
/// offset. Line/column information is derived on demand by SourceManager,
/// since most locations created while lexing or parsing are never shown
/// to a user.
class SourceLocation {
public:
    constexpr SourceLocation() noexcept = default;

    constexpr SourceLocation(FileId file, std::uint32_t offset) noexcept
        : file_(file), offset_(offset) {}

    constexpr bool isValid() const noexcept {
        return file_.isValid() && offset_ != kInvalidOffset;
    }

    constexpr FileId file() const noexcept { return file_; }
    constexpr std::uint32_t offset() const noexcept { return offset_; }

    friend constexpr bool operator==(SourceLocation lhs, SourceLocation rhs) noexcept = default;

private:
    static constexpr std::uint32_t kInvalidOffset = std::numeric_limits<std::uint32_t>::max();

    FileId file_{};
    std::uint32_t offset_ = kInvalidOffset;
};

/// A half-open range [begin, end) of source text within a single file.
///
/// SourceSpan is the unit tokens, AST nodes, and diagnostics use to refer
/// to a piece of source text. Both endpoints must belong to the same file;
/// this is checked with an assertion, not a runtime error, since violating
/// it is always a compiler bug rather than a recoverable condition.
class SourceSpan {
public:
    constexpr SourceSpan() noexcept = default;

    constexpr SourceSpan(SourceLocation begin, SourceLocation end) noexcept
        : begin_(begin), end_(end) {
        assert(isWellFormed(begin, end));
    }

    /// A zero-width span at a single location.
    static constexpr SourceSpan point(SourceLocation loc) noexcept {
        return SourceSpan(loc, loc);
    }

    constexpr bool isValid() const noexcept { return isWellFormed(begin_, end_) && begin_.isValid(); }

    constexpr SourceLocation begin() const noexcept { return begin_; }
    constexpr SourceLocation end() const noexcept { return end_; }

    friend constexpr bool operator==(SourceSpan lhs, SourceSpan rhs) noexcept = default;

private:
    // Either both endpoints are invalid (an explicitly "empty" span), or
    // both are valid, share a file, and are correctly ordered.
    static constexpr bool isWellFormed(SourceLocation begin, SourceLocation end) noexcept {
        if (!begin.isValid() && !end.isValid()) {
            return true;
        }
        return begin.isValid() && end.isValid() && begin.file() == end.file() &&
               begin.offset() <= end.offset();
    }

    SourceLocation begin_{};
    SourceLocation end_{};
};

} // namespace kai
