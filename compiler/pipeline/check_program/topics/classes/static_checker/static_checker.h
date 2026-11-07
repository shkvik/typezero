#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/classes/static_checker/static_checker_errors.h"
#include "compiler/result/result/result.h"
#include <vector>

namespace tz {

class StaticChecker {
public:
  explicit StaticChecker(CheckState &owner);

  const Type *checkStaticTarget(Expr *&target, ClassDecl *cls);
  Expr *checkStaticMember(MemberExpr *member, ClassDecl *cls, const Type *expected);
  Expr *checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                             const Type *expected = nullptr);

private:
  using ResultStaticFieldType = Result<const Type *, StaticCheckerFieldTypedError>;
  using ResultFieldTypeStated = Result<const Type *, StaticCheckerTypeDeclaredError>;
  using ResultFieldIsCallable = Result<const Type *, StaticCheckerStaticCallableError>;
  using ResultFieldValueIsSet = Result<const VarSymbol *, StaticCheckerFieldReadyError>;
  using ResultFieldOfTheClass = Result<const ClassField *, StaticCheckerFieldFoundError>;
  using ResultFieldIsWritable = Result<const VarSymbol *, StaticCheckerFieldWritableError>;
  using ResultTypeArgsAllowed = Result<const FuncSymbol *, StaticCheckerMethodTypeArgsError>;
  using ResultStaticFunctions =
      Result<std::vector<FunctionDecl *>, StaticCheckerStaticMethodsError>;

  static ResultFieldIsWritable fieldWritable(const VarSymbol *var);
  ResultFieldValueIsSet fieldReady(const ClassDecl *owner, const VarSymbol *var) const;
  static ResultFieldTypeStated typeDeclared(const VarSymbol *var, llvm::StringRef name);
  static ResultStaticFunctions staticMethods(const ClassDecl *cls, llvm::StringRef name);
  static ClassField *staticField(ClassDecl *cls, llvm::StringRef name, ClassDecl **owner);
  static ResultTypeArgsAllowed methodTypeArgs(const FuncSymbol *sym, const MemberExpr *member);
  static ResultFieldIsCallable staticCallable(const Type *type, const ClassDecl *cls,
                                              llvm::StringRef name);
  ResultStaticFieldType fieldTyped(const ClassDecl *owner, const VarSymbol *var,
                                   llvm::StringRef name) const;
  static ResultFieldOfTheClass fieldFound(const ClassField *found, const ClassDecl *cls,
                                          llvm::StringRef name);

  CheckState &state;
};

}
