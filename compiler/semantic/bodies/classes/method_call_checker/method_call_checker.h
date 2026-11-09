#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class EventMaps;
class FsDeclarer;
class CallChecker;
class ExprChecker;
class FlowChecker;
class ClassChecker;
class ClassDeclarer;
class StreamChecker;
class EmitterChecker;
class ReturnInferrer;
class ArgumentChecker;
class TypeArgInferrer;
class OverloadResolver;
class TzModulesChecker;
class ClassMemberChecker;
class EventTargetChecker;
class GenericClassChecker;

class MethodCallChecker {
public:
  MethodCallChecker(SemanticState &owner, SemanticParts &all);

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

  SemanticState &state;
  EventMaps *eventMaps = nullptr;
  FsDeclarer *fsDeclarer = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  ClassChecker *classChecker = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  StreamChecker *streamChecker = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  OverloadResolver *overloadResolver = nullptr;
  TzModulesChecker *tzModulesChecker = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
