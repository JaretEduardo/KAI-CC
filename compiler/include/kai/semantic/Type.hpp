#pragma once

#include <cstdint>
#include <limits>

namespace kai::semantic {

class SemanticModel;

/// The complete vocabulary of semantic type kinds this milestone knows
/// about. See Type's own class comment for the Unresolved/Error
/// distinction - they are never interchangeable.
enum class TypeKind : std::uint8_t {
    Unresolved,
    Error,

    Unit,

    I8,
    I16,
    I32,
    I64,

    U8,
    U16,
    U32,
    U64,

    F32,
    F64,

    Bool,
    Char,

    /// See Type::str()'s own comment: a temporary, deliberately
    /// underspecified internal type, not a declaration that KAI's final
    /// str/String/&str design (still open - see TYPE_SYSTEM.md,
    /// DESIGN_QUESTIONS.md) has been settled.
    Str,

    /// KAI LANGUAGE M7A: a fixed-size array `[T; N]` - a real structural
    /// type, not a fabricated stand-in. Unlike every other TypeKind
    /// above, an Array Type's full identity is NOT determined by `kind_`
    /// alone - it also needs an element Type and a compile-time length,
    /// neither of which fits in this flat enum. See Type's own class
    /// comment and CompoundTypeId below for how that structural payload
    /// is carried without turning Type itself into a heap object.
    Array,
};

/// KAI LANGUAGE M7A: an opaque handle into ONE SemanticModel's own
/// compound-type interning table (see SemanticModel::internArray()) -
/// the mechanism that lets a compound TypeKind (currently only Array)
/// carry structural data (element type, length) while Type itself stays
/// a small, flat, trivially-copyable value. Mirrors SymbolId's own
/// existing "opaque handle, only SemanticModel may construct or dereference
/// one" contract exactly (see Symbol.hpp) - a CompoundTypeId is never
/// constructed, incremented, or interpreted by anything other than the
/// SemanticModel that issued it, and (like SymbolId) survives that
/// model's internal storage reallocating, since it is an index, never a
/// raw pointer.
///
/// Lifetime invariant: a CompoundTypeId (and therefore any Type carrying
/// one) is only meaningful for as long as the SemanticModel that issued
/// it is alive, and must only ever be dereferenced against THAT SAME
/// model - never a different SemanticModel instance, even one built from
/// identical source text. This is the same lifetime contract
/// SemanticModel's own class comment already documents for identifier/
/// declaration resolution ("the ast::SourceFile used to build this
/// SemanticModel must remain alive...") extended to compound type data:
/// one SemanticModel, one coherent set of Type values, for the lifetime
/// of one compilation.
class CompoundTypeId {
public:
    constexpr CompoundTypeId() noexcept = default;

    constexpr bool isValid() const noexcept { return id_ != kInvalidId; }

    friend constexpr bool operator==(CompoundTypeId, CompoundTypeId) noexcept = default;

private:
    friend class SemanticModel;

    constexpr explicit CompoundTypeId(std::uint32_t id) noexcept : id_(id) {}

    constexpr std::uint32_t rawId() const noexcept { return id_; }

    static constexpr std::uint32_t kInvalidId = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t id_ = kInvalidId;
};

/// A semantic type: a small, cheap-to-copy value - NOT a heap object,
/// NOT reference-counted, NOT polymorphic. Every primitive TypeKind
/// (Unresolved through Str) is fully self-describing from `kind_` alone,
/// exactly as before M7A - constructing/copying/comparing one of those
/// remains as cheap as a plain enum, with `compoundId_` simply left at
/// its default, invalid value. Array is the one KIND whose full
/// identity additionally depends on structural payload (an element Type
/// + a length) that cannot fit in a flat enum tag - that payload is
/// never stored inline in Type itself (which would force EVERY Type,
/// including every plain `i32`, to carry unused space or an owning
/// pointer); instead Type stores only a `CompoundTypeId` handle into the
/// issuing SemanticModel's own interning table (see
/// SemanticModel::internArray()/arrayElementType()/arrayLength()). Type
/// therefore stays exactly what it always was - two small trivially-
/// copyable fields, no raw owning pointers, no global mutable state,
/// safe to store in a Symbol, an expression-type map, or a
/// FunctionSignature exactly like before.
///
/// Unresolved vs. Error - not interchangeable:
///
///   Unresolved: semantic information has not been computed yet, or this
///   semantic milestone intentionally does not model the syntactic type
///   shape yet (e.g. a ReferenceTypeSyntax before reference semantics
///   exist). Never accompanied by a SemanticError.
///
///   Error: semantic resolution was attempted and produced a real
///   semantic error (e.g. a NamedTypeSyntax naming an unknown type).
///   Always accompanied by a SemanticError recording why.
///
/// Unit, the primitive numeric/bool/char kinds, Str, and (as of KAI
/// LANGUAGE M7A) Array are modeled here. References, slices, generics,
/// and functions-as-values remain unrepresented - a syntactic type in
/// one of those shapes still resolves to Type::unresolved(), never a
/// fabricated Type of some new kind invented to stand in for it. Slices
/// (`[T]`) are a DISTINCT, still-future, non-owning view type - M7A
/// deliberately does not resolve them to Array or to anything else (see
/// SemanticAnalyzer.cpp's resolveTypeSyntax()).
///
/// Deliberately no single `primitive(TypeKind)` factory: that shape
/// would let a caller pass Error/Unresolved into it and read at the call
/// site as if it were requesting an ordinary primitive. Each kind gets
/// its own named factory instead, so constructing an Error or Unresolved
/// Type is always textually explicit. Array deliberately has NO such
/// factory at all - unlike every primitive kind, an array Type cannot be
/// constructed from a bare TypeKind: it can only be produced by
/// SemanticModel::internArray(), because canonicalization (so two
/// equivalent `[i32; 3]` types compare equal) requires consulting that
/// model's interning table. This is an intentional asymmetry, not an
/// oversight.
class Type {
public:
    static constexpr Type unresolved() noexcept { return Type(TypeKind::Unresolved); }
    static constexpr Type error() noexcept { return Type(TypeKind::Error); }

    static constexpr Type unit() noexcept { return Type(TypeKind::Unit); }

    static constexpr Type i8() noexcept { return Type(TypeKind::I8); }
    static constexpr Type i16() noexcept { return Type(TypeKind::I16); }
    static constexpr Type i32() noexcept { return Type(TypeKind::I32); }
    static constexpr Type i64() noexcept { return Type(TypeKind::I64); }

    static constexpr Type u8() noexcept { return Type(TypeKind::U8); }
    static constexpr Type u16() noexcept { return Type(TypeKind::U16); }
    static constexpr Type u32() noexcept { return Type(TypeKind::U32); }
    static constexpr Type u64() noexcept { return Type(TypeKind::U64); }

    static constexpr Type f32() noexcept { return Type(TypeKind::F32); }
    static constexpr Type f64() noexcept { return Type(TypeKind::F64); }

    /// Not named bool()/char(): both are reserved words in C++.
    static constexpr Type boolean() noexcept { return Type(TypeKind::Bool); }
    static constexpr Type character() noexcept { return Type(TypeKind::Char); }

    /// Minimal String Literal Support milestone ONLY: an internal type
    /// meaning "immutable UTF-8 byte sequence backed by static literal
    /// storage" (a `{ ptr, i64 }` pointer+length descriptor - see
    /// LLVMCodeGenerator's lowerType()). This is deliberately NOT a
    /// declaration that KAI's final string design is settled: TYPE_SYSTEM.md
    /// describes an owned `String` plus borrowed `&str`, SYNTAX.md uses a
    /// bare `str` annotation, and DESIGN_QUESTIONS.md still lists "what
    /// exactly is `str`?" as open. Str exists only so a string LITERAL
    /// expression (and a local inferred from one) has a concrete Type
    /// instead of Type::unresolved(); it is never reachable from a
    /// spellable source-level type annotation (SemanticAnalyzer's
    /// lookupPrimitiveTypeName() intentionally does not recognize `str`,
    /// so `let x: str = ...` still resolves to Type::error() /
    /// UnknownType, unchanged).
    static constexpr Type str() noexcept { return Type(TypeKind::Str); }

    constexpr TypeKind kind() const noexcept { return kind_; }
    constexpr bool isError() const noexcept { return kind_ == TypeKind::Error; }
    constexpr bool isUnresolved() const noexcept { return kind_ == TypeKind::Unresolved; }

    /// Pure structural facts about `kind()` - stable regardless of any
    /// future checker policy (conversions, operator applicability,
    /// common-type computation), which belong elsewhere, never here.
    constexpr bool isSignedInteger() const noexcept {
        return kind_ == TypeKind::I8 || kind_ == TypeKind::I16 || kind_ == TypeKind::I32 || kind_ == TypeKind::I64;
    }

    constexpr bool isUnsignedInteger() const noexcept {
        return kind_ == TypeKind::U8 || kind_ == TypeKind::U16 || kind_ == TypeKind::U32 || kind_ == TypeKind::U64;
    }

    constexpr bool isInteger() const noexcept { return isSignedInteger() || isUnsignedInteger(); }

    constexpr bool isFloat() const noexcept { return kind_ == TypeKind::F32 || kind_ == TypeKind::F64; }

    constexpr bool isNumeric() const noexcept { return isInteger() || isFloat(); }

    constexpr bool isBool() const noexcept { return kind_ == TypeKind::Bool; }

    constexpr bool isChar() const noexcept { return kind_ == TypeKind::Char; }

    constexpr bool isStr() const noexcept { return kind_ == TypeKind::Str; }

    /// KAI LANGUAGE M7A: true for a fixed-size array Type. Use
    /// SemanticModel::arrayElementType()/arrayLength() to inspect its
    /// structure - never CompoundTypeId/rawId() directly (private to
    /// Type/SemanticModel exactly like SymbolId's own rawId()).
    constexpr bool isArray() const noexcept { return kind_ == TypeKind::Array; }

    friend constexpr bool operator==(const Type&, const Type&) noexcept = default;

private:
    friend class SemanticModel;

    constexpr explicit Type(TypeKind kind) noexcept : kind_(kind) {}

    /// Only SemanticModel::internArray() ever calls this - see
    /// CompoundTypeId's own class comment for the lifetime contract this
    /// establishes.
    constexpr Type(TypeKind kind, CompoundTypeId compoundId) noexcept : kind_(kind), compoundId_(compoundId) {}

    constexpr CompoundTypeId compoundId() const noexcept { return compoundId_; }

    TypeKind kind_;

    /// Default-invalid (and therefore ignored by operator==='s member-
    /// wise comparison being trivially equal) for every primitive kind -
    /// only ever set to a real value by the (TypeKind, CompoundTypeId)
    /// constructor above, i.e. only for Array.
    CompoundTypeId compoundId_{};
};

} // namespace kai::semantic
