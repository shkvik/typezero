#pragma once

#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryReflectRegistryConstructibleErrorCode {
  BUILTIN_CLASS,
  ABSTRACT,
  NO_ARGUMENTS,
  UNKNOWN_CONSTRUCTOR,
  NO_FIT,
  SEVERAL_FIT,
};

struct LibraryReflectRegistryConstructibleError {
  LibraryReflectRegistryConstructibleErrorCode code;
  std::string message;
};

enum class LibraryReflectRegistryFillableClassErrorCode {
  JAVASCRIPT_CLASS,
  ABSTRACT,
  HAS_CONSTRUCTOR,
};

struct LibraryReflectRegistryFillableClassError {
  LibraryReflectRegistryFillableClassErrorCode code;
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

enum class LibraryReflectRegistryKeyOfRecordsErrorCode {
  NOT_A_KEY,
};

struct LibraryReflectRegistryKeyOfRecordsError {
  LibraryReflectRegistryKeyOfRecordsErrorCode code;
  std::string message;
};

enum class LibraryReflectRegistryTargetObjectErrorCode {
  NOT_AN_OBJECT,
};

struct LibraryReflectRegistryTargetObjectError {
  LibraryReflectRegistryTargetObjectErrorCode code;
  std::string message;
};

enum class LibraryReflectRegistryArgumentListErrorCode {
  TUPLE,
  NOT_AN_ARRAY,
};

struct LibraryReflectRegistryArgumentListError {
  LibraryReflectRegistryArgumentListErrorCode code;
  std::string message;
};

class ErrorTextReflectCall {
public:
  static std::string decorator();
  static std::string keys(llvm::StringRef fn);
  static std::string unknown(llvm::StringRef fn);
};

class ErrorTextMetadataKey {
public:
  static std::string type(llvm::StringRef type);
  static std::string design();
};

class ErrorTextMetadataValue {
public:
  static std::string voidValue();
  static std::string label(llvm::StringRef label);
  static std::string since(llvm::StringRef label, llvm::StringRef type);
  static std::string undefined(llvm::StringRef label);
};

class ErrorTextMetadataTarget {
public:
  static std::string notObject(llvm::StringRef type);
};

class ErrorTextReflectArgs {
public:
  static std::string tuple();
  static std::string notArray(llvm::StringRef type);
};

enum class LibraryReflectRegistryMetadataTypeErrorCode {
  NO_RECORD_TYPE,
};

struct LibraryReflectRegistryMetadataTypeError {
  LibraryReflectRegistryMetadataTypeErrorCode code;
  std::string message;
};

class ErrorTextMetadataType {
public:
  static std::string unknown(llvm::StringRef label);
};

}
