#pragma once

#include "compiler/program/library/events/events_function_table/events_function_table.h"
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

  struct WaitCall {
    CallExpr *call = nullptr;
    llvm::StringRef fn;
    llvm::SMLoc loc;
    bool once = false;
    bool timed = false;
    std::vector<bool> given;
  };

  static bool hasTimeoutOption(const CallExpr *call);
  bool waitEnds(const CallExpr *call) const;
  bool isEventTarget(const Type *type) const;
  void walkArgsFrom(CallExpr *call, size_t from);
  Expr *checkTargetWait(WaitCall &wait);
  void appendErrorsFlag(WaitCall &wait, bool listensErrors);
  Expr *targetListeners(CallExpr *call, Expr *object);
  Expr *checkEmitterWait(WaitCall &wait, const Type *events);
  Expr *checkWaitFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc, bool once);
  Expr *checkListenersFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc, bool listeners);
  Expr *checkTimedWait(WaitCall &wait, const Type *events, const Type *args, const Type *errors);
  Expr *checkPlainWait(WaitCall &wait, const Type *events, const Type *args, const Type *errors);
  const Type *listenedEvent(const Type *type, llvm::SMLoc loc, ClassDecl *eventClass);
  std::optional<const Type *> typedListener(CallExpr *call, ClassDecl *eventClass,
                                            const Type *object);
  std::optional<const Type *> typedValueListener(CallExpr *call, ClassDecl *eventClass,
                                                 const Type *object);
  std::optional<const Type *> typedFunctionListener(CallExpr *call, FunctionExpr *function,
                                                    ClassDecl *eventClass);

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
