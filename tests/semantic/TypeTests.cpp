#include "kai/semantic/Type.hpp"

#include "support/check.hpp"

using kai::semantic::Type;
using kai::semantic::TypeKind;

namespace {

// --- Unresolved / Error: never interchangeable ---

void testUnresolvedConstruction() {
    const Type type = Type::unresolved();

    KAI_CHECK(type.kind() == TypeKind::Unresolved);
    KAI_CHECK(type.isUnresolved());
    KAI_CHECK(!type.isError());
}

void testErrorConstruction() {
    const Type type = Type::error();

    KAI_CHECK(type.kind() == TypeKind::Error);
    KAI_CHECK(type.isError());
    KAI_CHECK(!type.isUnresolved());
}

void testUnresolvedAndErrorAreDistinct() {
    KAI_CHECK(Type::unresolved() != Type::error());
    KAI_CHECK(Type::unresolved().kind() != Type::error().kind());
}

// --- Unit ---

void testUnitConstruction() {
    const Type type = Type::unit();

    KAI_CHECK(type.kind() == TypeKind::Unit);
    KAI_CHECK(!type.isError());
    KAI_CHECK(!type.isUnresolved());
}

// --- Every primitive TypeKind is reachable through its own factory ---

void testSignedIntegerFactories() {
    KAI_CHECK(Type::i8().kind() == TypeKind::I8);
    KAI_CHECK(Type::i16().kind() == TypeKind::I16);
    KAI_CHECK(Type::i32().kind() == TypeKind::I32);
    KAI_CHECK(Type::i64().kind() == TypeKind::I64);
}

void testUnsignedIntegerFactories() {
    KAI_CHECK(Type::u8().kind() == TypeKind::U8);
    KAI_CHECK(Type::u16().kind() == TypeKind::U16);
    KAI_CHECK(Type::u32().kind() == TypeKind::U32);
    KAI_CHECK(Type::u64().kind() == TypeKind::U64);
}

void testFloatingPointFactories() {
    KAI_CHECK(Type::f32().kind() == TypeKind::F32);
    KAI_CHECK(Type::f64().kind() == TypeKind::F64);
}

void testBoolAndCharFactories() {
    KAI_CHECK(Type::boolean().kind() == TypeKind::Bool);
    KAI_CHECK(Type::character().kind() == TypeKind::Char);
}

// --- None of the primitive/unit factories ever report as Error/Unresolved ---

void testPrimitiveAndUnitTypesAreNeitherErrorNorUnresolved() {
    const Type types[] = {
        Type::unit(),  Type::i8(),  Type::i16(),    Type::i32(),  Type::i64(),
        Type::u8(),    Type::u16(), Type::u32(),    Type::u64(),  Type::f32(),
        Type::f64(),   Type::boolean(), Type::character(),
    };

    for (const Type& type : types) {
        KAI_CHECK(!type.isError());
        KAI_CHECK(!type.isUnresolved());
    }
}

// --- Equality is by value (kind), not identity ---

void testEqualityIsByKind() {
    KAI_CHECK(Type::i32() == Type::i32());
    KAI_CHECK(Type::i32() != Type::i64());
    KAI_CHECK(Type::unit() == Type::unit());
    KAI_CHECK(Type::boolean() != Type::character());
}

void testCopiesCompareEqual() {
    const Type original = Type::f64();
    const Type copy = original; // NOLINT - exercising value-copy semantics

    KAI_CHECK(copy == original);
    KAI_CHECK(copy.kind() == TypeKind::F64);
}

} // namespace

int main() {
    testUnresolvedConstruction();
    testErrorConstruction();
    testUnresolvedAndErrorAreDistinct();

    testUnitConstruction();

    testSignedIntegerFactories();
    testUnsignedIntegerFactories();
    testFloatingPointFactories();
    testBoolAndCharFactories();

    testPrimitiveAndUnitTypesAreNeitherErrorNorUnresolved();

    testEqualityIsByKind();
    testCopiesCompareEqual();

    return kai::test::failureCount == 0 ? 0 : 1;
}
