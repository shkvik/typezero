#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// EventEmitter: new, on, emit and the other methods (docs/events.md).
class EmitterChecker {
public:
  explicit EmitterChecker(Sema &sema);

  // A call of the program of a method of node's EventEmitter on an emitter
  // of class `cls` (with `super`, `super.m()` in a class down its chain):
  // checked against the event map, as a call of the emitter's code. Null if
  // the method is none of those.
  Expr *checkEmitterCall(CallExpr *call, MemberExpr *member, ClassDecl *cls, bool super);
  // `call` of the method `name` of the emitter's code (generic with
  // `bound`), whose arguments `given` marks as checked.
  Expr *callLibraryMethod(CallExpr *call, MemberExpr *member, ClassDecl *cls, llvm::StringRef name,
                          llvm::ArrayRef<const Type *> bound, const std::vector<bool> *given,
                          bool super);
  void reportEmitterMethodValue(MemberExpr *member);
  // "; ...": why a name of node:events that the module does not export is
  // not there, for the error of its import.
  static std::string libraryExportHint(llvm::StringRef name);
  // `new EventEmitter()` without a map: the one the place it goes gives, or
  // the one found from emits (sema_generic_class.cpp calls it).
  Expr *checkEmitterNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  // What a round looking for maps does with a call on an emitter of the
  // opaque map `events` of `site`: records the arguments of an emit, gives
  // a listener of a known event its type.
  Expr *collectEmitterCall(CallExpr *call, MemberExpr *member, const std::string &site);

private:
  Sema &sema_;
};

} // namespace tz
