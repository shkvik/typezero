#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/tz_modules/foreign_signatures/foreign_signatures.h"
#include "compiler/semantic/library/tz_modules/library_tz_modules_registry_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

class LibraryTzModulesRegistry {
public:
  explicit LibraryTzModulesRegistry(SemanticState &owner);

  void walkExternal(const WalkExternal &task);
  void noteHttpsCall(const NoteHttpsCall &task);
  void noteTlsMethod(const NoteTlsMethod &task);
  void noteHttpsValue(const NoteHttpsValue &task);
  void walkHeadersCall(const WalkHeadersCall &task);
  void noteLibraryInstance(const NoteLibraryInstance &task);

private:
  using ResultHeaderName = Result<llvm::StringRef, LibraryTzModulesRegistryHeaderNameError>;
  using ResultRequestUrl = Result<llvm::StringRef, LibraryTzModulesRegistryRequestUrlError>;
  using ResultMethodName = Result<llvm::StringRef, LibraryTzModulesRegistryMethodNameError>;
  using ResultUrlTextValid = Result<llvm::StringRef, LibraryTzModulesRegistryUrlTextError>;
  using ResultHeaderText = Result<const std::u16string *, LibraryTzModulesRegistryHeaderTextError>;
  using ResultHeaderNameChecked = Result<void, LibraryTzModulesRegistryHeaderNameError>;
  using ResultHeaderValueChecked = Result<void, LibraryTzModulesRegistryHeaderTextError>;
  using ResultKnownBaseUrl = Result<const Expr *, LibraryTzModulesRegistryKnownBaseError>;
  using ResultInputIsKnown = Result<const Expr *, LibraryTzModulesRegistryKnownInputError>;
  using ResultExternalParams = Result<const Type *, LibraryTzModulesRegistryExternalParamError>;
  using ResultExternalReturn = Result<const Type *, LibraryTzModulesRegistryExternalReturnError>;
  using ResultPlainSignature =
      Result<const FunctionDecl *, LibraryTzModulesRegistryPlainSignatureError>;

  struct RequestMethod {
    std::string name;
    bool isKnown;
  };

  void walkUrlNew(NewExpr *expr);
  void walkHeadersNew(NewExpr *expr);
  void walkRequestNew(NewExpr *expr);
  void walkResponseNew(NewExpr *expr);
  void noteServer(const NewExpr *made);
  void walkHeadersInit(const Expr *init);
  void walkStatusText(const Expr *value);
  void walkRequestInput(const Expr *input);
  void noteServeOptions(const Expr *options);
  void walkHeaderPair(const ArrayLitExpr::Element &element);
  void walkHeaderProperty(const ObjectLitExpr::Property &property);
  void walkResponseStatus(llvm::ArrayRef<Expr *> args, const Expr *value);
  void walkResponseProperty(llvm::ArrayRef<Expr *> args, const ObjectLitExpr::Property &property);


  bool hasBody(const Expr *body) const;
  bool isHttpSpace(char16_t unit) const;
  bool isReasonUnit(char16_t unit) const;
  bool isTokenUnit(char character) const;
  bool isFailedArg(const Expr *arg) const;
  bool isInProgram(const Expr *expr) const;
  bool isToken(llvm::StringRef text) const;
  bool isExternalValue(const Type *type) const;
  bool isInsecureValue(const Expr *value) const;
  bool hasCredentials(llvm::StringRef url) const;
  bool isExternalCallback(const Type *type) const;
  bool isTokenChar(unsigned char character) const;
  bool isExternalReference(const Type *type) const;
  bool isPlainCallbackParam(const Type *param) const;
  bool isPlainTransport(const Expr *transport) const;
  bool isValidValue(const std::u16string &text) const;
  bool isValidReason(const std::u16string &text) const;
  bool isSecuredProperty(const ObjectLitExpr::Property &property) const;


  const Expr *uncast(const Expr *expr) const;
  ResultHeaderNameChecked checkHeaderName(const Expr *name);
  ResultKnownBaseUrl checkKnownBase(const Expr *base) const;
  const std::u16string *stringLiteral(const Expr *expr) const;
  ResultHeaderValueChecked checkHeaderValue(const Expr *value);
  ResultRequestUrl checkRequestUrl(llvm::StringRef text) const;
  ResultMethodName checkMethodName(llvm::StringRef method) const;
  std::optional<int64_t> statusConstant(const Expr *value) const;
  ResultHeaderName checkHeaderNameText(llvm::StringRef name) const;
  ResultHeaderText checkHeaderText(const std::u16string &text) const;
  RequestMethod requestMethod(const ObjectLitExpr *init, const Expr *input);
  ResultInputIsKnown checkKnownInput(const Expr *input, bool isBased) const;
  ResultUrlTextValid checkUrlText(llvm::StringRef text, bool isBased) const;
  ResultPlainSignature checkPlainSignature(const FunctionDecl *functionDecl) const;
  ResultExternalParams checkExternalParam(const Param &param, const Type *type) const;
  ResultExternalReturn checkExternalReturn(const Type *returnType, const Type *declared) const;
  const ObjectLitExpr::Property *property(const ObjectLitExpr *literal, llvm::StringRef name) const;

  SemanticState &state;
  ForeignSignatures foreign;
};

}
