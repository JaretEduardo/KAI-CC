#include "kai/semantic/SemanticTypeName.hpp"

namespace kai::semantic {

// No `default:` case: TypeKind is fully implemented today, mirroring
// TypeChecker.cpp's/LLVMCodeGenerator.cpp's own exhaustive switches over
// it (see e.g. TypeChecker.cpp's integerRangeFor(), LLVMCodeGenerator.cpp's
// lowerType()).
std::string_view typeName(Type type) noexcept {
    switch (type.kind()) {
        case TypeKind::Unresolved:
            return "unresolved";
        case TypeKind::Error:
            return "error";
        case TypeKind::Unit:
            return "unit";
        case TypeKind::I8:
            return "i8";
        case TypeKind::I16:
            return "i16";
        case TypeKind::I32:
            return "i32";
        case TypeKind::I64:
            return "i64";
        case TypeKind::U8:
            return "u8";
        case TypeKind::U16:
            return "u16";
        case TypeKind::U32:
            return "u32";
        case TypeKind::U64:
            return "u64";
        case TypeKind::F32:
            return "f32";
        case TypeKind::F64:
            return "f64";
        case TypeKind::Bool:
            return "bool";
        case TypeKind::Char:
            return "char";
        case TypeKind::Str:
            // Minimal String Literal Support milestone: renders as the
            // internal Str type's canonical name, not a claim that a
            // spellable `str` annotation exists yet (see Type::str()'s
            // own comment).
            return "str";
    }
    // Unreachable while TypeKind's enumerators match the switch above
    // exactly - kept only so -Wreturn-type doesn't warn.
    return "unresolved";
}

} // namespace kai::semantic
