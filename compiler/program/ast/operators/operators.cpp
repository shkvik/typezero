#include "compiler/program/ast/operators/operators.h"

namespace tz {

const char *Operators::opSpelling(UnaryOp op) {
  switch (op) {
    case UnaryOp::Neg:
      return "-";
    case UnaryOp::Plus:
      return "+";
    case UnaryOp::Not:
      return "!";
    case UnaryOp::BitNot:
      return "~";
    case UnaryOp::TypeOf:
      return "typeof";
    case UnaryOp::Void:
      return "void";
    case UnaryOp::Delete:
      return "delete";
    case UnaryOp::NonNull:
      return "!";
  }

  return "?";
}

const char *Operators::opSpelling(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add:
      return "+";
    case BinaryOp::Sub:
      return "-";
    case BinaryOp::Mul:
      return "*";
    case BinaryOp::Div:
      return "/";
    case BinaryOp::Rem:
      return "%";
    case BinaryOp::Pow:
      return "**";
    case BinaryOp::BitAnd:
      return "&";
    case BinaryOp::BitOr:
      return "|";
    case BinaryOp::BitXor:
      return "^";
    case BinaryOp::Shl:
      return "<<";
    case BinaryOp::Shr:
      return ">>";
    case BinaryOp::UShr:
      return ">>>";
    case BinaryOp::Lt:
      return "<";
    case BinaryOp::Le:
      return "<=";
    case BinaryOp::Gt:
      return ">";
    case BinaryOp::Ge:
      return ">=";
    case BinaryOp::Eq:
      return "==";
    case BinaryOp::Ne:
      return "!=";
    case BinaryOp::LogAnd:
      return "&&";
    case BinaryOp::LogOr:
      return "||";
    case BinaryOp::Nullish:
      return "??";
    case BinaryOp::In:
      return "in";
    case BinaryOp::InstanceOf:
      return "instanceof";
  }

  return "?";
}

}
