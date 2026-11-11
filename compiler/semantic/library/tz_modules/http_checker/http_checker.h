#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/tz_modules/http_checker/http_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class UrlChecker;
class LibraryCalls;
class ConstantFolder;

class HttpChecker {
public:
  HttpChecker(SemanticState &owner, SemanticParts &all);

  void walkResponseNew(NewExpr *expr);
  void walkRequestNew(NewExpr *expr);
  void walkHeadersNew(NewExpr *expr);
  void walkHeadersCall(const CallExpr *call, const MemberExpr *member);
  static bool validReason(const std::u16string &text);

private:
  using ResultHeaderName = Result<llvm::StringRef, HttpCheckerHeaderNameError>;
  using ResultRequestUrl = Result<llvm::StringRef, HttpCheckerRequestUrlError>;
  using ResultMethodName = Result<llvm::StringRef, HttpCheckerMethodNameError>;
  using ResultHeaderText = Result<const std::u16string *, HttpCheckerHeaderTextError>;
  using ResultHeaderNameChecked = Result<void, HttpCheckerHeaderNameError>;
  using ResultHeaderValueChecked = Result<void, HttpCheckerHeaderTextError>;

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

  void walkHeadersInit(const Expr *init);
  ResultHeaderNameChecked checkHeaderName(const Expr *name);
  ResultHeaderValueChecked checkHeaderValue(const Expr *value);
  bool inProgram(const Expr *expr) const;

  SemanticState &state;
  UrlChecker *urlChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  ConstantFolder *constantFolder = nullptr;
};

}
