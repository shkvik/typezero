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

  static bool holdsBytes(const Type *type);
  static bool isJwkObject(const Type *type);
  static Expr *argAt(const CallExpr *call, size_t i);
  static const Expr *withoutImplicitCasts(const Expr *expr);
  static const Expr *algorithmNameExpr(const Expr *arg, const ObjectLitExpr *lit);
  static int importedPublicness(const std::string &format, const std::string &name);
  static std::vector<llvm::StringLiteral> allowedUsages(llvm::StringRef name, int isPublic);
  bool keyDataFits(const SubtleCall &subtle);
  void walkWrapKey(SubtleCall &subtle);
  void walkDeriveKey(SubtleCall &subtle);
  void walkExportKey(SubtleCall &subtle);
  void walkImportKey(SubtleCall &subtle);
  void walkUnwrapKey(SubtleCall &subtle);
  void walkGenerateKey(SubtleCall &subtle);
  void wrapJwkArgument(SubtleCall &subtle);
  void walkSubtleArguments(SubtleCall &subtle);
  bool reportsBadCurve(const ObjectLitExpr *lit);
  Expr *checkRejectedSubtleCall(SubtleCall &subtle);
  std::string keyFormatAt(SubtleCall &subtle, size_t i);
  void walkAlgorithmOnly(SubtleCall &subtle, llvm::StringRef op);
  const Expr *algorithmAt(const CallExpr *call, size_t i) const;
  Expr *finishSubtleCall(SubtleCall &subtle, const Type *expected);
  bool reportsBadGcmTag(const ObjectLitExpr *lit, llvm::StringRef name);
  bool reportsBadHash(const ObjectLitExpr *lit, llvm::StringRef dictionary);
  bool reportsBadAesLength(const ObjectLitExpr *lit, llvm::StringRef name, llvm::StringRef op);
  void walkOwnUsage(SubtleCall &subtle, const ArrayLitExpr *lit, llvm::StringRef name,
                    int isPublic);
  void walkUsages(SubtleCall &subtle, size_t i, llvm::StringRef name, int isPublic, bool creates);
  void walkImportFormat(SubtleCall &subtle, const std::string &format, const std::string &name,
                        bool unwraps);
  std::string algorithmNameAt(SubtleCall &subtle, size_t i, llvm::StringRef op,
                              llvm::StringRef fallback);

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
};

}
