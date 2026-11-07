#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

std::string moduleLabel(NodeModule module);

bool targetIsWindows();

Expr *stringLiteral(AstContext &ctx, llvm::SMLoc loc, llvm::StringRef text);

}
