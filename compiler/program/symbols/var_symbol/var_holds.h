#pragma once

#include "compiler/union/union/union.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

struct FunctionDecl;
class RegexLitExpr;

enum class VarHoldsKind {
  ORDINARY_VALUE,
  DECLARED_FUNCTION,
  LAMBDA,
  REGEX_LITERAL,
  REFLECTED_METHOD,
  REFLECTED_NAMES,
};

struct OrdinaryValue {
  static constexpr VarHoldsKind kind = VarHoldsKind::ORDINARY_VALUE;
};

struct DeclaredFunction {
  static constexpr VarHoldsKind kind = VarHoldsKind::DECLARED_FUNCTION;
  FunctionDecl *function = nullptr;
};

struct Lambda {
  static constexpr VarHoldsKind kind = VarHoldsKind::LAMBDA;
  FunctionDecl *lambda = nullptr;
};

struct RegexLiteral {
  static constexpr VarHoldsKind kind = VarHoldsKind::REGEX_LITERAL;
  const RegexLitExpr *regex = nullptr;
};

struct ReflectedMethod {
  static constexpr VarHoldsKind kind = VarHoldsKind::REFLECTED_METHOD;
  const FunctionDecl *method = nullptr;
};

struct ReflectedNames {
  static constexpr VarHoldsKind kind = VarHoldsKind::REFLECTED_NAMES;
  std::vector<llvm::StringRef> names;
};

using VarHolds =
    Union<OrdinaryValue, DeclaredFunction, Lambda, RegexLiteral, ReflectedMethod, ReflectedNames>;

}
