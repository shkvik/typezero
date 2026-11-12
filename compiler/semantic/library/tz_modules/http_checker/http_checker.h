#pragma once

#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
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
  bool isValidReason(const std::u16string &text) const;

private:
  using ResultHeaderName = Result<llvm::StringRef, HttpCheckerHeaderNameError>;
  using ResultRequestUrl = Result<llvm::StringRef, HttpCheckerRequestUrlError>;
  using ResultMethodName = Result<llvm::StringRef, HttpCheckerMethodNameError>;
  using ResultHeaderText = Result<const std::u16string *, HttpCheckerHeaderTextError>;
  using ResultHeaderNameChecked = Result<void, HttpCheckerHeaderNameError>;
  using ResultHeaderValueChecked = Result<void, HttpCheckerHeaderTextError>;

  ResultHeaderName checkHeaderNameText(llvm::StringRef name) const;
  ResultRequestUrl checkRequestUrl(llvm::StringRef text) const;
  ResultMethodName checkMethodName(llvm::StringRef method) const;
  ResultHeaderText checkHeaderText(const std::u16string &text) const;
  bool isTokenChar(unsigned char character) const;
  bool isTokenUnit(char character) const;
  bool isToken(llvm::StringRef text) const;
  bool isHttpSpace(char16_t unit) const;
  bool isValidValue(const std::u16string &text) const;
  const std::u16string *stringLiteral(const Expr *expr) const;
  bool isLibraryClass(const Type *type, LibraryClass libraryClass) const;
  const ObjectLitExpr::Property *property(const ObjectLitExpr *literal, llvm::StringRef name) const;
  bool hasBody(const Expr *body) const;
  bool hasCredentials(llvm::StringRef url) const;
  bool isAuthorityEnd(char character) const;
  bool isReasonUnit(char16_t unit) const;
  void walkHeaderProperty(const ObjectLitExpr::Property &property);
  void walkHeaderPair(const ArrayLitExpr::Element &element);
  void walkResponseProperty(llvm::ArrayRef<Expr *> args, const ObjectLitExpr::Property &property);
  void walkResponseStatus(llvm::ArrayRef<Expr *> args, const Expr *value);
  std::optional<int64_t> statusConstant(const Expr *value) const;
  void walkStatusText(const Expr *value);
  bool isFailedArg(const Expr *arg) const;
  void walkRequestInput(const Expr *input);

  struct RequestMethod {
    std::string name;
    bool isKnown;
  };

  RequestMethod requestMethod(const ObjectLitExpr *init, const Expr *input);

  void walkHeadersInit(const Expr *init);
  ResultHeaderNameChecked checkHeaderName(const Expr *name);
  ResultHeaderValueChecked checkHeaderValue(const Expr *value);
  bool isInProgram(const Expr *expr) const;

  SemanticState &state;
  UrlChecker *urlChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  ConstantFolder *constantFolder = nullptr;
};

}
