#pragma once

#include "compiler/parallel/analysis/analysis.h"

namespace tz::parallel {

const Expr *strip(const Expr *e);

bool holdsReferences(const Type *type);

bool makesNewObject(Builtin builtin);

bool knownBuiltin(Builtin builtin);

llvm::StringRef libraryClassName(const ClassDecl *cls);

bool knownBuiltin(Builtin b);

bool onlyReadsArgs(Builtin b);

}
