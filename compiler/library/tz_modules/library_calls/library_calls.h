#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The built-in modules written in tz: where they are, and their calls of the compiler.
class LibraryCalls {
public:
  explicit LibraryCalls(Sema &sema);

  // Events (sema_events.cpp, docs/events.md): node:events and the events of
  // the web platform, which tz has written in tz (library/), and what the
  // compiler adds to that code.
  //
  // Marks the classes of the built-in modules written in tz
  // (Module::builtin) and their fields `@__tz_hidden`; notes where their
  // code is, which alone may call the intrinsics `__tz_*`.
  void prepareLibraries(SourceFile &program);
  bool inLibrary(llvm::SMLoc loc) const;
  // `__tz_listener<F>(u)` and the other intrinsics (Builtin::LibListener...);
  // null if `name` names none of them.
  Expr *checkLibCall(CallExpr *call, NameExpr *name);

private:
  Sema &sema_;
  std::vector<std::pair<const char *, const char *>> libraryRanges_;
};

} // namespace tz
