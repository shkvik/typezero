#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// JSON.parse, JSON.stringify and embedJson.
class JsonStaticChecker {
public:
  explicit JsonStaticChecker(Checker &checker);

  // JSON.parse, JSON.parse<T> and JSON.stringify.
  Expr *checkJsonStatic(CallExpr *call, MemberExpr *member);
  // embedJson, embedText, embedBytes and fromJson<T>; null if `name` is none
  // of them.
  Expr *checkJsonGlobal(CallExpr *call, NameExpr *name);
  // Reads the file the argument of embedJson or embedText names, relative to
  // the file of the call; false after reporting an error. `shown` is the
  // path as written.
  bool readEmbedded(CallExpr *call, llvm::StringRef fn, std::string &bytes, std::string &shown,
                    std::string &full);

private:
  Checker &checker_;
};

} // namespace tz
