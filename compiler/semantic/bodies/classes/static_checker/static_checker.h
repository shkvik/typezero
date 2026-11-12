#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/static_checker/static_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class ClassBases;
class SemanticParts;
class CallChecker;
class StreamSources;
class TypeRelations;
class ClassDeclarer;
class ReturnInferrer;
class ArgumentChecker;
class TypeArgInferrer;
class OverloadResolver;
class ClassValueChecker;
class ClassMemberChecker;
class EventTargetChecker;

class StaticChecker {
public:
  StaticChecker(SemanticState &owner, SemanticParts &all);

  const Type *checkStaticTarget(Expr *&target, ClassDecl *classDecl);
  Expr *checkStaticMember(MemberExpr *member, ClassDecl *classDecl, const Type *expected);
  Expr *checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
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

  Expr *checkStaticFieldRead(MemberExpr *member, ClassDecl *classDecl, const ClassField &field,
                             const ClassDecl *owner);
  Expr *checkStaticMethodValue(MemberExpr *member, ClassDecl *classDecl, const Type *expected);
  Expr *checkClassValueMember(MemberExpr *member, ClassDecl *classDecl);
  Expr *checkLibraryStaticCall(CallExpr *call, MemberExpr *member, const ClassDecl *classDecl,
                               const Type *expected);
  Expr *checkStaticFieldCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl);
  Expr *checkStaticMethodCall(CallExpr *call, MemberExpr *member,
                              const std::vector<FunctionDecl *> &methods, const Type *expected);
  ResultFieldIsWritable fieldWritable(const VarSymbol *variable) const;
  ResultFieldValueIsSet fieldReady(const ClassDecl *owner, const VarSymbol *variable) const;
  ResultFieldTypeStated typeDeclared(const VarSymbol *variable, llvm::StringRef name) const;
  ResultStaticFunctions staticMethods(const ClassDecl *classDecl, llvm::StringRef name) const;
  ClassField *staticField(ClassDecl *classDecl, llvm::StringRef name, ClassDecl **owner) const;
  ResultTypeArgsAllowed methodTypeArgs(const FuncSymbol *function, const MemberExpr *member) const;
  ResultFieldIsCallable staticCallable(const Type *type, const ClassDecl *classDecl,
                                       llvm::StringRef name) const;
  ResultStaticFieldType fieldTyped(const ClassDecl *owner, const VarSymbol *variable,
                                   llvm::StringRef name) const;
  ResultFieldOfTheClass fieldFound(const ClassField *found, const ClassDecl *classDecl,
                                   llvm::StringRef name) const;

  SemanticState &state;
  ClassBases *classBases = nullptr;
  CallChecker *callChecker = nullptr;
  StreamSources *streamSources = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  OverloadResolver *overloadResolver = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
};

}
