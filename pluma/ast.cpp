#include "pluma/ast.hpp"

namespace pluma {

std::string_view to_string(UnaryOp op) {
    switch (op) {
        case UnaryOp::Neg: return "-";
        case UnaryOp::Not: return "!";
        case UnaryOp::BitNot: return "~";
    }
    return "?";
}

std::string_view to_string(BinaryOp op) {
    switch (op) {
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Rem: return "%";
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Shl: return "<<";
        case BinaryOp::Shr: return ">>";
        case BinaryOp::BitAnd: return "&";
        case BinaryOp::BitXor: return "^";
        case BinaryOp::BitOr: return "|";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::Eq: return "==";
        case BinaryOp::Ne: return "!=";
        case BinaryOp::And: return "&&";
        case BinaryOp::Or: return "||";
        case BinaryOp::Coalesce: return "??";
    }
    return "?";
}

std::string_view to_string(AssignOp op) {
    switch (op) {
        case AssignOp::Set: return "=";
        case AssignOp::Add: return "+=";
        case AssignOp::Sub: return "-=";
        case AssignOp::Mul: return "*=";
        case AssignOp::Div: return "/=";
        case AssignOp::Rem: return "%=";
        case AssignOp::BitAnd: return "&=";
        case AssignOp::BitOr: return "|=";
        case AssignOp::BitXor: return "^=";
        case AssignOp::Shl: return "<<=";
        case AssignOp::Shr: return ">>=";
    }
    return "?";
}

std::string_view to_string(NumSuffix suffix) {
    switch (suffix) {
        case NumSuffix::None: return "";
        case NumSuffix::I8: return "i8";
        case NumSuffix::I16: return "i16";
        case NumSuffix::I32: return "i32";
        case NumSuffix::I64: return "i64";
        case NumSuffix::U8: return "u8";
        case NumSuffix::U16: return "u16";
        case NumSuffix::U32: return "u32";
        case NumSuffix::U64: return "u64";
        case NumSuffix::F32: return "f32";
        case NumSuffix::F64: return "f64";
        case NumSuffix::Deg: return "deg";
    }
    return "?";
}

Prec precedence(BinaryOp op) {
    switch (op) {
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Rem: return Prec::Multiplicative;
        case BinaryOp::Add:
        case BinaryOp::Sub: return Prec::Additive;
        case BinaryOp::Shl:
        case BinaryOp::Shr: return Prec::Shift;
        case BinaryOp::BitAnd: return Prec::BitAnd;
        case BinaryOp::BitXor: return Prec::BitXor;
        case BinaryOp::BitOr: return Prec::BitOr;
        case BinaryOp::Lt:
        case BinaryOp::Le:
        case BinaryOp::Gt:
        case BinaryOp::Ge: return Prec::Comparison;
        case BinaryOp::Eq:
        case BinaryOp::Ne: return Prec::Equality;
        case BinaryOp::And: return Prec::And;
        case BinaryOp::Or: return Prec::Or;
        case BinaryOp::Coalesce: return Prec::Coalesce;
    }
    return Prec::Lowest;
}

Prec precedence(const Expr* expr) {
    switch (expr->kind) {
        case ExprKind::Binary: return precedence(static_cast<const BinaryExpr*>(expr)->op);
        case ExprKind::Range: return Prec::Range;
        case ExprKind::Ternary: return Prec::Ternary;
        case ExprKind::Is: return Prec::Comparison;
        case ExprKind::As: return Prec::As;
        case ExprKind::Unary:
        case ExprKind::Await: return Prec::Unary;
        case ExprKind::Call:
        case ExprKind::Index:
        case ExprKind::Member:
        case ExprKind::Try: return Prec::Postfix;
        // A block lambda is self-delimiting (`fn() { ... }()` is a call); an
        // arrow lambda's body extends as far right as possible.
        case ExprKind::Lambda:
            return static_cast<const LambdaExpr*>(expr)->arrow ? Prec::Lambda : Prec::Primary;
        default: return Prec::Primary;
    }
}

} // namespace pluma
