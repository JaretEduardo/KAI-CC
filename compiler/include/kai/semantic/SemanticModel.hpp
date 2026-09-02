#pragma once

#include "kai/ast/Expr.hpp"
#include "kai/ast/Node.hpp"
#include "kai/semantic/Symbol.hpp"
#include "kai/source/SourceLocation.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kai::semantic {

/// This vocabulary covers every kind of semantic failure this milestone
/// represents. Extend it as later phases add further kinds - none yet.
enum class SemanticErrorKind : std::uint8_t {
    DuplicateSymbol,
    UnknownIdentifier,
    UnknownType,
    TypeMismatch,
    LiteralOutOfRange,
    InvalidUnaryOperand,
    InvalidBinaryOperands,
    InvalidArgumentCount,
    NotCallable,
    InvalidAssignmentTarget,
    AssignmentToImmutableBinding,
    MissingReturn,

    /// Spellable str + parameters/returns MVP (M9): a `str`-returning
    /// function has more than one `str`-typed parameter, AND the returned
    /// expression is not itself a string literal. This is a temporary,
    /// conservative implementation-boundary restriction - NOT a general
    /// provenance/borrow-checking result - kept only until KAI has an
    /// owned `String`/borrowed-view mechanism and a real return-provenance
    /// analysis can replace it (see TypeChecker.cpp's checkReturnStmt()).
    UnsupportedStrReturn,

    /// KAI LANGUAGE M6 (`for` + integer ranges): a `for` statement's
    /// iterable is not a literal `start..end` range expression (the
    /// only iterable form M6 supports - see TypeChecker.cpp's
    /// checkForStmt()). KAI 0.1 has no general iterable protocol/arrays/
    /// iterators yet, so any other iterable shape (a bare identifier, a
    /// literal, a call, ...) is rejected here explicitly rather than
    /// silently left Unresolved and deferred to a confusing backend
    /// failure later.
    UnsupportedForIterable,

    /// KAI LANGUAGE M7A (fixed-size arrays): an array literal `[]` with
    /// no elements AND no usable contextual array Type (e.g. an
    /// explicit `let xs: [T; N] = ...` annotation) to determine its
    /// element type from. `[]` has no standalone inferred element type -
    /// approved language rule, not an implementation gap - see
    /// TypeChecker.cpp's checkArrayLiteralExpr().
    AmbiguousEmptyArrayLiteral,

    /// KAI LANGUAGE M7A (fixed-size arrays): an array literal whose
    /// elements do not all share one concrete type, e.g. `[1, true, 3]`.
    /// Reuses TypeMismatch's own expectedType/actualType fields
    /// (expectedType: the type every earlier element already
    /// established; actualType: this element's own type) rather than
    /// inventing a new error shape - see TypeChecker.cpp's
    /// checkArrayLiteralExpr().
    IncompatibleArrayElementType,

    /// KAI LANGUAGE M7B: `xs[index]` where `xs`'s own Type is not a
    /// fixed-size array (e.g. `5[0]`, or indexing a `str`/scalar local -
    /// array indexing and string indexing remain separate, unrelated
    /// features). `actualType` records what `xs` actually is.
    InvalidIndexTarget,

    /// KAI LANGUAGE M7B: `xs[index]` where `index`'s own Type is not one
    /// of the eight concrete integer types (float/bool/char/str/unit/
    /// array all rejected). `actualType` records what `index` actually
    /// is.
    InvalidIndexType,

    /// KAI LANGUAGE M7B: `xs[index]` where `index` is a compile-time
    /// constant (a bare or directly-negated integer literal) PROVABLY
    /// outside `xs`'s fixed length - e.g. `xs[3]`/`xs[-1]` for a
    /// length-3 array. A DYNAMIC out-of-bounds index is intentionally
    /// NOT a SemanticError at all (TYPE_SYSTEM.md §18's approved
    /// design): it is checked at runtime and traps via `llvm.trap`, a
    /// backend/runtime mechanism, never a compile-time diagnostic.
    ArrayIndexOutOfBounds,
};

/// A minimal, message-free description of a semantic failure - the
/// semantic-analysis counterpart to parser::ParseError, but deliberately
/// not a reuse of it: ParseError's `actual`/`expected` TokenKind fields
/// describe a syntactic-failure shape with no meaning here (there is no
/// "expected token" for a duplicate symbol). No message string, no
/// diagnostic code, no severity, no diagnostics-renderer integration yet
/// - this is a temporary stand-in for a future Diagnostic, the same role
/// ParseError plays for the parser.
struct SemanticError {
    SemanticErrorKind kind;
    SourceSpan primarySpan;

    /// Set only when a second location is meaningful, e.g.
    /// DuplicateSymbol's original declaration site, or TypeMismatch's/
    /// LiteralOutOfRange's explicit annotation span.
    std::optional<SourceSpan> relatedSpan;

    /// Only meaningful for TypeMismatch (concrete expected type) and
    /// LiteralOutOfRange (target integer type); nullopt otherwise. The
    /// default member initializer (rather than relying on aggregate-init
    /// value-initialization alone) is what lets every existing 3-field
    /// SemanticError{...} call site in SemanticAnalyzer.cpp keep
    /// compiling with no -Wmissing-field-initializers warning.
    std::optional<Type> expectedType = std::nullopt;

    /// Only meaningful for TypeMismatch (concrete actual type); nullopt
    /// otherwise, including for LiteralOutOfRange.
    std::optional<Type> actualType = std::nullopt;
};

/// Forward-declared only so SemanticModel can grant it access to its
/// private mutators (see below) - not implemented in this phase.
class SemanticAnalyzer;

/// Forward-declared only so SemanticModel can grant it access to its
/// private expression/symbol type mutators (see below). TypeChecker runs
/// as a separate pass after SemanticAnalyzer, over the same SemanticModel.
class TypeChecker;

/// Forward-declared only so SemanticModel can grant it access to
/// addError() (see below). ControlFlowAnalyzer runs as a third, separate
/// pass after TypeChecker, over the same SemanticModel - it needs no
/// other mutator: it never records expression/symbol types.
class ControlFlowAnalyzer;

/// The result of semantic analysis on one SourceFile: every declared
/// Symbol, every identifier-use resolution, every declaration-identifier
/// association, and every SemanticError collected along the way.
///
/// SemanticModel never mutates the AST: all semantic information is
/// attached here, externally, keyed by the address of the AST node it
/// describes, rather than by extending ast::Expr/ast::Decl with semantic
/// fields.
///
/// AST identity: SemanticModel keys its maps on raw pointer identity -
/// `const ast::IdentifierExpr*` for identifier uses, `const
/// ast::Identifier*` for declaration names - never SourceSpan. Two
/// different AST nodes can legitimately share an identical SourceSpan in
/// this AST today (e.g. Parser.cpp's expression-statement path always
/// constructs an ExprStmt's span as exactly its wrapped Expr's own
/// span), which makes SourceSpan unsound as a lookup key. No NodeId is
/// introduced either: AST nodes in this compiler are heap-allocated via
/// unique_ptr and never relocated after construction, so their addresses
/// are already stable for the AST's entire lifetime - exactly the
/// property a map key needs, with no new AST infrastructure. The
/// declaration-Identifier map is deliberately keyed on `ast::Identifier`
/// (a plain, span-only, non-polymorphic struct embedded by value inside
/// FunctionDecl/Param/VarDeclStmt/ForStmt) rather than on each of those
/// containing node types individually, so one map handles every future
/// declaration category uniformly.
///
/// Lifetime contract: the ast::SourceFile (and everything it owns) used
/// to build this SemanticModel must remain alive for as long as this
/// SemanticModel is queried. SemanticModel stores addresses into that
/// tree; it never extends its lifetime.
class SemanticModel {
public:
    SemanticModel() = default;

    /// The SymbolId a given identifier *use* resolved to, or
    /// std::nullopt if it never resolved (e.g. an UnknownIdentifier
    /// error was recorded for it instead).
    std::optional<SymbolId> resolution(const ast::IdentifierExpr& expr) const {
        const auto it = identifierResolutions_.find(&expr);
        if (it == identifierResolutions_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// The SymbolId a given *declaration* name resolves to. Covers every
    /// declaration-site ast::Identifier uniformly - FunctionDecl::name(),
    /// Param::name, VarDeclStmt::name(), ForStmt::variable() - without a
    /// separate map per declaration category.
    std::optional<SymbolId> declarationSymbol(const ast::Identifier& identifier) const {
        const auto it = declarationSymbols_.find(&identifier);
        if (it == declarationSymbols_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    const Symbol& symbol(SymbolId id) const {
        assert(id.isValid());
        return symbols_[id.rawId()];
    }

    const std::vector<SemanticError>& errors() const noexcept { return errors_; }

    /// The semantic Type recorded for `expr` by TypeChecker, or
    /// std::nullopt if `expr` has not been type-checked at all. Distinct
    /// from a recorded Type::unresolved()/Type::error() - see
    /// TypeChecker.hpp's class comment for the three-state contract this
    /// deliberately preserves.
    std::optional<Type> typeOf(const ast::Expr& expr) const {
        const auto it = expressionTypes_.find(&expr);
        if (it == expressionTypes_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// KAI LANGUAGE M7A: `type`'s element Type - `type` MUST be an array
    /// Type this SAME SemanticModel issued (via internArray(), directly
    /// or through a fixed-size-array TypeSyntax resolution) - see
    /// CompoundTypeId's own class comment for why a Type from a
    /// DIFFERENT SemanticModel is never a valid argument here, even one
    /// built from identical source text. Callers (SemanticInspector,
    /// SemanticTypeName, a future LLVMCodeGenerator array lowering, ...)
    /// use this - and arrayLength() below - instead of ever inspecting
    /// CompoundTypeId/the interning table directly, exactly the "clean
    /// API, no internal ID leakage" contract Type::isArray()'s own
    /// comment describes.
    Type arrayElementType(Type type) const {
        assert(type.isArray());
        return arrayTypes_[type.compoundId().rawId()].elementType;
    }

    /// KAI LANGUAGE M7A: `type`'s compile-time element count. Same
    /// "must be an array Type issued by THIS model" precondition as
    /// arrayElementType() above.
    std::uint64_t arrayLength(Type type) const {
        assert(type.isArray());
        return arrayTypes_[type.compoundId().rawId()].length;
    }

    /// KAI LANGUAGE M10A: `type`'s element Type - `type` MUST be a slice
    /// Type this SAME SemanticModel issued (via internSlice(), directly
    /// or through a slice TypeSyntax resolution) - same "must be issued
    /// by THIS model" lifetime contract as arrayElementType() above.
    /// There is no sliceLength() counterpart: a slice's length is runtime
    /// data, never part of the type itself (TYPE_SYSTEM.md's own
    /// "Slices" section).
    Type sliceElementType(Type type) const {
        assert(type.isSlice());
        return sliceTypes_[type.compoundId().rawId()].elementType;
    }

private:
    // Only a future SemanticAnalyzer populates a SemanticModel. Nothing
    // else - not even tests - should be able to construct an arbitrary
    // (possibly invalid) SemanticModel state through the public API;
    // friendship keeps that mutation surface to exactly the one
    // component responsible for producing correct semantic facts.
    friend class SemanticAnalyzer;

    /// TypeChecker is the only component allowed to record expression
    /// types or overwrite a symbol's inferred type, mirroring the same
    /// friendship-only-mutation contract SemanticAnalyzer already has
    /// above - never exposed as public mutable semantic state.
    friend class TypeChecker;

    /// ControlFlowAnalyzer only ever needs addError() - it never records
    /// expression/symbol types, adds symbols, or resolves names.
    friend class ControlFlowAnalyzer;

    SymbolId addSymbol(Symbol symbol) {
        symbols_.push_back(std::move(symbol));
        return SymbolId(static_cast<std::uint32_t>(symbols_.size() - 1));
    }

    void recordResolution(const ast::IdentifierExpr& expr, SymbolId id) {
        identifierResolutions_.emplace(&expr, id);
    }

    void recordDeclaration(const ast::Identifier& identifier, SymbolId id) {
        declarationSymbols_.emplace(&identifier, id);
    }

    void addError(SemanticError error) { errors_.push_back(std::move(error)); }

    void setExpressionType(const ast::Expr& expr, Type type) { expressionTypes_.insert_or_assign(&expr, type); }

    void setSymbolType(SymbolId id, Type type) {
        assert(id.isValid());
        symbols_[id.rawId()].type = type;
    }

    /// KAI LANGUAGE M7A: the one compile-scoped compound-type interner
    /// this model owns (see Type.hpp's CompoundTypeId/Type class
    /// comments for the full design rationale). `elementType` must
    /// itself already be a Type this SAME model produced/resolved -
    /// SemanticAnalyzer's resolveTypeSyntax() and TypeChecker's
    /// checkArrayLiteralExpr() are the only two callers (both already
    /// friends of SemanticModel, mirroring every other mutator here).
    ///
    /// Canonicalization: a linear scan for a structurally-equal existing
    /// entry (same elementType, same length) - deliberately not a hash
    /// map keyed on (Type, uint64_t), since ONE compilation's total
    /// distinct array-type-shape count is expected to stay small (this
    /// is source-declared shapes, not runtime values), and a linear scan
    /// keeps this interner trivially correct with no custom hash/equal
    /// functor to maintain. Reusing an existing entry - rather than
    /// always appending - is what makes `[i32, i32, i32]`'s array
    /// literal Type and a separately-written `[i32; 3]` annotation
    /// compare `==` to each other: both resolve to the SAME
    /// CompoundTypeId.
    Type internArray(Type elementType, std::uint64_t length) {
        for (std::size_t i = 0; i < arrayTypes_.size(); ++i) {
            if (arrayTypes_[i].elementType == elementType && arrayTypes_[i].length == length) {
                return Type(TypeKind::Array, CompoundTypeId(static_cast<std::uint32_t>(i)));
            }
        }
        arrayTypes_.push_back(ArrayTypeInfo{elementType, length});
        return Type(TypeKind::Array, CompoundTypeId(static_cast<std::uint32_t>(arrayTypes_.size() - 1)));
    }

    /// KAI LANGUAGE M10A: the slice-type counterpart to internArray()
    /// above - same canonicalizing-linear-scan design, same "elementType
    /// must already be a Type this SAME model produced/resolved"
    /// precondition, same friend-only-caller contract
    /// (SemanticAnalyzer::resolveSliceTypeSyntax() is the only caller).
    /// A SEPARATE table from arrayTypes_ (see this class's own §10 design
    /// note in SemanticModel.hpp's implementation for the reasoning) -
    /// deliberately NOT unified into one tagged compound-type table:
    /// Array and Slice have different structural shapes (length vs. no
    /// length), so a shared table would need a variant/tagged payload for
    /// exactly two current cases, adding indirection with no present
    /// benefit. Two small sibling tables, one per compound kind, is the
    /// least-invasive extension of M7A's existing design, and generalizes
    /// cleanly to a THIRD compound kind later (another sibling table) far
    /// more simply than retrofitting a tagged union would.
    Type internSlice(Type elementType) {
        for (std::size_t i = 0; i < sliceTypes_.size(); ++i) {
            if (sliceTypes_[i].elementType == elementType) {
                return Type(TypeKind::Slice, CompoundTypeId(static_cast<std::uint32_t>(i)));
            }
        }
        sliceTypes_.push_back(SliceTypeInfo{elementType});
        return Type(TypeKind::Slice, CompoundTypeId(static_cast<std::uint32_t>(sliceTypes_.size() - 1)));
    }

    /// KAI LANGUAGE M7A: the compound-type interning table's own storage
    /// shape - never exposed outside SemanticModel (see
    /// arrayElementType()/arrayLength() above for the public, ID-free
    /// read API every other consumer uses instead).
    struct ArrayTypeInfo {
        Type elementType;
        std::uint64_t length;
    };

    /// KAI LANGUAGE M10A: Slice's own interning-table storage shape - see
    /// ArrayTypeInfo's own comment above for the identical rationale.
    /// Deliberately no `length` field: a slice's length is runtime data,
    /// never part of the type's own structural identity.
    struct SliceTypeInfo {
        Type elementType;
    };

    std::vector<Symbol> symbols_;
    std::unordered_map<const ast::IdentifierExpr*, SymbolId> identifierResolutions_;
    std::unordered_map<const ast::Identifier*, SymbolId> declarationSymbols_;
    std::vector<SemanticError> errors_;
    std::unordered_map<const ast::Expr*, Type> expressionTypes_;
    std::vector<ArrayTypeInfo> arrayTypes_;
    std::vector<SliceTypeInfo> sliceTypes_;
};

} // namespace kai::semantic
