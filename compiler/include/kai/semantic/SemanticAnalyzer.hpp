#pragma once

#include "kai/ast/Decl.hpp"
#include "kai/ast/SourceFile.hpp"
#include "kai/ast/TypeSyntax.hpp"
#include "kai/semantic/SemanticModel.hpp"
#include "kai/source/SourceManager.hpp"

#include <string>
#include <unordered_map>

namespace kai::semantic {

/// Semantic Foundation Phase 2: collects top-level function declarations
/// and resolves their signatures.
///
/// This is only the first half of the eventual two-pass design (collect
/// top-level declarations, then analyze bodies): analyze() does not yet
/// traverse function bodies at all - no identifier resolution, no
/// Parameter/Local/Builtin symbols, no scopes, no prelude. That is a
/// later phase.
///
/// SemanticAnalyzer keeps no state between calls: every analyze() call
/// builds its SemanticModel and internal top-level name table entirely
/// from local state, so one SemanticAnalyzer instance may safely analyze
/// multiple, independent SourceFiles in sequence with no leakage between
/// calls.
class SemanticAnalyzer {
public:
    /// `sources` is stored non-owningly, matching Parser's own contract.
    /// It, and every ast::SourceFile later passed to analyze(), must
    /// outlive any SemanticModel this analyzer returns: SemanticModel
    /// stores AST pointer identity (see SemanticModel.hpp's own lifetime
    /// contract), and name resolution here reads source text through
    /// `sources`.
    explicit SemanticAnalyzer(const SourceManager& sources) noexcept;

    /// Phase 2 scope only: collects top-level ast::FunctionDecl symbols
    /// and resolves their FunctionSignature. Does not analyze bodies.
    SemanticModel analyze(const ast::SourceFile& file);

private:
    void collectTopLevelDeclaration(const ast::Decl& decl, SemanticModel& model,
                                     std::unordered_map<std::string, SymbolId>& topLevelNames) const;

    void collectFunctionDecl(const ast::FunctionDecl& fn, SemanticModel& model,
                              std::unordered_map<std::string, SymbolId>& topLevelNames) const;

    FunctionSignature resolveFunctionSignature(const ast::FunctionDecl& fn, SemanticModel& model) const;

    /// Exhaustive over ast::TypeSyntaxKind, no `default:`: Named
    /// resolves to a primitive Type or Type::error() (with an
    /// UnknownType SemanticError); Unit resolves to Type::unit();
    /// Reference/Slice/Array/Generic all resolve to Type::unresolved()
    /// with no SemanticError, since this phase does not model those
    /// semantic shapes yet (see Type.hpp's Unresolved-vs-Error
    /// distinction, and SemanticAnalyzer.cpp for the exact rationale).
    Type resolveTypeSyntax(const ast::TypeSyntax& type, SemanticModel& model) const;

    Type resolveNamedTypeSyntax(const ast::NamedTypeSyntax& type, SemanticModel& model) const;

    const SourceManager& sources_;
};

} // namespace kai::semantic
