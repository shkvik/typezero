#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/tz_modules/http_checker/http_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class HttpChecker {
public:
  explicit HttpChecker(CheckState &owner);

  void checkResponseNew(NewExpr *expr);
  void checkRequestNew(NewExpr *expr);
  void checkHeadersNew(NewExpr *expr);
  void checkHeadersCall(const CallExpr *call, const MemberExpr *member);
  static bool validReason(const std::u16string &text);

private:
  using ResultHeaderName = Result<llvm::StringRef, HttpCheckerHeaderNameError>;
  using ResultRequestUrl = Result<llvm::StringRef, HttpCheckerRequestUrlError>;
  using ResultMethodName = Result<llvm::StringRef, HttpCheckerMethodNameError>;
  using ResultHeaderText = Result<const std::u16string *, HttpCheckerHeaderTextError>;

  static ResultHeaderName headerName(llvm::StringRef name);
  static ResultRequestUrl requestUrl(llvm::StringRef text);
  static ResultMethodName methodName(llvm::StringRef method);
  static ResultHeaderText headerText(const std::u16string &text);
  static bool isTokenChar(unsigned char c);
  static bool isToken(llvm::StringRef text);
  static bool isHttpSpace(char16_t c);
  static bool validValue(const std::u16string &text);
  static const std::u16string *stringLiteral(const Expr *expr);
  static bool isLibraryClass(const Type *type, llvm::StringRef name);
  static const ObjectLitExpr::Property *property(const ObjectLitExpr *lit, llvm::StringRef name);
  static bool hasBody(const Expr *body);
  static bool hasCredentials(llvm::StringRef url);

  void checkHeadersInit(const Expr *init);
  void checkHeaderName(const Expr *name);
  void checkHeaderValue(const Expr *value);
  bool inProgram(const Expr *expr) const;

  CheckState &state;
};

}
