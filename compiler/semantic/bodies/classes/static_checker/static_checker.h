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

  struct OwnedStaticField {
    ClassField *field;
    ClassDecl *owner;
  };

  OwnedStaticField staticField(ClassDecl *classDecl, llvm::StringRef name) const;
  ClassField *ownStaticField(ClassDecl *owner, llvm::StringRef name) const;
  ResultStaticFieldType checkFieldTyped(const ClassDecl *owner, const VarSymbol *variable,
                                        llvm::StringRef name) const;
  ResultFieldValueIsSet checkFieldReady(const ClassDecl *owner, const VarSymbol *variable) const;
  ResultStaticFunctions checkStaticMethods(const ClassDecl *classDecl, llvm::StringRef name) const;
  Expr *checkStaticFieldRead(MemberExpr *member, ClassDecl *classDecl, const ClassField &field,
                             const ClassDecl *owner);
  Expr *checkStaticMethodValue(MemberExpr *member, ClassDecl *classDecl, const Type *expected);
  Expr *checkClassValueMember(MemberExpr *member, ClassDecl *classDecl);
  ResultFieldOfTheClass checkFieldFound(const ClassField *found, const ClassDecl *classDecl,
                                        llvm::StringRef name) const;
  ResultFieldIsWritable checkFieldWritable(const VarSymbol *variable) const;
  ResultFieldTypeStated checkTypeDeclared(const VarSymbol *variable, llvm::StringRef name) const;
  ResultFieldIsCallable checkStaticCallable(const Type *type, const ClassDecl *classDecl,
                                            llvm::StringRef name) const;
  ResultTypeArgsAllowed checkMethodTypeArgs(const FuncSymbol *function,
                                            const MemberExpr *member) const;
  Expr *checkLibraryStaticCall(CallExpr *call, MemberExpr *member, const ClassDecl *classDecl,
                               const Type *expected);
  Expr *checkEventsStaticCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkStaticFieldCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl);
  Expr *checkStaticValueCall(CallExpr *call, const Type *type);
  Expr *checkStaticMethodCall(CallExpr *call, MemberExpr *member,
                              const std::vector<FunctionDecl *> &methods, const Type *expected);

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
