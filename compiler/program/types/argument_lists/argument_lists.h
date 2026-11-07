#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

class ArgumentLists {
public:
  static bool flattenArguments(llvm::ArrayRef<const Type *> params, size_t &minArgs, bool &hasRest,
                               std::vector<const Type *> &flat);
  static const Type *spreadTuple(llvm::ArrayRef<const Type *> parts,
                                 const std::vector<bool> &spread, std::string &why);
};

}
