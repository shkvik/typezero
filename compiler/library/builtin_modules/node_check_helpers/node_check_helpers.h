#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

std::string moduleLabel(NodeModule module);

bool targetIsWindows();

Expr *stringLiteral(AstContext &ctx, llvm::SMLoc loc, llvm::StringRef text);

}
