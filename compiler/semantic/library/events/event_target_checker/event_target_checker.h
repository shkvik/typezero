#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/events/event_target_checker/event_target_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class EventMaps;
class TypeParams;
class CallChecker;
class ExprChecker;
class LibraryCalls;
class TypeDeclarer;
class TypeResolver;
class TypeRelations;
class ClosureChecker;
class EmitterChecker;
class ArgumentChecker;
class GenericInstantiator;

class EventTargetChecker {
public:
  EventTargetChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkEventTargetCall(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  Expr *checkEventsFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func);
  Expr *callLibraryFunction(CallExpr *call, llvm::StringRef name,
                            llvm::ArrayRef<const Type *> bound, const std::vector<bool> &given,
                            llvm::SMLoc loc);
  Expr *callLibraryStatic(CallExpr *call, ClassDecl *cls, llvm::StringRef name,
                          llvm::ArrayRef<const Type *> bound, const std::vector<bool> &given,
                          llvm::SMLoc loc);

private:
  using ResultListenerEvent = Result<const Type *, EventTargetCheckerListenerEventError>;
  using ResultEmitterEvents = Result<const Type *, EventTargetCheckerEmitterEventsError>;
  using ResultOneErrorValue = Result<const Type *, EventTargetCheckerOneErrorValueError>;

  static ResultOneErrorValue oneErrorValue(const Type *errors, llvm::StringRef fn);
  ResultEmitterEvents emitterEvents(const Type *type, llvm::StringRef fn, bool target);
  static ResultListenerEvent listenerEvent(const Type *type, const ClassDecl *eventClass);
  Expr *callLibrarySymbol(CallExpr *call, FuncSymbol *sym, llvm::ArrayRef<const Type *> bound,
                          const std::vector<bool> &given, llvm::SMLoc loc);
  SemanticState &state;
  EventMaps *eventMaps = nullptr;
  TypeParams *typeParams = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClosureChecker *closureChecker = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  GenericInstantiator *genericInstantiator = nullptr;
};

}
