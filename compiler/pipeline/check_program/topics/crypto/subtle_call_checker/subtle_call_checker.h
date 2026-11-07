#pragma once

#include "compiler/library/crypto/subtle_algorithms/subtle_algorithms.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/crypto/subtle_call_checker/subtle_call_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class SubtleCallChecker {
public:
  explicit SubtleCallChecker(CheckState &owner);

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

  std::string literalAlgorithm(const Expr *arg, llvm::StringRef op, llvm::StringRef fallback,
                               bool &bad);

  CheckState &state;
};

}
