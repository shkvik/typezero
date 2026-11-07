#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class HttpChecker {
public:
  explicit HttpChecker(Checker &owner);

  void checkResponseNew(NewExpr *expr);
  void checkRequestNew(NewExpr *expr);
  void checkHeadersNew(NewExpr *expr);
  void checkHeadersCall(const CallExpr *call, const MemberExpr *member);
  static bool validReason(const std::u16string &text);

private:
  static bool isTokenChar(unsigned char c);
  static bool isToken(llvm::StringRef text);
  static bool isHttpSpace(char16_t c);
  static bool validValue(const std::u16string &text);
  static const std::u16string *stringLiteral(const Expr *expr);
  static std::string utf8(const std::u16string &units);
  static bool isLibraryClass(const Type *type, llvm::StringRef name);
  static const ObjectLitExpr::Property *property(const ObjectLitExpr *lit, llvm::StringRef name);
  static bool hasBody(const Expr *body);
  static bool hasCredentials(llvm::StringRef url);

  void checkHeadersInit(const Expr *init);
  void checkHeaderName(const Expr *name);
  void checkHeaderValue(const Expr *value);
  bool inProgram(const Expr *expr) const;

  Checker &checker;
};

}
