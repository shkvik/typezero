#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

const Expr *strip(const Expr *e);

// Whether values of `type` hold references to objects whose contents may
// change.
bool holdsReferences(const Type *type);

// The built-ins whose result is a new object, which no other code has yet.
bool makesNewObject(Builtin builtin);

// Whether the pass knows what the built-in does (see Analysis::builtinEffects);
// one it does not know may change whatever it is given.
bool knownBuiltin(Builtin builtin);

// The name of a class of a built-in module; of a generic one, for its copies
// too.
llvm::StringRef libraryClassName(const ClassDecl *cls);

bool knownBuiltin(Builtin b);

} // namespace tz::parallel
