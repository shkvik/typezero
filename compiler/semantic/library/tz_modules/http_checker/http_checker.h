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
  bool validReason(const std::u16string &text) const;

private:
  using ResultHeaderName = Result<llvm::StringRef, HttpCheckerHeaderNameError>;
  using ResultRequestUrl = Result<llvm::StringRef, HttpCheckerRequestUrlError>;
  using ResultMethodName = Result<llvm::StringRef, HttpCheckerMethodNameError>;
  using ResultHeaderText = Result<const std::u16string *, HttpCheckerHeaderTextError>;
  using ResultHeaderNameChecked = Result<void, HttpCheckerHeaderNameError>;
  using ResultHeaderValueChecked = Result<void, HttpCheckerHeaderTextError>;

  ResultHeaderName headerName(llvm::StringRef name) const;
  ResultRequestUrl requestUrl(llvm::StringRef text) const;
  ResultMethodName methodName(llvm::StringRef method) const;
  ResultHeaderText headerText(const std::u16string &text) const;
  bool isTokenChar(unsigned char c) const;
  bool isToken(llvm::StringRef text) const;
  bool isHttpSpace(char16_t c) const;
  bool validValue(const std::u16string &text) const;
  const std::u16string *stringLiteral(const Expr *expr) const;
  bool isLibraryClass(const Type *type, llvm::StringRef name) const;
  const ObjectLitExpr::Property *property(const ObjectLitExpr *literal, llvm::StringRef name) const;
  bool hasBody(const Expr *body) const;
  bool hasCredentials(llvm::StringRef url) const;

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
