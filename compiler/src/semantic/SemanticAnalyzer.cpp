#include "kai/semantic/SemanticAnalyzer.hpp"

#include <optional>
#include <string_view>
#include <utility>

namespace kai::semantic {

namespace {

// Exact GRAMMAR.md §12 primitive_type list. Deliberately not a
// generic Type::primitive(TypeKind) lookup: Type's constructor is
// private with no such entry point (see Type.hpp) specifically so
// Error/Unresolved can never be constructed by anything other than
// their own named factories - this table calls each factory by name
// instead of routing through a shared TypeKind-keyed path.
std::optional<Type> lookupPrimitiveTypeName(std::string_view name) {
    if (name == "i8") return Type::i8();
    if (name == "i16") return Type::i16();
    if (name == "i32") return Type::i32();
    if (name == "i64") return Type::i64();
    if (name == "u8") return Type::u8();
    if (name == "u16") return Type::u16();
    if (name == "u32") return Type::u32();
    if (name == "u64") return Type::u64();
    if (name == "f32") return Type::f32();
    if (name == "f64") return Type::f64();
    if (name == "bool") return Type::boolean();
    if (name == "char") return Type::character();
    return std::nullopt;
}

} // namespace

SemanticAnalyzer::SemanticAnalyzer(const SourceManager& sources) noexcept : sources_(sources) {}

SemanticModel SemanticAnalyzer::analyze(const ast::SourceFile& file) {
    SemanticModel model;
    std::unordered_map<std::string, SymbolId> topLevelNames;

    for (const auto& decl : file.declarations()) {
        collectTopLevelDeclaration(*decl, model, topLevelNames);
    }

    return model;
}

// No `default:` case: DeclKind is fully implemented today (the same
// idiom AstPrinter.cpp's printDecl() already uses), so -Wswitch fires
// the moment a new DeclKind is added without a case here.
void SemanticAnalyzer::collectTopLevelDeclaration(const ast::Decl& decl, SemanticModel& model,
                                                   std::unordered_map<std::string, SymbolId>& topLevelNames) const {
    switch (decl.kind()) {
        case ast::DeclKind::Function:
            collectFunctionDecl(static_cast<const ast::FunctionDecl&>(decl), model, topLevelNames);
            return;
    }
}

void SemanticAnalyzer::collectFunctionDecl(const ast::FunctionDecl& fn, SemanticModel& model,
                                            std::unordered_map<std::string, SymbolId>& topLevelNames) const {
    const std::string name(sources_.text(fn.name().span));

    // Checked before this declaration's own signature is resolved, so
    // the deterministic, source-order error sequence for
    // `fn a(x: Foo) {} fn a(y: Bar) {}` is: UnknownType(Foo),
    // DuplicateSymbol(second a), UnknownType(Bar) - the duplicate check
    // for a declaration happens immediately once its name is known,
    // before its parameter/return types are inspected.
    const auto existing = topLevelNames.find(name);
    if (existing != topLevelNames.end()) {
        const Symbol& originalSymbol = model.symbol(existing->second);
        model.addError(SemanticError{
            SemanticErrorKind::DuplicateSymbol,
            fn.name().span,
            originalSymbol.declaredAt,
        });
    }

    FunctionSignature signature = resolveFunctionSignature(fn, model);

    // Every syntactically-present declaration gets its own Symbol,
    // including duplicates: declarationSymbol(fn.name()) must stay
    // meaningful for tooling inspecting the invalid declaration itself,
    // even though a duplicate's name never enters `topLevelNames` below
    // (approved "first declaration wins" policy - see #13 of this
    // phase's design).
    Symbol symbol{
        SymbolKind::Function, name, fn.name().span, false, Type::unresolved(), std::move(signature),
    };
    const SymbolId id = model.addSymbol(std::move(symbol));
    model.recordDeclaration(fn.name(), id);

    if (existing == topLevelNames.end()) {
        topLevelNames.emplace(name, id);
    }
}

FunctionSignature SemanticAnalyzer::resolveFunctionSignature(const ast::FunctionDecl& fn, SemanticModel& model) const {
    std::vector<Type> parameterTypes;
    parameterTypes.reserve(fn.params().size());

    for (const ast::Param& param : fn.params()) {
        parameterTypes.push_back(resolveTypeSyntax(*param.type, model));
    }

    // GRAMMAR.md §9's return type is optional; TYPE_SYSTEM.md §11
    // commits the no-annotation case to the same semantic Unit an
    // explicit `-> ()` produces. The AST alone preserves which form the
    // source actually used (FunctionDecl::returnType() == nullptr vs. a
    // real UnitTypeSyntax) - the semantic signature does not.
    const Type returnType = fn.returnType() == nullptr ? Type::unit() : resolveTypeSyntax(*fn.returnType(), model);

    return FunctionSignature{std::move(parameterTypes), returnType};
}

Type SemanticAnalyzer::resolveTypeSyntax(const ast::TypeSyntax& type, SemanticModel& model) const {
    switch (type.kind()) {
        case ast::TypeSyntaxKind::Named:
            return resolveNamedTypeSyntax(static_cast<const ast::NamedTypeSyntax&>(type), model);
        case ast::TypeSyntaxKind::Unit:
            return Type::unit();
        case ast::TypeSyntaxKind::Reference:
        case ast::TypeSyntaxKind::Slice:
        case ast::TypeSyntaxKind::Array:
        case ast::TypeSyntaxKind::Generic:
            // Deferred: this phase does not model these semantic shapes
            // yet. Type::unresolved(), not Type::error() - nothing was
            // attempted, so nothing failed (Type.hpp's
            // Unresolved-vs-Error distinction). No SemanticError, and no
            // partial inspection of what's nested inside an entirely
            // deferred shape: e.g. `&Foo`'s `Foo` is never looked at
            // here, even though `Foo` alone would be UnknownType.
            return Type::unresolved();
    }

    // Unreachable while TypeSyntaxKind's enumerators match the switch
    // above exactly - kept only so -Wreturn-type doesn't warn; the
    // switch itself still has no `default:`, so -Wswitch still fires
    // the moment a new TypeSyntaxKind is added without a case here
    // (same idiom as TokenKind.cpp's tokenKindName()).
    return Type::error();
}

Type SemanticAnalyzer::resolveNamedTypeSyntax(const ast::NamedTypeSyntax& type, SemanticModel& model) const {
    if (const std::optional<Type> primitive = lookupPrimitiveTypeName(sources_.text(type.name().span))) {
        return *primitive;
    }

    // str/String/Result/Option/Buffer and any other non-primitive name
    // are all uniformly UnknownType in this phase - no special-casing,
    // per the approved design (no user-defined types exist yet to
    // recognize).
    model.addError(SemanticError{SemanticErrorKind::UnknownType, type.name().span, std::nullopt});
    return Type::error();
}

} // namespace kai::semantic
