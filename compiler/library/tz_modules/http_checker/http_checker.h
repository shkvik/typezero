#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class HttpChecker {
public:
  explicit HttpChecker(Checker &checker);

  void checkResponseNew(NewExpr *expr);
  void checkRequestNew(NewExpr *expr);
  void checkHeadersNew(NewExpr *expr);
  void checkHeadersCall(const CallExpr *call, const MemberExpr *member);
  static bool validReason(const std::u16string &text);

private:
  void checkHeadersInit(const Expr *init);
  void checkHeaderName(const Expr *name);
  void checkHeaderValue(const Expr *value);
  bool inProgram(const Expr *expr) const;

  Checker &checker_;
};

}
