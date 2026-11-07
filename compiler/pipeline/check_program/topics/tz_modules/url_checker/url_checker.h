#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class UrlChecker {
public:
  explicit UrlChecker(CheckState &owner);

  void checkUrlNew(NewExpr *expr);
  bool checkUrlSetter(MemberExpr *member, const CallExpr *call);
  static bool validUrlText(llvm::StringRef text, bool relative);
  static std::optional<std::string> literalText(const Expr *expr);
  static const Expr *unconverted(const Expr *expr);
  bool knownAbsolute(const Expr *expr) const;

private:
  static bool validHost(llvm::StringRef host);

  static bool isLibraryClass(const Type *type, llvm::StringRef name);

  CheckState &state;
};

}
