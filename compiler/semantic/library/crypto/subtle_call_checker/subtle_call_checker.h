#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/crypto/subtle_algorithms/subtle_algorithms.h"
#include "compiler/semantic/library/crypto/subtle_call_checker/subtle_call_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CallChecker;
class ExprChecker;
class ReturnInferrer;
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

  static ResultAesLengthValid aesLength(double bits);
  static ResultGcmTagLengthOk gcmTagLength(double bits);
  static ResultCurveNameKnown knownCurve(llvm::StringRef curve);
  static ResultKeyFormatKnown knownFormat(llvm::StringRef format);
  static ResultShaHashIsKnown knownShaHash(llvm::StringRef hash, llvm::StringRef dictionary);
  static ResultParamsAsObject paramsAsObject(llvm::StringRef name, llvm::StringRef op,
                                             const Dictionary &dictionary);
  static ResultMembersPresent membersPresent(llvm::StringRef name, llvm::StringRef op,
                                             const Dictionary &dictionary,
                                             const ObjectLitExpr *lit);
  static ResultAlgorithmKnown knownAlgorithm(llvm::StringRef written, llvm::StringRef op,
                                             llvm::StringRef fallback);
  static std::string algorithmIn(llvm::StringRef op, llvm::StringRef upper);
  static std::optional<double> numberLiteral(const Expr *e);
  static const ObjectLitExpr::Property *property(const ObjectLitExpr *lit, llvm::StringRef name);
  static const Expr *accessInner(const Expr *expr);
  bool isJsonValue(const Expr *expr) const;

  std::string literalAlgorithm(const Expr *arg, llvm::StringRef op, llvm::StringRef fallback,
                               bool &bad);

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
};

}
