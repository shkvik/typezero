#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

std::string objectTypeSpelling(llvm::ArrayRef<Field> fields);

const char *typeOfName(const Type *type);

inline constexpr const char *kTypeOfNames[] = {
    "undefined", "null",   "boolean", "int",     "int8",   "int16",  "int32",
    "uint8",     "uint16", "uint32",  "uint64",  "double", "decimal", "string",
    "array",     "object", "function", "class"};

bool isTypeOfName(llvm::StringRef name);

std::vector<llvm::StringRef> typeOfNames(const Type *type);

std::string jsonTagField(llvm::ArrayRef<const Type *> objects);

}
