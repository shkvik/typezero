#pragma once

#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ReflectCheckerConstructibleErrorCode {
  BUILTIN_CLASS,
  ABSTRACT,
  NO_ARGUMENTS,
  UNKNOWN_CONSTRUCTOR,
  NO_FIT,
  SEVERAL_FIT,
};

struct ReflectCheckerConstructibleError {
  ReflectCheckerConstructibleErrorCode code;
  std::string message;
};

enum class ReflectCheckerFillableClassErrorCode {
  JAVASCRIPT_CLASS,
  ABSTRACT,
  HAS_CONSTRUCTOR,
};

struct ReflectCheckerFillableClassError {
  ReflectCheckerFillableClassErrorCode code;
  std::string message;
};

class ErrorTextConstructible {
public:
  static std::string builtin();
  static std::string abstract();
  static std::string noArgs();
  static std::string unknown();
  static std::string takes(llvm::ArrayRef<const FuncSymbol *> constructors);
  static std::string several();
};

class ErrorTextReflectNew {
public:
  static std::string mayFail(llvm::StringRef type, llvm::StringRef why);
  static std::string third();
  static std::string notClass(llvm::StringRef type);
  static std::string javascript(llvm::StringRef name);
};

class ErrorTextReflectGet {
public:
  static std::string json();
  static std::string notObject(llvm::StringRef type);
};

class ErrorTextReflectFill {
public:
  static std::string notClass(llvm::StringRef type);
  static std::string cannot(llvm::StringRef name, llvm::StringRef why);
  static std::string javascript();
  static std::string abstract();
  static std::string ownCtor(llvm::StringRef name);
};

class ErrorTextReflectApply {
public:
  static std::string notFunc(llvm::StringRef type);
  static std::string voidThis();
};

}
