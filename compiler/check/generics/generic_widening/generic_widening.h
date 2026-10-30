#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class GenericWidening {
public:
  explicit GenericWidening(Checker &checker);

  static bool widens(const Type *from, const Type *to);
  static bool covariant(const ClassDecl *generic, size_t index);
  static const ClassDecl *instanceOf(const ClassDecl *cls, const ClassDecl *generic);
  static std::string whyNot(const Type *from, const Type *to);
  Expr *joined(Expr *expr);

private:
  Checker &checker_;
};

}
