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

    // `topLevelNames` doubles as the file scope for Pass 2's lookup
    // chain - it is already exactly a Scope (same map type), already
    // reflects Pass 1's "first declaration wins" policy for duplicate
    // top-level functions, and already contains every top-level function
    // regardless of source order, which is exactly what makes forward
    // references and self/mutual recursion resolve correctly below with
    // no extra bookkeeping.
    const Scope preludeScope = buildPreludeScope(model);

    // Pass 2 only starts once every top-level FunctionDecl already has a
    // Symbol + resolved FunctionSignature (Pass 1, above) - a function's
    // own parameter types are read back from that signature here, never
    // re-resolved.
    for (const auto& decl : file.declarations()) {
        analyzeTopLevelDeclarationBody(*decl, model, preludeScope, topLevelNames);
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

// --- Pass 2: function-body declaration/scope/name analysis ---

SemanticAnalyzer::Scope SemanticAnalyzer::buildPreludeScope(SemanticModel& model) const {
    Scope prelude;
    for (const std::string_view name : {"print", "panic", "assert"}) {
        // No signature (nullopt, not a fabricated "() -> ()"), no
        // declaredAt (nullopt - there is no source declaration), no
        // declarationSymbol() mapping (there is no ast::Identifier to
        // map from). Only the Symbol itself and this Scope's own
        // name -> SymbolId entry exist.
        Symbol symbol{SymbolKind::Builtin, std::string(name), std::nullopt, false, Type::unresolved(), std::nullopt};
        const SymbolId id = model.addSymbol(std::move(symbol));
        prelude.emplace(std::string(name), id);
    }
    return prelude;
}

// No `default:` case: DeclKind is fully implemented today, mirroring
// collectTopLevelDeclaration()'s own exhaustive switch above.
void SemanticAnalyzer::analyzeTopLevelDeclarationBody(const ast::Decl& decl, SemanticModel& model,
                                                       const Scope& preludeScope, const Scope& fileScope) const {
    switch (decl.kind()) {
        case ast::DeclKind::Function:
            analyzeFunctionBody(static_cast<const ast::FunctionDecl&>(decl), model, preludeScope, fileScope);
            return;
    }
}

void SemanticAnalyzer::analyzeFunctionBody(const ast::FunctionDecl& fn, SemanticModel& model,
                                            const Scope& preludeScope, const Scope& fileScope) const {
    // Pass 1 unconditionally creates a Symbol + FunctionSignature for
    // every FunctionDecl (collectFunctionDecl(), above), including
    // duplicates - so this always has a value here.
    const auto fnId = model.declarationSymbol(fn.name());
    assert(fnId.has_value());

    // Copied by value, not held by reference: declareInScope() below
    // calls model.addSymbol() once per parameter, which can reallocate
    // SemanticModel's internal Symbol storage - a reference taken from
    // that storage beforehand would be invalidated by the very first
    // such call. This is the exact bug class Phase 3A's own development
    // hit once already; do not reintroduce it anywhere in this file.
    const std::vector<Type> parameterTypes = model.symbol(*fnId).signature->parameterTypes;

    // Parameters and the function's outermost body block share ONE
    // lexical scope (approved policy - see SemanticAnalyzer.hpp's Scope
    // comment): `fn f(x: i32) { let x = 1 }` is a same-scope
    // DuplicateSymbol, not shadowing. This is why the body below is
    // walked with analyzeBlockContents() directly into `functionScope`,
    // not analyzeNestedBlock(). Parameters are declared before any
    // expression in the body is analyzed, so they are visible to every
    // use inside it.
    Scope functionScope;

    const std::vector<ast::Param>& params = fn.params();
    for (std::size_t i = 0; i < params.size(); ++i) {
        // The parameter's semantic type comes from the signature Pass 1
        // already resolved - never re-resolved here (re-resolving could
        // duplicate an UnknownType error and risks the signature and the
        // Parameter symbol silently disagreeing).
        declareInScope(functionScope, SymbolKind::Parameter, params[i].name, false, parameterTypes[i], model);
    }

    // Lookup chain seeded innermost-known-so-far to outermost:
    // [prelude, file]. A nested scope entered while walking the body
    // pushes one more entry after `fileScope`, closer to the back -
    // lookupIdentifier() always checks the back of this stack first.
    ScopeStack scopeStack{&preludeScope, &fileScope};

    analyzeBlockContents(fn.body(), functionScope, scopeStack, model);
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
    // top-level functions, and the reason later lookups of a duplicated
    // name keep resolving to the first declaration - see #18/#19 of this
    // phase's design).
    Symbol symbol{kind, name, identifier.span, isMutable, type, std::nullopt};
    const SymbolId id = model.addSymbol(std::move(symbol));
    model.recordDeclaration(identifier, id);

    if (existing == scope.end()) {
        scope.emplace(name, id);
    }

    return id;
}

void SemanticAnalyzer::analyzeBlockContents(const ast::BlockStmt& block, Scope& scope, ScopeStack& scopeStack,
                                             SemanticModel& model) const {
    for (const auto& stmt : block.statements()) {
        analyzeStatement(*stmt, scope, scopeStack, model);
    }
}

void SemanticAnalyzer::analyzeNestedBlock(const ast::BlockStmt& block, const Scope& enclosingScope,
                                           ScopeStack& scopeStack, SemanticModel& model) const {
    scopeStack.push_back(&enclosingScope);
    Scope childScope;
    analyzeBlockContents(block, childScope, scopeStack, model);
    scopeStack.pop_back();
}

// No `default:` case: StmtKind is fully implemented today, so -Wswitch
// fires the moment a new StmtKind is added without a case here.
void SemanticAnalyzer::analyzeStatement(const ast::Stmt& stmt, Scope& scope, ScopeStack& scopeStack,
                                         SemanticModel& model) const {
    switch (stmt.kind()) {
        case ast::StmtKind::VarDecl:
            declareLocal(static_cast<const ast::VarDeclStmt&>(stmt), scope, scopeStack, model);
            return;
        case ast::StmtKind::If:
            analyzeIfStmt(static_cast<const ast::IfStmt&>(stmt), scope, scopeStack, model);
            return;
        case ast::StmtKind::While:
            analyzeWhileStmt(static_cast<const ast::WhileStmt&>(stmt), scope, scopeStack, model);
            return;
        case ast::StmtKind::For:
            analyzeForStmt(static_cast<const ast::ForStmt&>(stmt), scope, scopeStack, model);
            return;
        case ast::StmtKind::Block:
            // A bare `{ ... }` block statement is genuinely nested: it
            // is neither a function's own outermost body nor a
            // for-loop's own outermost body, so it gets its own fresh
            // child scope like any other nested block.
            analyzeNestedBlock(static_cast<const ast::BlockStmt&>(stmt), scope, scopeStack, model);
            return;
        case ast::StmtKind::Expr:
            analyzeExpr(static_cast<const ast::ExprStmt&>(stmt).expr(), scope, scopeStack, model);
            return;
        case ast::StmtKind::Return: {
            const auto& returnStmt = static_cast<const ast::ReturnStmt&>(stmt);
            if (const ast::Expr* value = returnStmt.value(); value != nullptr) {
                analyzeExpr(*value, scope, scopeStack, model);
            }
            return;
        }
    }
}

void SemanticAnalyzer::declareLocal(const ast::VarDeclStmt& varDecl, Scope& scope, ScopeStack& scopeStack,
                                     SemanticModel& model) const {
    // Resolve the annotation (if any) using the same resolver Pass 1
    // uses for signatures - same primitive/Unit/UnknownType/Unresolved
    // rules apply identically here. No annotation means Unresolved, not
    // an inferred type: literal/expression-based inference is out of
    // scope for this phase entirely.
    const Type type = varDecl.type() == nullptr ? Type::unresolved() : resolveTypeSyntax(*varDecl.type(), model);

    // Analyzed against `scope`/`scopeStack` exactly as they exist right
    // now - i.e. before the local below is declared into `scope` - so a
    // binding can never be visible inside its own initializer: `let x =
    // x` resolves the RHS `x` against any *outer* x (or
    // UnknownIdentifier if there is none), never the new x being
    // declared on this same line.
    analyzeExpr(varDecl.initializer(), scope, scopeStack, model);

    const bool isMutable = varDecl.binding() == ast::BindingKind::Mutable;
    declareInScope(scope, SymbolKind::Local, varDecl.name(), isMutable, type, model);
}

void SemanticAnalyzer::analyzeIfStmt(const ast::IfStmt& ifStmt, const Scope& scope, ScopeStack& scopeStack,
                                      SemanticModel& model) const {
    // Every branch body - including `else` - gets its own fresh, sibling
    // scope: none of them share a scope with each other or with the
    // enclosing one. Each branch's own condition is analyzed against the
    // ENCLOSING scope/stack, not that branch's own (not-yet-created)
    // body scope.
    for (const ast::IfBranch& branch : ifStmt.branches()) {
        analyzeExpr(*branch.condition, scope, scopeStack, model);
        analyzeNestedBlock(*branch.body, scope, scopeStack, model);
    }
    if (const std::optional<ast::ElseClause>& elseClause = ifStmt.elseClause(); elseClause.has_value()) {
        analyzeNestedBlock(*elseClause->body, scope, scopeStack, model);
    }
}

void SemanticAnalyzer::analyzeWhileStmt(const ast::WhileStmt& whileStmt, const Scope& scope, ScopeStack& scopeStack,
                                         SemanticModel& model) const {
    analyzeExpr(whileStmt.condition(), scope, scopeStack, model);
    analyzeNestedBlock(whileStmt.body(), scope, scopeStack, model);
}

void SemanticAnalyzer::analyzeForStmt(const ast::ForStmt& forStmt, const Scope& scope, ScopeStack& scopeStack,
                                       SemanticModel& model) const {
    // Required order: the iterable is analyzed against the ENCLOSING
    // scope/stack BEFORE the for-scope (and therefore the loop variable)
    // exists at all - `for x in x` resolves the iterable `x` outward,
    // never to the variable this very statement is about to declare.
    analyzeExpr(forStmt.iterable(), scope, scopeStack, model);

    // The loop variable and the body's outermost declarations share ONE
    // scope (analyzeBlockContents directly, not analyzeNestedBlock) -
    // but that one scope is itself freshly nested relative to the
    // surrounding scope (pushed onto scopeStack below), so a same-named
    // outer declaration is shadowed, not duplicated.
    scopeStack.push_back(&scope);
    Scope forScope;
    declareInScope(forScope, SymbolKind::Local, forStmt.variable(), false, Type::unresolved(), model);
    analyzeBlockContents(forStmt.body(), forScope, scopeStack, model);
    scopeStack.pop_back();
}

std::optional<SymbolId> SemanticAnalyzer::lookupIdentifier(const std::string& name, const Scope& scope,
                                                             const ScopeStack& scopeStack) const {
    if (const auto it = scope.find(name); it != scope.end()) {
        return it->second;
    }
    // Innermost-enclosing-first: scopeStack is ordered outermost-first
    // (prelude, file, ...), so walking from the back finds the closest
    // enclosing scope before falling all the way back to file/prelude.
    for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
        const Scope& enclosing = **it;
        if (const auto found = enclosing.find(name); found != enclosing.end()) {
            return found->second;
        }
    }
    return std::nullopt;
}

void SemanticAnalyzer::analyzeIdentifierExpr(const ast::IdentifierExpr& expr, const Scope& scope,
                                              const ScopeStack& scopeStack, SemanticModel& model) const {
    const std::string name(sources_.text(expr.name().span));

    if (const std::optional<SymbolId> id = lookupIdentifier(name, scope, scopeStack)) {
        model.recordResolution(expr, *id);
        return;
    }

    // No fabricated Symbol, no resolution entry - just the error.
    // Traversal continues normally: an unresolved identifier is a
    // recoverable semantic issue, not a reason to stop analyzing the
    // rest of the file (SemanticModel collects a vector<SemanticError>
    // for exactly this reason).
    model.addError(SemanticError{SemanticErrorKind::UnknownIdentifier, expr.name().span, std::nullopt});
}

// No `default:` case: ExprKind is fully implemented today, so -Wswitch
// fires the moment a new ExprKind is added without a case here.
void SemanticAnalyzer::analyzeExpr(const ast::Expr& expr, const Scope& scope, const ScopeStack& scopeStack,
                                    SemanticModel& model) const {
    switch (expr.kind()) {
        case ast::ExprKind::Literal:
        case ast::ExprKind::Unit:
            // No identifier use, and no literal typing here either -
            // see this class's own "no expression typing" note.
            return;

        case ast::ExprKind::Identifier:
            analyzeIdentifierExpr(static_cast<const ast::IdentifierExpr&>(expr), scope, scopeStack, model);
            return;

        case ast::ExprKind::Call: {
            const auto& call = static_cast<const ast::CallExpr&>(expr);
            analyzeExpr(call.callee(), scope, scopeStack, model);
            for (const auto& argument : call.arguments()) {
                analyzeExpr(*argument, scope, scopeStack, model);
            }
            return;
        }

        case ast::ExprKind::Paren:
            analyzeExpr(static_cast<const ast::ParenExpr&>(expr).inner(), scope, scopeStack, model);
            return;

        case ast::ExprKind::Unary:
            analyzeExpr(static_cast<const ast::UnaryExpr&>(expr).operand(), scope, scopeStack, model);
            return;

        case ast::ExprKind::Binary: {
            const auto& binary = static_cast<const ast::BinaryExpr&>(expr);
            analyzeExpr(binary.left(), scope, scopeStack, model);
            analyzeExpr(binary.right(), scope, scopeStack, model);
            return;
        }

        case ast::ExprKind::Assignment: {
            const auto& assignment = static_cast<const ast::AssignmentExpr&>(expr);
            analyzeExpr(assignment.target(), scope, scopeStack, model);
            analyzeExpr(assignment.value(), scope, scopeStack, model);
            return;
        }

        case ast::ExprKind::ArrayLiteral:
            for (const auto& element : static_cast<const ast::ArrayLiteralExpr&>(expr).elements()) {
                analyzeExpr(*element, scope, scopeStack, model);
            }
            return;

        case ast::ExprKind::Index: {
            const auto& index = static_cast<const ast::IndexExpr&>(expr);
            analyzeExpr(index.object(), scope, scopeStack, model);
            analyzeExpr(index.index(), scope, scopeStack, model);
            return;
        }

        case ast::ExprKind::Member:
            // Only the object is a lexical name use. `member` is
            // syntactic metadata whose meaning depends on the object's
            // (not-yet-modeled) type, never resolved through lexical
            // scopes - it must never produce UnknownIdentifier here.
            analyzeExpr(static_cast<const ast::MemberExpr&>(expr).object(), scope, scopeStack, model);
            return;

        case ast::ExprKind::ErrorPropagation:
            analyzeExpr(static_cast<const ast::ErrorPropagationExpr&>(expr).operand(), scope, scopeStack, model);
            return;
    }
}

} // namespace kai::semantic
