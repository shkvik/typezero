#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/events/event_maps/event_maps_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <optional>
#include <string>

namespace tz {

class SemanticParts;
class ExprChecker;
class TypeDeclarer;
class TypeResolver;
class TypeRelations;
class ClassDeclarer;
class ClosureChecker;
class ArgumentChecker;
class EventMapInference;
class KeyedFieldChecker;
class FunctionValueChecker;
class GenericClassInstantiator;

class EventMaps {
public:
  using ResultEventMapFits = Result<const Type *, EventMapsEventMapFitsError>;

  EventMaps(SemanticState &owner, SemanticParts &all);

  bool isEmitterClass(const ClassDecl *classDecl) const;
  const Type *eventMapOf(const Type *type) const;
  ResultEventMapFits eventMapFits(const Type *events) const;
  bool isServiceEvent(llvm::StringRef name) const;
  const Type *serviceEventArgs() const;
  std::optional<std::string> eventKey(Expr *&name, llvm::StringRef method);
  const Type *eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                        std::string *label = nullptr);
  const Type *findEventArgs(const Type *events, llvm::StringRef name);
  const Type *listenerType(const Type *args, const Type *returnType) const;
  Expr *checkListener(Expr *arg, const Type *args, llvm::StringRef event, const Type *receiver,
                      ListenerKind &kind);
  const Type *createdType(const NewExpr *create);

private:
  using ResultListenerThis = Result<const Type *, EventMapsListenerThisError>;

  ResultListenerThis listenerThis(const Type *self, const Type *receiver) const;
  bool carriesOne(const Type *args) const;
  ResultEventMapFits recordMapFits(const Type *events) const;
  ResultEventMapFits eventFieldFits(const Field &field) const;
  std::string nearestEvent(const Type *events, llvm::StringRef name) const;

  struct ListenerWant {
    const Type *args = nullptr;
    llvm::StringRef event;
    const Type *receiver = nullptr;
    const Type *sync = nullptr;
    const Type *async = nullptr;
  };

  bool listenerTakesThis(const ListenerWant &want, const Type *self, llvm::SMLoc loc);
  const Type *withThis(const ListenerWant &want, const Type *self) const;
  Expr *checkListenerLiteral(FunctionExpr *functionExpr, const ListenerWant &want,
                             ListenerKind &kind);
  Expr *checkThisListener(FunctionExpr *functionExpr, const ListenerWant &want, ListenerKind &kind);
  Expr *checkListenerValue(Expr *arg, const ListenerWant &want, ListenerKind &kind);
  Expr *checkThisValue(Expr *arg, const ListenerWant &want, ListenerKind &kind);

  const Type *inferredEventArgs(EventSite *site, const std::string &key, llvm::SMLoc loc);

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ClosureChecker *closureChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  EventMapInference *eventMapInference = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
