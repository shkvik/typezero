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

  bool isEmitterClass(const ClassDecl *cls) const;
  const Type *eventMapOf(const Type *type) const;
  static ResultEventMapFits eventMapFits(const Type *events);
  static bool isServiceEvent(llvm::StringRef name);
  static const Type *serviceEventArgs();
  std::optional<std::string> eventKey(Expr *&name, llvm::StringRef method);
  const Type *eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                        std::string *label = nullptr);
  const Type *findEventArgs(const Type *events, llvm::StringRef name);
  static const Type *listenerType(const Type *args, const Type *ret);
  Expr *checkListener(Expr *arg, const Type *args, llvm::StringRef event, const Type *receiver,
                      ListenerKind &kind);
  const Type *createdType(const NewExpr *create);

private:
  using ResultListenerThis = Result<const Type *, EventMapsListenerThisError>;

  static ResultListenerThis listenerThis(const Type *self, const Type *receiver);
  static bool carriesOne(const Type *args);
  static std::string nearestEvent(const Type *events, llvm::StringRef name);

  const Type *inferredEventArgs(EventSite *site, const std::string &key, llvm::SMLoc loc);

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClosureChecker *closureChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  EventMapInference *eventMapInference = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
