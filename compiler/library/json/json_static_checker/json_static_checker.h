#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class JsonStaticChecker {
public:
  explicit JsonStaticChecker(Checker &checker);

  Expr *checkJsonStatic(CallExpr *call, MemberExpr *member);
  Expr *checkJsonGlobal(CallExpr *call, NameExpr *name);
  bool readEmbedded(CallExpr *call, llvm::StringRef fn, std::string &bytes, std::string &shown,
                    std::string &full);

private:
  void checkConstantJson(const Expr *arg);

  Checker &checker_;
};

}
