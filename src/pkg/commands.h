#pragma once

#include "llvm/ADT/ArrayRef.h"

namespace tz::pkg {

// The commands of tz that work on a project rather than compile it. Each
// takes the arguments after the command's name and returns tz's exit code.

// tz init [directory]: tz.json, main.tz and .gitignore.
int commandInit(llvm::ArrayRef<const char *> args);
// tz add <package>...: adds packages to tz.json and tz.lock.
int commandAdd(llvm::ArrayRef<const char *> args);
// tz remove <name>...
int commandRemove(llvm::ArrayRef<const char *> args);
// tz update [name...]: the newest versions tz.json allows.
int commandUpdate(llvm::ArrayRef<const char *> args);
// tz port <package> [-o directory] [--in-place] (see port.cpp).
int commandPort(llvm::ArrayRef<const char *> args);

} // namespace tz::pkg
