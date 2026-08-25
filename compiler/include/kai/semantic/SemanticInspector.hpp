#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/Stmt.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/semantic/Type.hpp"
#include "kai/source/SourceManager.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kai::semantic {

/// SEMANTIC INSPECTION MILESTONE 1: a 1-indexed source position, reusing
/// SourceManager::LineColumn's own documented convention verbatim (see
/// SourceManager.hpp) rather than inventing a second one - line and
/// column both start at 1; column is a 1-indexed BYTE offset within the
/// line, not a Unicode codepoint count.
struct InspectionPosition {
    std::uint32_t line;
    std::uint32_t column;
};

/// A source range as exposed to tooling. `end` is the position
/// immediately AFTER the last character - half-open, mirroring
/// SourceSpan's own `[begin, end)` convention exactly (never converted to
/// an inclusive last-character position).
struct InspectionRange {
    InspectionPosition start;
    InspectionPosition end;
};

/// The ONE canonical SourceSpan -> InspectionRange conversion (M2 spec
/// §7: reuse M1's "source line/column conversion" primitive rather than
/// re-deriving it) - decodes both endpoints via
/// `SourceManager::lineColumn()`. Shared by SemanticInspector and
/// SemanticQuery so a span's tooling-facing position can never drift
/// between the two.
InspectionRange inspectionRangeOf(const SourceManager& sources, SourceSpan span);

/// One function parameter's tooling summary, nested under its owning
/// function's SemanticSymbolInfo::parameters (see SemanticSymbolInfo
/// below for why a parameter ALSO gets its own top-level, flat
/// SemanticSymbolInfo entry - this nested copy is intentionally minimal:
/// just enough to discover a signature's shape without a second lookup).
struct SemanticParameterInfo {
    std::string name;
    Type type;
    InspectionRange definition;
};

/// The symbol-kind vocabulary this EXTERNAL tooling contract exposes -
/// deliberately its own enum, not a reuse of semantic::SymbolKind: this
/// milestone intentionally excludes SymbolKind::Builtin (prelude names
/// like `print` are not declared BY the inspected file - see
/// SemanticInspector.cpp's own filtering note), and keeping a separate
/// enum here means a future addition to the compiler-internal SymbolKind
/// vocabulary can never silently leak into this versioned JSON schema
/// without a deliberate decision in this file first.
enum class SemanticSymbolKind : std::uint8_t {
    Function,
    Parameter,
    Local,
};

/// One user-authored symbol, as exposed to tooling.
///
/// Public identity is (name, kind, definition range) - NEVER
/// semantic::SymbolId, which is a compiler-internal storage index with
/// no cross-run or cross-implementation stability guarantee (SymbolId's
/// own class comment: "Callers should treat them as opaque tokens").
/// SemanticInspector never serializes a SymbolId anywhere.
///
/// Not every field is meaningful for every `kind` - see writeJson() in
/// SemanticInspectionJson.cpp for the exact per-kind JSON shape this
/// produces (Function: parameters+returnType; Parameter/Local: type+
/// enclosingFunction). A tagged std::variant was considered and rejected
/// for M1: with only two real shapes and every field already a small
/// value type, the extra ceremony would not pay for itself yet.
struct SemanticSymbolInfo {
    std::string name;
    SemanticSymbolKind kind;
    InspectionRange definition;

    /// Meaningful for Parameter/Local only. Symbol.hpp itself documents
    /// a Function's own `.type` field as "unused" (a function has no
    /// single semantic::Type in this compiler's vocabulary - its type
    /// information is the signature below), so this is left at its
    /// Type::unresolved() default for Function and never serialized for
    /// that kind.
    Type type = Type::unresolved();

    /// Meaningful for Function only - a per-parameter summary in
    /// declaration order (see SemanticParameterInfo). Empty for
    /// Parameter/Local.
    std::vector<SemanticParameterInfo> parameters;

    /// Meaningful for Function only.
    Type returnType = Type::unresolved();

    /// The enclosing function's NAME (never a SymbolId - see this
    /// struct's own header comment), for Parameter/Local symbols only.
    /// KAI 0.1 has no function overloading and no nested functions, so
    /// one file can never have two functions sharing a name - a plain
    /// name is therefore a stable enough context for M1 (see this
    /// milestone's own §13). std::nullopt for Function (top-level, no
    /// enclosing function to name).
    std::optional<std::string> enclosingFunction;
};

/// The complete result of inspecting one already-fully-checked
/// SourceFile.
struct SemanticInspectionResult {
    /// The file's own source identity, exactly as SourceManager knows it
    /// (`SourceManager::fileName()`) - a path for a file loaded from
    /// disk, or whatever display name a virtual file was registered
    /// under. Never canonicalized/resolved to an absolute path beyond
    /// whatever SourceManager itself already stores.
    std::string file;

    /// Every user-authored symbol this milestone models, in SOURCE
    /// DECLARATION ORDER (functions in file order; within a function,
    /// its own entry, then its parameters, then its body's locals in
    /// traversal order) - never alphabetically sorted, and never
    /// ordered by an unordered_map's own iteration order (see
    /// SemanticInspector.cpp's own determinism note).
    std::vector<SemanticSymbolInfo> symbols;
};

/// Builds a SemanticInspectionResult by walking `file`'s AST in source
/// order - the AST is the STRUCTURAL traversal source (which
/// declarations exist, and in what order); `model` is the SEMANTIC TRUTH
/// for each declaration's kind/type/signature (via
/// `model.declarationSymbol()` + `model.symbol()` - SemanticModel itself
/// exposes no direct "iterate every symbol" API, see SemanticModel.hpp).
/// This performs NO name resolution of its own - every fact it reports
/// was already computed by SemanticAnalyzer/TypeChecker.
///
/// Precondition: `model` already reflects a successful
/// `SemanticAnalyzer::analyze()` + `TypeChecker::check()` +
/// `ControlFlowAnalyzer::check()` run over `file`. This class performs
/// NO semantic validation itself and does not consult `model.errors()` -
/// that policy decision (whether to run inspection at all on an invalid
/// program) belongs to the caller (see kai::cli::runInspectCommand).
///
/// Never touches argv, stdout, stderr, JSON text, or LLVM/codegen - this
/// is a pure, reusable compiler-layer query. Callers (the CLI today; a
/// future AI-agent API, a future LSP, a future refactoring tool) render
/// or consume SemanticInspectionResult however they need.
class SemanticInspector {
public:
    SemanticInspector(const SourceManager& sources, const SemanticModel& model) noexcept
        : sources_(sources), model_(model) {}

    SemanticInspectionResult inspect(const ast::SourceFile& file) const;

private:
    void collectFunction(const ast::FunctionDecl& fn, SemanticInspectionResult& result) const;
    void collectBlock(const ast::BlockStmt& block, const std::string& enclosingFunction,
                       SemanticInspectionResult& result) const;
    void collectStatement(const ast::Stmt& stmt, const std::string& enclosingFunction,
                           SemanticInspectionResult& result) const;

    const SourceManager& sources_;
    const SemanticModel& model_;
};

} // namespace kai::semantic
