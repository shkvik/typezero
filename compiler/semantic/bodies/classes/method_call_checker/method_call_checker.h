#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class ClassLinks;
class ClassBases;
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

  Expr *checkClassNew(NewExpr *expr, ClassDecl *classDecl, const Type *expected = nullptr);
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

  ResultClassNotRuntime notRuntime(const ClassDecl *classDecl) const;
  ResultClassIsDeclared classDeclared(const ClassDecl *classDecl) const;
  ResultFieldsAreAllSet fieldsSet(const MemberExpr *member) const;
  ResultClassIsConcrete classConcrete(const ClassDecl *classDecl) const;
  ResultFieldIsCallable fieldCallable(const Type *type, llvm::StringRef name) const;
  ResultNoTypeArguments classTypeArgs(const NewExpr *expr, const ClassDecl *classDecl) const;
  ResultArgumentsAbsent argumentsAbsent(const CallExpr *call, const ClassDecl *classDecl) const;
  ResultMethodsAreFound instanceMethods(const ClassDecl *classDecl, llvm::StringRef name) const;
  ResultTypeArgsAllowed methodTypeArgs(const FuncSymbol *function, const MemberExpr *member) const;
  Expr *checkLibraryMethodCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl);
  Expr *checkFieldCall(CallExpr *call, MemberExpr *member);
  void walkMethodAccess(MemberExpr *member, FunctionDecl *method);
  Expr *checkGenericMethodCall(CallExpr *call, MemberExpr *member, FuncSymbol *function,
                               const Type *expected);
  Expr *checkPlainMethodCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                             FuncSymbol *function, bool checked);
  void walkNewClass(const NewExpr *expr, const ClassDecl *classDecl);
  bool noArgumentsFit(const NewExpr *expr, CallExpr *call, const ClassDecl *classDecl);
  bool constructorChosen(const NewExpr *expr, CallExpr *call,
                         llvm::ArrayRef<FunctionDecl *> constructors);
  bool hasInitializer(const ClassDecl *classDecl) const;

  SemanticState &state;
  EventMaps *eventMaps = nullptr;
  FsDeclarer *fsDeclarer = nullptr;
  ClassLinks *classLinks = nullptr;
  ClassBases *classBases = nullptr;
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
