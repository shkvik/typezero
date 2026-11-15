#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryModulesModuleFoundErrorCode {
  NOT_EXPORTED,
};

struct LibraryModulesModuleFoundError {
  LibraryModulesModuleFoundErrorCode code;
  std::string message;
};

class ErrorTextNodeImport {
public:
  static std::string conflict(llvm::StringRef alias, llvm::StringRef name, llvm::StringRef path);
  static std::string declared(llvm::StringRef alias);
  static std::string duplicate(llvm::StringRef alias);
  static std::string list(llvm::ArrayRef<llvm::StringRef> used);
  static std::string form(bool isDefault);
  static std::string process(llvm::StringRef form);
  static std::string buffer(llvm::StringRef form, llvm::StringRef path);
  static std::string uses(llvm::StringRef form, llvm::StringRef names, llvm::StringRef path);
  static std::string typical(llvm::StringRef form, llvm::StringRef name, llvm::StringRef path);
};

class ErrorTextNodeValue {
public:
  static std::string notValue(llvm::StringRef label, bool path, llvm::StringRef typical);
  static std::string onlyCalled(llvm::StringRef name, llvm::StringRef label);
  static std::string notFunc(llvm::StringRef name, llvm::StringRef label);
};

enum class LibraryModulesExportFoundErrorCode {
  NO_EXPORT,
  NOT_YET,
  NOT_POSSIBLE,
  ELSEWHERE,
  CALLBACK_FORM,
};

struct LibraryModulesExportFoundError {
  LibraryModulesExportFoundErrorCode code;
  std::string message;
};

class ErrorTextModuleExport {
public:
  static std::string none(llvm::StringRef path, llvm::StringRef name);
  static std::string notYet(llvm::StringRef name, llvm::StringRef path);
};

class ErrorTextRuntimeObject {
public:
  static std::string members(Type::Runtime kind);
  static std::string noMember(llvm::StringRef name, llvm::StringRef type, Type::Runtime kind);
  static std::string assigned(llvm::StringRef name, llvm::StringRef type, Type::Runtime kind);
};

class ErrorTextReadlineMethod {
public:
  static std::string callback();
  static std::string options();
  static std::string events(llvm::StringRef name);
};

class ErrorTextAsyncIterator {
public:
  static std::string noThrow();
  static std::string notYet(llvm::StringRef name);
};

enum class LibraryDispatchRegistryIntrinsicShapeErrorCode {
  NOT_AN_INTRINSIC,
};

struct LibraryDispatchRegistryIntrinsicShapeError {
  LibraryDispatchRegistryIntrinsicShapeErrorCode code;
  std::string message;
};

class ErrorTextLibraryCall {
public:
  static std::string listener();
  static std::string asClass();
  static std::string ahead(llvm::StringRef name);
  static std::string context();
};

class ErrorTextFsExport {
public:
  static std::string inTzFs(llvm::StringRef path, llvm::StringRef name);
  static std::string callback(llvm::StringRef name, llvm::StringRef path, llvm::StringRef sync);
  static std::string syncOnly(llvm::StringRef name, llvm::StringRef path, llvm::StringRef sync);
  static std::string waiting(llvm::StringRef path, llvm::StringRef name);
  static std::string readline(llvm::StringRef name, llvm::StringRef path);
  static std::string tzFs(llvm::StringRef path, llvm::StringRef name, bool inNodeFs);
  static std::string tzIo(llvm::StringRef path, llvm::StringRef name);
};

class ErrorTextCryptoExport {
public:
  static std::string webcrypto(llvm::StringRef name, llvm::StringRef path);
  static std::string subtle(llvm::StringRef name, llvm::StringRef path);
};

class ErrorTextChildExport {
public:
  static std::string async(llvm::StringRef name, llvm::StringRef path);
  static std::string fork(llvm::StringRef name, llvm::StringRef path);
  static std::string notYet(llvm::StringRef name, llvm::StringRef path);
};

class ErrorTextProcessExport {
public:
  static std::string notYet(llvm::StringRef name, llvm::StringRef path);
};

class ErrorTextBufferExport {
public:
  static std::string notYet(llvm::StringRef name, llvm::StringRef path);
};

class ErrorTextTimersExport {
public:
  static std::string none(llvm::StringRef path, llvm::StringRef name);
};

}
