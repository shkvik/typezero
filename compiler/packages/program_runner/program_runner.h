#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

int runProgram(llvm::StringRef program, llvm::ArrayRef<std::string> args, std::string &out,
               std::string &err, std::string &error);

}
