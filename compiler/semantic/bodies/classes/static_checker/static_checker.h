#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/static_checker/static_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class CallChecker;
class StreamSources;
class TypeRelations;
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

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  StreamSources *streamSources = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  OverloadResolver *overloadResolver = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
};

}
