#pragma once

#include "compiler/program/symbols/var_symbol/var_symbol.h"
#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>
#include <vector>

namespace tz {

class ErrorTextListenerLife {
public:
  static std::string what(bool timer);
  static std::string ends(bool timer);
  static std::string section(bool timer);
  static std::string list(const std::vector<const VarSymbol *> &vars);
  static std::string unknown(bool timer);
  static std::string keeps(bool timer, llvm::StringRef name);
  static std::string captures(bool timer, const std::vector<const VarSymbol *> &captured,
                              const std::vector<const VarSymbol *> &others, llvm::StringRef kept);
};

class ErrorTextUnlistened {
public:
  static std::string mayEmit(llvm::StringRef path);
  static std::string unheard();
};

class ErrorTextEmitterNew {
public:
  static std::string dropped(llvm::StringRef cls);
  static std::string needsMap();
};

class ErrorTextEmitted {
public:
  static std::string oneValue(llvm::StringRef event);
  static std::string noName();
  static std::string typeArgs(llvm::StringRef name);
  static std::string countAt(llvm::StringRef event, llvm::StringRef args, llvm::StringRef first,
                             size_t count);
  static std::string arity(llvm::StringRef event, llvm::StringRef args, size_t min, size_t max,
                           size_t count);
  static std::string typeAt(llvm::StringRef event, llvm::StringRef args, llvm::StringRef first,
                            llvm::StringRef type);
  static std::string methodVal(llvm::StringRef name);
};

class ErrorTextEventsExport {
public:
  static std::string hint(llvm::StringRef name);
};

}
