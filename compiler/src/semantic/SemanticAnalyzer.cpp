#include "kai/semantic/SemanticAnalyzer.hpp"

#include <cassert>
#include <cstddef>
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

    // Pass 2 only starts once every top-level FunctionDecl already has a
    // Symbol + resolved FunctionSignature (Pass 1, above) - a function's
    // own parameter types are read back from that signature here, never
    // re-resolved.
    for (const auto& decl : file.declarations()) {
        analyzeTopLevelDeclarationBody(*decl, model);
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

// --- Pass 2: function-body declaration/scope analysis (Phase 3A) ---

// No `default:` case: DeclKind is fully implemented today, mirroring
// collectTopLevelDeclaration()'s own exhaustive switch above.
void SemanticAnalyzer::analyzeTopLevelDeclarationBody(const ast::Decl& decl, SemanticModel& model) const {
    switch (decl.kind()) {
        case ast::DeclKind::Function:
            analyzeFunctionBody(static_cast<const ast::FunctionDecl&>(decl), model);
            return;
    }
}

void SemanticAnalyzer::analyzeFunctionBody(const ast::FunctionDecl& fn, SemanticModel& model) const {
    // Pass 1 unconditionally creates a Symbol + FunctionSignature for
    // every FunctionDecl (collectFunctionDecl(), above), including
    // duplicates - so this always has a value by the time Pass 2 runs.
    const auto fnId = model.declarationSymbol(fn.name());
    assert(fnId.has_value());

    // Copied by value, not held by reference: declareInScope() below
    // calls model.addSymbol() once per parameter, which can reallocate
    // SemanticModel's internal Symbol storage - a reference taken from
    // that storage beforehand would be invalidated by the very first
    // such call.
    const std::vector<Type> parameterTypes = model.symbol(*fnId).signature->parameterTypes;

    // Parameters and the function's outermost body block share ONE
    // lexical scope (approved policy - see SemanticAnalyzer.hpp's Scope
    // comment): `fn f(x: i32) { let x = 1 }` is a same-scope
    // DuplicateSymbol, not shadowing. This is why the body below is
    // walked with analyzeBlockContents() directly into `functionScope`,
    // not analyzeNestedBlock().
    Scope functionScope;

    const std::vector<ast::Param>& params = fn.params();
    for (std::size_t i = 0; i < params.size(); ++i) {
        // The parameter's semantic type comes from the signature Pass 1
        // already resolved - never re-resolved here (re-resolving could
        // duplicate an UnknownType error and risks the signature and the
        // Parameter symbol silently disagreeing).
        declareInScope(functionScope, SymbolKind::Parameter, params[i].name, false, parameterTypes[i], model);
    }

    analyzeBlockContents(fn.body(), functionScope, model);
}

SymbolId SemanticAnalyzer::declareInScope(Scope& scope, SymbolKind kind, const ast::Identifier& identifier,
                                           bool isMutable, Type type, SemanticModel& model) const {
    const std::string name(sources_.text(identifier.span));

    const auto existing = scope.find(name);
    if (existing != scope.end()) {
        const Symbol& originalSymbol = model.symbol(existing->second);
        model.addError(SemanticError{
            SemanticErrorKind::DuplicateSymbol,
            identifier.span,
            originalSymbol.declaredAt,
        });
    }

    // Every syntactically-present declaration gets its own Symbol,
    // including a duplicate: declarationSymbol(identifier) must stay
    // meaningful for tooling inspecting the invalid declaration itself,
    // even though a duplicate's name never enters `scope` below
    // ("first declaration wins" - same policy Pass 1 already applies to
    // top-level functions).
    Symbol symbol{kind, name, identifier.span, isMutable, type, std::nullopt};
    const SymbolId id = model.addSymbol(std::move(symbol));
    model.recordDeclaration(identifier, id);

    if (existing == scope.end()) {
        scope.emplace(name, id);
    }

    return id;
}

void SemanticAnalyzer::analyzeBlockContents(const ast::BlockStmt& block, Scope& scope, SemanticModel& model) const {
    for (const auto& stmt : block.statements()) {
        analyzeStatement(*stmt, scope, model);
    }
}

void SemanticAnalyzer::analyzeNestedBlock(const ast::BlockStmt& block, SemanticModel& model) const {
    Scope childScope;
    analyzeBlockContents(block, childScope, model);
}

// No `default:` case: StmtKind is fully implemented today, so -Wswitch
// fires the moment a new StmtKind is added without a case here.
void SemanticAnalyzer::analyzeStatement(const ast::Stmt& stmt, Scope& scope, SemanticModel& model) const {
    switch (stmt.kind()) {
        case ast::StmtKind::VarDecl:
            declareLocal(static_cast<const ast::VarDeclStmt&>(stmt), scope, model);
            return;
        case ast::StmtKind::If:
            analyzeIfStmt(static_cast<const ast::IfStmt&>(stmt), model);
            return;
        case ast::StmtKind::While:
            analyzeWhileStmt(static_cast<const ast::WhileStmt&>(stmt), model);
            return;
        case ast::StmtKind::For:
            analyzeForStmt(static_cast<const ast::ForStmt&>(stmt), model);
            return;
        case ast::StmtKind::Block:
            // A bare `{ ... }` block statement is genuinely nested: it
            // is neither a function's own outermost body nor a
            // for-loop's own outermost body, so it gets its own fresh
            // child scope like any other nested block.
            analyzeNestedBlock(static_cast<const ast::BlockStmt&>(stmt), model);
            return;
        case ast::StmtKind::Expr:
        case ast::StmtKind::Return:
            // No expression traversal in this phase: an ExprStmt's
            // expression and a ReturnStmt's value are never inspected -
            // that is Phase 3B (identifier-use resolution).
            return;
    }
}

void SemanticAnalyzer::declareLocal(const ast::VarDeclStmt& varDecl, Scope& scope, SemanticModel& model) const {
    // Resolve the annotation (if any) using the same resolver Pass 1
    // uses for signatures - same primitive/Unit/UnknownType/Unresolved
    // rules apply identically here. No annotation means Unresolved, not
    // an inferred type: literal/expression-based inference is Phase 3B.
    const Type type = varDecl.type() == nullptr ? Type::unresolved() : resolveTypeSyntax(*varDecl.type(), model);

    // Phase 3B will analyze the initializer here, against `scope` as it
    // exists right now - i.e. before the local below is declared into
    // it, so a binding can never be visible inside its own initializer
    // (`let x = x` must resolve `x` on the right against any *outer*
    // x, never the new one). Phase 3A does not resolve identifier uses
    // at all yet, so there is nothing to call here.

    const bool isMutable = varDecl.binding() == ast::BindingKind::Mutable;
    declareInScope(scope, SymbolKind::Local, varDecl.name(), isMutable, type, model);
}

void SemanticAnalyzer::analyzeIfStmt(const ast::IfStmt& ifStmt, SemanticModel& model) const {
    // Conditions are not analyzed in this phase. Every branch body -
    // including `else` - gets its own fresh, sibling scope: none of them
    // share a scope with each other or with the enclosing one.
    for (const ast::IfBranch& branch : ifStmt.branches()) {
        analyzeNestedBlock(*branch.body, model);
    }
    if (const std::optional<ast::ElseClause>& elseClause = ifStmt.elseClause(); elseClause.has_value()) {
        analyzeNestedBlock(*elseClause->body, model);
    }
}

void SemanticAnalyzer::analyzeWhileStmt(const ast::WhileStmt& whileStmt, SemanticModel& model) const {
    // Condition is not analyzed in this phase.
    analyzeNestedBlock(whileStmt.body(), model);
}

void SemanticAnalyzer::analyzeForStmt(const ast::ForStmt& forStmt, SemanticModel& model) const {
    // Iterable is not analyzed in this phase. The loop variable and the
    // body's outermost declarations share ONE scope (analyzeBlockContents
    // directly, not analyzeNestedBlock) - but that one scope is itself
    // freshly nested relative to the surrounding scope, so a same-named
    // outer declaration is shadowed, not duplicated.
    Scope forScope;
    declareInScope(forScope, SymbolKind::Local, forStmt.variable(), false, Type::unresolved(), model);
    analyzeBlockContents(forStmt.body(), forScope, model);
}

} // namespace kai::semantic
