#include "kai/lexer/TokenKind.hpp"

namespace kai {

// No `default:` case on purpose: adding a TokenKind enumerator without a
// corresponding name here should trigger -Wswitch.
std::string_view tokenKindName(TokenKind kind) noexcept {
    switch (kind) {
        case TokenKind::EndOfFile:
            return "EndOfFile";
        case TokenKind::Invalid:
            return "Invalid";
        case TokenKind::Newline:
            return "Newline";

        case TokenKind::Identifier:
            return "Identifier";

        case TokenKind::IntegerLiteral:
            return "IntegerLiteral";
        case TokenKind::FloatLiteral:
            return "FloatLiteral";
        case TokenKind::StringLiteral:
            return "StringLiteral";
        case TokenKind::CharLiteral:
            return "CharLiteral";

        case TokenKind::KwFn:
            return "KwFn";
        case TokenKind::KwLet:
            return "KwLet";
        case TokenKind::KwMut:
            return "KwMut";
        case TokenKind::KwReturn:
            return "KwReturn";
        case TokenKind::KwIf:
            return "KwIf";
        case TokenKind::KwElse:
            return "KwElse";
        case TokenKind::KwWhile:
            return "KwWhile";
        case TokenKind::KwFor:
            return "KwFor";
        case TokenKind::KwIn:
            return "KwIn";
        case TokenKind::KwStruct:
            return "KwStruct";
        case TokenKind::KwEnum:
            return "KwEnum";
        case TokenKind::KwUse:
            return "KwUse";
        case TokenKind::KwPub:
            return "KwPub";
        case TokenKind::KwAs:
            return "KwAs";
        case TokenKind::KwTrue:
            return "KwTrue";
        case TokenKind::KwFalse:
            return "KwFalse";

        case TokenKind::Plus:
            return "Plus";
        case TokenKind::Minus:
            return "Minus";
        case TokenKind::Star:
            return "Star";
        case TokenKind::Slash:
            return "Slash";
        case TokenKind::Percent:
            return "Percent";

        case TokenKind::Equal:
            return "Equal";
        case TokenKind::EqualEqual:
            return "EqualEqual";
        case TokenKind::BangEqual:
            return "BangEqual";

        case TokenKind::Less:
            return "Less";
        case TokenKind::LessEqual:
            return "LessEqual";
        case TokenKind::Greater:
            return "Greater";
        case TokenKind::GreaterEqual:
            return "GreaterEqual";

        case TokenKind::AmpAmp:
            return "AmpAmp";
        case TokenKind::PipePipe:
            return "PipePipe";
        case TokenKind::Bang:
            return "Bang";
        case TokenKind::Amp:
            return "Amp";

        case TokenKind::Arrow:
            return "Arrow";
        case TokenKind::DotDot:
            return "DotDot";
        case TokenKind::Question:
            return "Question";

        case TokenKind::LeftParen:
            return "LeftParen";
        case TokenKind::RightParen:
            return "RightParen";
        case TokenKind::LeftBrace:
            return "LeftBrace";
        case TokenKind::RightBrace:
            return "RightBrace";
        case TokenKind::LeftBracket:
            return "LeftBracket";
        case TokenKind::RightBracket:
            return "RightBracket";

        case TokenKind::Comma:
            return "Comma";
        case TokenKind::Colon:
            return "Colon";
        case TokenKind::Semicolon:
            return "Semicolon";
        case TokenKind::Dot:
            return "Dot";
    }

    return "Unknown";
}

} // namespace kai
