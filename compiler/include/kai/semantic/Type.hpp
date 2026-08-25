#pragma once

#include <cstdint>

namespace kai::semantic {

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
};

/// A semantic type: a small, closed, value-typed kind tag. Never
/// interned, never heap-allocated, never polymorphic - Type is compared
/// and matched by value, not dynamically dispatched, the same way
/// ast::LiteralKind or ast::ReferenceMutability are plain enums rather
/// than class hierarchies.
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
/// Only Unit, the primitive numeric/bool/char kinds, and (as of the
/// Minimal String Literal Support milestone) Str are modeled here.
/// References, slices, arrays, generics, and functions-as-values remain
/// unrepresented - a syntactic type in one of those shapes still resolves
/// to Type::unresolved(), never a fabricated Type of some new kind
/// invented to stand in for it.
///
/// Deliberately no single `primitive(TypeKind)` factory: that shape
/// would let a caller pass Error/Unresolved into it and read at the call
/// site as if it were requesting an ordinary primitive. Each kind gets
/// its own named factory instead, so constructing an Error or Unresolved
/// Type is always textually explicit.
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

    friend constexpr bool operator==(const Type&, const Type&) noexcept = default;

private:
    constexpr explicit Type(TypeKind kind) noexcept : kind_(kind) {}

    TypeKind kind_;
};

} // namespace kai::semantic
