#include "kai/semantic/SemanticTypeName.hpp"

namespace kai::semantic {

// No `default:` case: TypeKind is fully implemented today, mirroring
// TypeChecker.cpp's/LLVMCodeGenerator.cpp's own exhaustive switches over
// it (see e.g. TypeChecker.cpp's integerRangeFor(), LLVMCodeGenerator.cpp's
// lowerType()).
std::string typeName(Type type, const SemanticModel& model) {
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
        case TypeKind::Array: {
            // KAI LANGUAGE M7A: "[ElementName; N]" - the canonical
            // fixed-size-array spelling (TYPE_SYSTEM.md §18), built
            // recursively so a nested array (e.g. "[[i32; 2]; 3]")
            // renders correctly with no separate case needed. `model`
            // must be the SAME SemanticModel that produced `type` - see
            // this function's own header doc comment.
            std::string name = "[";
            name += typeName(model.arrayElementType(type), model);
            name += "; ";
            name += std::to_string(model.arrayLength(type));
            name += "]";
            return name;
        }
        case TypeKind::Slice: {
            // KAI LANGUAGE M10A: "[ElementName]" - no length component,
            // since a slice's length is runtime data, never part of the
            // type itself (TYPE_SYSTEM.md's own "Slices" section). Built
            // recursively exactly like Array above, so a nested slice
            // (e.g. "[[i32]]") or a slice of arrays (e.g. "[[i32; 3]]")
            // both render correctly with no separate case needed.
            std::string name = "[";
            name += typeName(model.sliceElementType(type), model);
            name += "]";
            return name;
        }
    }
    // Unreachable while TypeKind's enumerators match the switch above
    // exactly - kept only so -Wreturn-type doesn't warn.
    return "unresolved";
}

} // namespace kai::semantic
