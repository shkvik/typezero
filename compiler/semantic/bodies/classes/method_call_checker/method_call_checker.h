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

  ResultFieldIsCallable checkFieldCallable(const Type *type, llvm::StringRef name) const;
  ResultMethodsAreFound checkInstanceMethods(const ClassDecl *classDecl,
                                             llvm::StringRef name) const;
  ResultFieldsAreAllSet checkFieldsSet(const MemberExpr *member) const;
  ResultTypeArgsAllowed checkMethodTypeArgs(const FuncSymbol *function,
                                            const MemberExpr *member) const;
  Expr *checkLibraryMethodCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl);
  Expr *checkFieldCall(CallExpr *call, MemberExpr *member);
  Expr *checkFieldValueCall(CallExpr *call, const Type *type);
  void walkMethodAccess(MemberExpr *member, FunctionDecl *method);
  Expr *checkGenericMethodCall(CallExpr *call, MemberExpr *member, FuncSymbol *function,
                               const Type *expected);
  Expr *checkPlainMethodCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                             FuncSymbol *function, bool isChecked);
  MethodCall methodTarget(ClassDecl *classDecl, FuncSymbol *function) const;
  ResultClassNotRuntime checkNotRuntime(const ClassDecl *classDecl) const;
  ResultNoTypeArguments checkClassTypeArgs(const NewExpr *expr, const ClassDecl *classDecl) const;
  ResultClassIsDeclared checkClassDeclared(const ClassDecl *classDecl) const;
  ResultClassIsConcrete checkClassConcrete(const ClassDecl *classDecl) const;
  ResultArgumentsAbsent checkArgumentsAbsent(const CallExpr *call,
                                             const ClassDecl *classDecl) const;
  Expr *failClassNew(NewExpr *expr);
  void walkNewClass(const NewExpr *expr, const ClassDecl *classDecl);
  bool reportUnexpectedArgs(const NewExpr *expr, CallExpr *call, const ClassDecl *classDecl);
  bool chooseConstructor(const NewExpr *expr, CallExpr *call,
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
