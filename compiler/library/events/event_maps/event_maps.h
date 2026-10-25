#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Event maps: the events of an emitter and their listeners.
class EventMaps {
public:
  explicit EventMaps(Sema &sema);

  // An emitter: an object of EventEmitter of node:events or of a class
  // that extends it.
  bool isEmitterClass(const ClassDecl *cls) const;
  // The event map of an emitter of `type`: the type argument of the
  // EventEmitter up its chain; null for any other type.
  const Type *eventMapOf(const Type *type) const;
  // Reports `events`, the type argument of EventEmitter, that is no event
  // map: an object type of tuples, or a Record of them.
  bool checkEventMap(const Type *events, llvm::SMLoc loc);
  // The arguments the listeners of the event `name` get, a tuple; `name`,
  // an argument of `method`, is checked: a constant, unless the map takes
  // any name. `label` gets the name for messages. Null after an error.
  const Type *eventArgs(const Type *events, Expr *&name, llvm::StringRef method,
                        std::string *label = nullptr);
  // The function a listener of events with these arguments is, returning
  // `ret`.
  static const Type *listenerType(const Type *args, const Type *ret);
  Expr *checkListener(Expr *arg, const Type *args, llvm::StringRef event, const Type *receiver,
                      Sema::ListenerKind &kind);
  // The type of the object `create` makes, before it is checked: an
  // instance of a class (of EventEmitter with the map found for its place);
  // null if that is not known from its form.
  const Type *createdType(const NewExpr *create);

private:
  Sema &sema_;
};

} // namespace tz
