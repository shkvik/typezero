#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/conversions/code_conversions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class ConversionRules {
public:
  explicit ConversionRules(SemanticState &owner);

  void walkAssignable(const Expr *expr, const Type *to, llvm::StringRef what);


  Expr *convertExpr(Expr *expr, const Type *to);
  Expr *adaptLiteralExpr(Expr *expr, const Type *to);

private:
  struct AssignAttempt {
    const Expr *expr = nullptr;
    const Type *from = nullptr;
    const Type *to = nullptr;
    const Type *fromObject = nullptr;
    const Type *toObject = nullptr;
  };

  using ResultTypeAssignable = Result<const Expr *, CodeConversionsRegistryAssignableError>;

  void addView(const Type *from, const Type *to);
  void rememberView(const Type *from, const Type *to);


  bool isNamedClass(const Type *to) const;
  bool isFormData(const Type *type) const;
  bool isWebStream(const Type *type) const;
  bool isReflectingTarget(const Type *to) const;
  bool hasObjectMember(const Type *unionType) const;
  bool isRequestInit(const AssignAttempt &attempt) const;
  bool isWideningWrites(const Type *from, const Type *to);
  bool isStreamIntoBody(const AssignAttempt &attempt) const;
  bool hasLiteralFieldInAll(llvm::ArrayRef<const Type *> members, llvm::StringRef name) const;


  IntLitExpr *retypedLiteral(Expr *expr, const Type *to) const;
  const Type *otherPlace(const Expr *expr, const Type *to) const;
  const Type *innerObject(const Type *type, const Type *other) const;


  std::string hintName(const Expr *expr) const;
  std::string unionText(const Type *toObject) const;
  std::string genericClassText(const ClassDecl *generic) const;
  const Field *sharedLiteralField(const Type *unionType) const;
  std::string unknownHint(const Expr *expr, const Type *to) const;
  llvm::SMLoc placeLoc(const Expr *expr, const Type *place) const;
  std::string objectMismatchText(const AssignAttempt &attempt) const;
  std::string typeOfCheck(llvm::StringRef shown, const Type *target) const;
  std::string objectMismatch(const Type *from, const Type *to, bool isConflict) const;
  std::string mismatchPrefix(const AssignAttempt &attempt, llvm::StringRef what) const;
  std::string narrowingCheck(llvm::StringRef shown, const Expr *expr, const Type *to) const;
  std::string fieldTypeMismatch(const Field &field, const Field &target, const Type *to,
                                bool isConflict) const;
  std::string mismatchReason(const AssignAttempt &attempt,
                             CodeConversionsRegistryAssignableErrorCode code) const;


  AssignAttempt describeAttempt(const Expr *expr, const Type *to) const;
  CodeConversionsRegistryAssignableErrorCode mismatchOf(const AssignAttempt &attempt) const;
  ResultTypeAssignable assignable(const Expr *expr, const Type *to, llvm::StringRef what) const;

  SemanticState &state;
};

}
