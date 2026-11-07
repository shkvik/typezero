#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/tz_modules/url_checker/url_checker_errors.h"
#include "compiler/result/result/result.h"

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
  using ResultKnownBaseUrl = Result<const Expr *, UrlCheckerKnownBaseError>;
  using ResultUrlTextValid = Result<llvm::StringRef, UrlCheckerUrlTextError>;
  using ResultInputIsKnown = Result<const Expr *, UrlCheckerKnownInputError>;
  using ResultSettableText = Result<llvm::StringRef, UrlCheckerSetterValueError>;

  ResultKnownBaseUrl knownBase(const Expr *base) const;
  ResultInputIsKnown knownInput(const Expr *input, bool based) const;
  static ResultUrlTextValid urlText(llvm::StringRef text, bool based);
  static ResultSettableText setterValue(llvm::StringRef name, llvm::StringRef value);
  static bool validHost(llvm::StringRef host);

  static bool isLibraryClass(const Type *type, llvm::StringRef name);

  CheckState &state;
};

}
