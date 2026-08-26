#include "kai/cli/SemanticErrorFormat.hpp"

#include <sstream>

namespace kai::cli {

namespace {

// No `default:` case: SemanticErrorKind is fully implemented today,
// mirroring SemanticAnalyzer.cpp's/TypeChecker.cpp's own exhaustive
// switches over it.
const char* semanticErrorKindName(semantic::SemanticErrorKind kind) {
    switch (kind) {
        case semantic::SemanticErrorKind::DuplicateSymbol:
            return "duplicate symbol";
        case semantic::SemanticErrorKind::UnknownIdentifier:
            return "unknown identifier";
        case semantic::SemanticErrorKind::UnknownType:
            return "unknown type";
        case semantic::SemanticErrorKind::TypeMismatch:
            return "type mismatch";
        case semantic::SemanticErrorKind::LiteralOutOfRange:
            return "literal out of range";
        case semantic::SemanticErrorKind::InvalidUnaryOperand:
            return "invalid unary operand";
        case semantic::SemanticErrorKind::InvalidBinaryOperands:
            return "invalid binary operands";
        case semantic::SemanticErrorKind::InvalidArgumentCount:
            return "invalid argument count";
        case semantic::SemanticErrorKind::NotCallable:
            return "not callable";
        case semantic::SemanticErrorKind::InvalidAssignmentTarget:
            return "invalid assignment target";
        case semantic::SemanticErrorKind::AssignmentToImmutableBinding:
            return "assignment to immutable binding";
        case semantic::SemanticErrorKind::MissingReturn:
            return "missing return";
        case semantic::SemanticErrorKind::UnsupportedStrReturn:
            return "unsupported str return";
    }
    return "semantic error";
}

} // namespace

std::string formatSemanticError(const SourceManager& sources, const semantic::SemanticError& error) {
    const SourceManager::LineColumn where = sources.lineColumn(error.primarySpan.begin());
    std::ostringstream message;
    message << "kaicc: error at " << where.line << ':' << where.column << ": " << semanticErrorKindName(error.kind);
    return message.str();
}

} // namespace kai::cli
