#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

const Type *spreadTuple(llvm::ArrayRef<const Type *> parts, const std::vector<bool> &spread,
                        std::string &why);

bool flattenArguments(llvm::ArrayRef<const Type *> params, size_t &minArgs, bool &hasRest,
                      std::vector<const Type *> &flat);

}
