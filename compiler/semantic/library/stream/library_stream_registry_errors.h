#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryStreamRegistryHookParamsErrorCode {
  NO_LIBRARY_HOOK,
  PARAMS_NOT_TYPED,
  OTHER_PARAMETERS,
};

struct LibraryStreamRegistryHookParamsError {
  LibraryStreamRegistryHookParamsErrorCode code;
  std::string message;
};

class ErrorTextStreamHook {
public:
  static std::string params(const FuncSymbol *hook);
  static std::string other(llvm::StringRef name, llvm::StringRef params, bool chunks);
};

enum class LibraryStreamRegistryDecodedChunksErrorCode {
  NOT_READABLE,
  OBJECT_MODE,
};

struct LibraryStreamRegistryDecodedChunksError {
  LibraryStreamRegistryDecodedChunksErrorCode code;
  std::string message;
};

enum class LibraryStreamRegistryStdinPlaceErrorCode {
  NOT_TOP_LEVEL,
};

struct LibraryStreamRegistryStdinPlaceError {
  LibraryStreamRegistryStdinPlaceErrorCode code;
  std::string message;
};

class ErrorTextSetEncoding {
public:
  static std::string unreadable(llvm::StringRef type);
  static std::string objectMode(llvm::StringRef chunk);
  static std::string stdinPlace();
  static std::string changes(llvm::StringRef why);
  static std::string usedBefore(llvm::StringRef how);
  static std::string usedHere();
};

class ErrorTextEncodingWhy {
public:
  static std::string noConstant();
  static std::string notConst(llvm::StringRef name);
  static std::string fieldSet(llvm::StringRef field);
  static std::string ownBlock(llvm::StringRef name);
};

class ErrorTextEncodingUse {
public:
  static std::string listeners(llvm::StringRef method);
  static std::string function();
  static std::string elsewhere();
};

enum class LibraryStreamRegistryOverridesArgsErrorCode {
  NOT_CLASS_AND_LITERAL,
};

struct LibraryStreamRegistryOverridesArgsError {
  LibraryStreamRegistryOverridesArgsErrorCode code;
  std::string message;
};

enum class LibraryStreamRegistryOverriddenMethodErrorCode {
  NO_SUCH_METHOD,
};

struct LibraryStreamRegistryOverriddenMethodError {
  LibraryStreamRegistryOverriddenMethodErrorCode code;
  std::string message;
};

enum class LibraryStreamRegistryChunkNotUndefinedErrorCode {
  UNDEFINED_CHUNK,
};

struct LibraryStreamRegistryChunkNotUndefinedError {
  LibraryStreamRegistryChunkNotUndefinedErrorCode code;
  std::string message;
};

enum class LibraryStreamRegistryStreamSideErrorCode {
  NOT_READABLE,
  NOT_WRITABLE,
};

struct LibraryStreamRegistryStreamSideError {
  LibraryStreamRegistryStreamSideErrorCode code;
  std::string message;
};

class ErrorTextStreamCall {
public:
  static std::string overrides();
  static std::string noMethod(llvm::StringRef cls, llvm::StringRef method);
  static std::string undefined(llvm::StringRef method);
  static std::string chunk(llvm::StringRef method, llvm::StringRef chunk, bool onlyStrings,
                           llvm::StringRef type);
  static std::string pipe();
  static std::string noSide(llvm::StringRef type, llvm::StringRef name, bool reading);
  static std::string endWithFn();
  static std::string endArgs();
};

enum class LibraryStreamRegistryConcreteStreamErrorCode {
  ABSTRACT_STREAM,
};

struct LibraryStreamRegistryConcreteStreamError {
  LibraryStreamRegistryConcreteStreamErrorCode code;
  std::string message;
};

enum class LibraryStreamRegistryChunksGivenErrorCode {
  CHUNK_TYPE_MISSING,
};

struct LibraryStreamRegistryChunksGivenError {
  LibraryStreamRegistryChunksGivenErrorCode code;
  std::string message;
};

class ErrorTextStreamNew {
public:
  static std::string abstract(llvm::StringRef name, bool readable, bool writable);
  static std::string untyped(llvm::StringRef name);
};

enum class LibraryStreamRegistryWritableEndErrorCode {
  NOT_WRITABLE,
};

struct LibraryStreamRegistryWritableEndError {
  LibraryStreamRegistryWritableEndErrorCode code;
  std::string message;
};

enum class LibraryStreamRegistryReadableEndErrorCode {
  NOT_READABLE,
};

struct LibraryStreamRegistryReadableEndError {
  LibraryStreamRegistryReadableEndErrorCode code;
  std::string message;
};

class ErrorTextPipeline {
public:
  static std::string notWritten(llvm::StringRef type);
  static std::string chunkKind(llvm::StringRef fn, llvm::StringRef chunk, llvm::StringRef type,
                               llvm::StringRef takes);
  static std::string notRead(llvm::StringRef type);
  static std::string source();
  static std::string function(bool last);
  static std::string stage(llvm::StringRef type);
  static std::string callback();
  static std::string streams();
};

class ErrorTextCompose {
public:
  static std::string streams();
  static std::string notStream(llvm::StringRef type);
};

enum class LibraryStreamRegistryValuesIteratorErrorCode {
  NOT_ITERABLE,
};

struct LibraryStreamRegistryValuesIteratorError {
  LibraryStreamRegistryValuesIteratorErrorCode code;
  std::string message;
};

class ErrorTextStreamFrom {
public:
  static std::string source(llvm::StringRef type);
};

}
