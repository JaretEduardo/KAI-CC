#include "kai/source/SourceManager.hpp"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <ios>

namespace kai {

std::expected<FileId, std::error_code> SourceManager::loadFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(ec);
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    std::string contents(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        file.read(contents.data(), static_cast<std::streamsize>(size));
        if (!file) {
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }
    }

    return registerBuffer(path.string(), std::move(contents));
}

FileId SourceManager::addVirtualFile(std::string displayName, std::string contents) {
    return registerBuffer(std::move(displayName), std::move(contents));
}

FileId SourceManager::registerBuffer(std::string name, std::string contents) {
    const auto id = static_cast<std::uint32_t>(buffers_.size());
    buffers_.push_back(Buffer{std::move(name), std::move(contents), {}, false});
    return FileId(id); // SourceManager is a friend of FileId; id is 0-based.
}

const SourceManager::Buffer& SourceManager::bufferFor(FileId file) const {
    assert(file.isValid());
    assert(file.rawId() < buffers_.size());
    return buffers_[file.rawId()];
}

void SourceManager::ensureLineIndex(const Buffer& buffer) {
    if (buffer.lineStartsBuilt) {
        return;
    }

    buffer.lineStarts.clear();
    buffer.lineStarts.push_back(0);

    const std::string_view contents = buffer.contents;
    for (std::size_t i = 0; i < contents.size(); ++i) {
        if (contents[i] == '\n') {
            buffer.lineStarts.push_back(static_cast<std::uint32_t>(i + 1));
        }
    }

    buffer.lineStartsBuilt = true;
}

std::string_view SourceManager::fileName(FileId file) const { return bufferFor(file).name; }

std::string_view SourceManager::buffer(FileId file) const { return bufferFor(file).contents; }

SourceManager::LineColumn SourceManager::lineColumn(SourceLocation loc) const {
    assert(loc.isValid());

    const Buffer& buffer = bufferFor(loc.file());
    ensureLineIndex(buffer);

    const auto& lineStarts = buffer.lineStarts;
    const auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), loc.offset());
    const std::size_t lineIndex = static_cast<std::size_t>(std::distance(lineStarts.begin(), it)) - 1;

    return LineColumn{
        static_cast<std::uint32_t>(lineIndex) + 1,
        loc.offset() - lineStarts[lineIndex] + 1,
    };
}

std::string_view SourceManager::lineText(FileId file, std::uint32_t line) const {
    assert(line >= 1);

    const Buffer& buffer = bufferFor(file);
    ensureLineIndex(buffer);

    const std::size_t lineIndex = line - 1;
    assert(lineIndex < buffer.lineStarts.size());

    const std::uint32_t start = buffer.lineStarts[lineIndex];
    const std::uint32_t end = (lineIndex + 1 < buffer.lineStarts.size())
                                   ? buffer.lineStarts[lineIndex + 1] - 1 // exclude the '\n' itself
                                   : static_cast<std::uint32_t>(buffer.contents.size());

    std::string_view lineView = std::string_view(buffer.contents).substr(start, end - start);

    // Strip a trailing '\r' from a CRLF line ending without touching the buffer.
    if (!lineView.empty() && lineView.back() == '\r') {
        lineView.remove_suffix(1);
    }

    return lineView;
}

std::string_view SourceManager::text(SourceSpan span) const {
    assert(span.isValid());

    const Buffer& buffer = bufferFor(span.begin().file());
    const std::uint32_t start = span.begin().offset();
    const std::uint32_t end = span.end().offset();

    return std::string_view(buffer.contents).substr(start, end - start);
}

} // namespace kai
