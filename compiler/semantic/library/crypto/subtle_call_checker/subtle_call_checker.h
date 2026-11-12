#pragma once

#include "compiler/program/library/crypto/subtle_method_table/subtle_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/subtle_algorithms/subtle_algorithms.h"
#include "compiler/semantic/library/crypto/subtle_call_checker/subtle_call_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CallChecker;
class ExprChecker;
class ClassDeclarer;
class ArgumentChecker;
class MethodCallChecker;
class EventTargetChecker;

class SubtleCallChecker {
public:
  SubtleCallChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkSubtleCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  using ResultAesLengthValid = Result<double, SubtleCallCheckerAesLengthError>;
  using ResultCurveNameKnown = Result<llvm::StringRef, SubtleCallCheckerKnownCurveError>;
  using ResultGcmTagLengthOk = Result<double, SubtleCallCheckerGcmTagLengthError>;
  using ResultShaHashIsKnown = Result<llvm::StringRef, SubtleCallCheckerKnownShaHashError>;
  using ResultKeyFormatKnown = Result<llvm::StringRef, SubtleCallCheckerKnownFormatError>;
  using ResultParamsAsObject = Result<llvm::StringRef, SubtleCallCheckerParamsAsObjectError>;
  using ResultMembersPresent = Result<const ObjectLitExpr *, SubtleCallCheckerMembersPresentError>;
  using ResultAlgorithmKnown = Result<std::string, SubtleCallCheckerKnownAlgorithmError>;

  ResultAesLengthValid checkAesLength(double bits) const;
  ResultGcmTagLengthOk checkGcmTagLength(double bits) const;
  ResultCurveNameKnown checkKnownCurve(llvm::StringRef curve) const;
  ResultKeyFormatKnown checkKnownFormat(llvm::StringRef format) const;
  ResultShaHashIsKnown checkKnownShaHash(llvm::StringRef hash, llvm::StringRef dictionary) const;
  ResultParamsAsObject checkParamsAsObject(llvm::StringRef name, llvm::StringRef op,
                                           const Dictionary &dictionary) const;
  ResultMembersPresent checkMembersPresent(llvm::StringRef name, llvm::StringRef op,
                                           const Dictionary &dictionary,
                                           const ObjectLitExpr *literal) const;
  ResultAlgorithmKnown checkKnownAlgorithm(llvm::StringRef written, llvm::StringRef op,
                                           llvm::StringRef fallback) const;
  std::string algorithmIn(llvm::StringRef op, llvm::StringRef upper) const;
  std::optional<double> numberLiteral(const Expr *expr) const;
  const ObjectLitExpr::Property *property(const ObjectLitExpr *literal, llvm::StringRef name) const;
  const Expr *accessInner(const Expr *expr) const;
  bool isJsonValue(const Expr *expr) const;

  struct SubtleCall {
    CallExpr *call = nullptr;
    MemberExpr *member = nullptr;
    SubtleMethod method = SubtleMethod::DIGEST;
    std::vector<bool> checkedArgs;
    std::string keyFormat;
    std::string target;
    bool bad = false;
  };

  std::string literalAlgorithm(const Expr *arg, llvm::StringRef op, llvm::StringRef fallback,
                               bool &bad);

  bool isHoldingBytes(const Type *type) const;
  bool isJwkObject(const Type *type) const;
  Expr *argAt(const CallExpr *call, size_t index) const;
  const Expr *withoutImplicitCasts(const Expr *expr) const;
  const Expr *algorithmNameExpr(const Expr *arg, const ObjectLitExpr *literal) const;
  int importedPublicness(const std::string &format, const std::string &name) const;
  std::vector<llvm::StringLiteral> allowedUsages(llvm::StringRef name, int isPublic) const;
  bool matchKeyData(const SubtleCall &subtle);
  void walkWrapKey(SubtleCall &subtle);
  void walkDeriveKey(SubtleCall &subtle);
  void walkExportKey(SubtleCall &subtle);
  void walkImportKey(SubtleCall &subtle);
  void walkUnwrapKey(SubtleCall &subtle);
  void walkGenerateKey(SubtleCall &subtle);
  void wrapJwkArgument(SubtleCall &subtle);
  void walkSubtleArguments(SubtleCall &subtle);
  bool reportBadCurve(const ObjectLitExpr *literal);
  Expr *checkRejectedSubtleCall(SubtleCall &subtle);
  std::string keyFormatAt(SubtleCall &subtle, size_t index);
  void walkAlgorithmOnly(SubtleCall &subtle, llvm::StringRef op);
  const Expr *algorithmAt(const CallExpr *call, size_t index) const;
  Expr *finishSubtleCall(SubtleCall &subtle, const Type *expected);
  bool reportBadGcmTag(const ObjectLitExpr *literal, llvm::StringRef name);
  bool reportBadHash(const ObjectLitExpr *literal, llvm::StringRef dictionary);
  bool reportBadAesLength(const ObjectLitExpr *literal, llvm::StringRef name, llvm::StringRef op);
  void walkOwnUsage(SubtleCall &subtle, const ArrayLitExpr *literal, llvm::StringRef name,
                    int isPublic);
  void walkUsages(SubtleCall &subtle, size_t index, llvm::StringRef name, int isPublic,
                  bool creates);
  void walkImportFormat(SubtleCall &subtle, const std::string &format, const std::string &name,
                        bool unwraps);
  std::string algorithmNameAt(SubtleCall &subtle, size_t index, llvm::StringRef op,
                              llvm::StringRef fallback);

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
};

}
