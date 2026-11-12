#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker_errors.h"

namespace tz {

class SemanticParts;
class StaticChecker;
class ArgumentChecker;
class ClassValueChecker;

class UnknownChecker {
public:
  using ResultValueNarrowed = Result<const Expr *, UnknownCheckerUnknownUseError>;
  using ResultMemberChecked = Result<Expr *, UnknownCheckerCheckUnknownMemberError>;
  using ResultMethodChecked = Result<Expr *, UnknownCheckerCheckUnknownMethodError>;

  UnknownChecker(SemanticState &owner, SemanticParts &all);

  bool isHoldingUnknown(const Type *type) const;
  static bool isObjectReference(const Type *type);
  ResultMemberChecked checkUnknownMember(MemberExpr *member);
  std::string unknownHint(const Expr *expr, const Type *to) const;
  ResultMethodChecked checkUnknownMethod(CallExpr *call, MemberExpr *member);
  ResultValueNarrowed unknownUse(const Expr *object, llvm::StringRef what) const;
  void addUnknownFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

private:
  using ResultMemberAllowed = Result<void, UnknownCheckerCheckUnknownMemberError>;
  using ResultMethodAllowed = Result<void, UnknownCheckerCheckUnknownMethodError>;

  bool isNamedClass(const Type *to) const;
  std::string hintName(const Expr *expr) const;
  const Type *instanceType(const Type *classValue) const;
  ResultMemberChecked checkClassMember(MemberExpr *member);
  const VarSymbol *checkedVariable(const Expr *expr) const;
  ResultMethodAllowed checkNotClassValue(const Type *receiver) const;
  ResultMethodAllowed checkNotRuntimeClass(const Type *receiver) const;
  void narrowByTypeOf(const BinaryExpr *binary, Flow &equal) const;
  ResultMemberAllowed checkNotConstructor(const MemberExpr *member) const;
  void narrowByInstanceOf(const BinaryExpr *binary, Flow &whenTrue) const;
  std::string typeOfCheck(llvm::StringRef shown, const Type *target) const;
  void narrowChecked(const Expr *checked, const Type *type, Flow &flow) const;
  void narrowTypeOfSide(const Expr *side, const Expr *other, Flow &equal) const;
  Expr *checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl);
  void replaceNarrowing(Flow &flow, const VarSymbol *variable, const Type *type) const;
  std::string narrowingCheck(llvm::StringRef shown, const Expr *expr, const Type *to) const;

  SemanticState &state;
  StaticChecker *staticChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
};

}
