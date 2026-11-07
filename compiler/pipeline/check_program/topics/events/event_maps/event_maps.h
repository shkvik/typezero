#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/events/event_maps/event_maps_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class EventMaps {
public:
  using ResultEventMapFits = Result<const Type *, EventMapsEventMapFitsError>;

  explicit EventMaps(CheckState &owner);

  bool isEmitterClass(const ClassDecl *cls) const;
  const Type *eventMapOf(const Type *type) const;
  static ResultEventMapFits eventMapFits(const Type *events);
  const Type *eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                        std::string *label = nullptr);
  static const Type *listenerType(const Type *args, const Type *ret);
  Expr *checkListener(Expr *arg, const Type *args, llvm::StringRef event, const Type *receiver,
                      ListenerKind &kind);
  const Type *createdType(const NewExpr *create);

private:
  using ResultListenerThis = Result<const Type *, EventMapsListenerThisError>;

  static ResultListenerThis listenerThis(const Type *self, const Type *receiver);
  static bool carriesOne(const Type *args);
  static bool isServiceEvent(llvm::StringRef name);
  static const Type *serviceEventArgs();
  static std::string nearestEvent(const Type *events, llvm::StringRef name);

  CheckState &state;
};

}
