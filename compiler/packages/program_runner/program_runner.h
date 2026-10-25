#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

// Runs a program; its stdout and stderr are collected. Returns its exit code,
// or -1 with `error` set if it could not run.
int runProgram(llvm::StringRef program, llvm::ArrayRef<std::string> args, std::string &out,
               std::string &err, std::string &error);

} // namespace tz::pkg
