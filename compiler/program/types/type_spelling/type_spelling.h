#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

std::string objectTypeSpelling(llvm::ArrayRef<Field> fields);

const char *typeOfName(const Type *type);

std::string jsonTagField(llvm::ArrayRef<const Type *> objects);

}
