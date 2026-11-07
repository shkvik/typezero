#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/classes/method_call_checker/method_call_checker_errors.h"
#include "compiler/result/result/result.h"
#include <vector>

namespace tz {

class MethodCallChecker {
public:
  explicit MethodCallChecker(CheckState &owner);

  Expr *checkClassNew(NewExpr *expr, ClassDecl *cls, const Type *expected = nullptr);
  Expr *checkMethodCall(CallExpr *call, MemberExpr *member, const Type *expected = nullptr);

private:
  using ResultFieldIsCallable = Result<const Type *, MethodCallCheckerFieldCallableError>;
  using ResultClassNotRuntime = Result<const ClassDecl *, MethodCallCheckerNotRuntimeError>;
  using ResultFieldsAreAllSet = Result<const MemberExpr *, MethodCallCheckerFieldsSetError>;
  using ResultNoTypeArguments = Result<const NewExpr *, MethodCallCheckerClassTypeArgsError>;
  using ResultClassIsConcrete = Result<const ClassDecl *, MethodCallCheckerClassConcreteError>;
  using ResultClassIsDeclared = Result<const ClassDecl *, MethodCallCheckerClassDeclaredError>;
  using ResultArgumentsAbsent = Result<const CallExpr *, MethodCallCheckerArgumentsAbsentError>;
  using ResultTypeArgsAllowed = Result<const FuncSymbol *, MethodCallCheckerMethodTypeArgsError>;
  using ResultMethodsAreFound =
      Result<std::vector<FunctionDecl *>, MethodCallCheckerInstanceMethodsError>;

  ResultClassNotRuntime notRuntime(const ClassDecl *cls) const;
  ResultClassIsDeclared classDeclared(const ClassDecl *cls) const;
  ResultFieldsAreAllSet fieldsSet(const MemberExpr *member) const;
  static ResultClassIsConcrete classConcrete(const ClassDecl *cls);
  static ResultFieldIsCallable fieldCallable(const Type *type, llvm::StringRef name);
  static ResultNoTypeArguments classTypeArgs(const NewExpr *expr, const ClassDecl *cls);
  static ResultArgumentsAbsent argumentsAbsent(const CallExpr *call, const ClassDecl *cls);
  static ResultMethodsAreFound instanceMethods(const ClassDecl *cls, llvm::StringRef name);
  static ResultTypeArgsAllowed methodTypeArgs(const FuncSymbol *sym, const MemberExpr *member);

  CheckState &state;
};

}
