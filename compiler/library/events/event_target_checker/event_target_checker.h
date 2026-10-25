#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// EventTarget, AbortSignal and the functions of node:events.
class EventTargetChecker {
public:
  explicit EventTargetChecker(Checker &checker);

  // `signal.onabort = handler` of the program, with both checked: a call of
  // setOnabort (library/events/web.tz), as node's event handlers listen from
  // when they are first set; `aborted` and `reason`, an error; null for any
  // other assignment. `logical`: the assignment of `o.x ??= v`, `||=` or `&&=`.
  Expr *onabortAssign(AssignExpr *assign, MemberExpr *member, bool logical);
  // addEventListener() and removeEventListener() of an EventTarget of the
  // program: the kind of event the listener takes, from its annotation; null
  // for the other methods.
  Expr *checkEventTargetCall(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  // once(emitter, name), on(), getEventListeners() and listenerCount() of
  // node:events, also as `EventEmitter.once(...)`: `fn` is the function's
  // name there. Null for the other functions of the module.
  Expr *checkEventsFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  // A call of `func` when it is a function of node:events (once and the like).
  Expr *checkLibraryCall(CallExpr *call, NameExpr *name, FuncSymbol *func);
  // `call` of the function `name` of the emitter's code, generic with
  // `bound`, whose arguments `given` marks as checked.
  Expr *callLibraryFunction(CallExpr *call, llvm::StringRef name,
                            llvm::ArrayRef<const Type *> bound, const std::vector<bool> &given,
                            llvm::SMLoc loc);

private:
  Checker &checker_;
};

} // namespace tz
