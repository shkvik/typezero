#pragma once

#include "compiler/program/symbols/var_symbol/var_symbol.h"
#include "compiler/program/types/types.h"
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
  static std::string foreign(llvm::StringRef event, llvm::StringRef owner);
  static std::string voidValue(llvm::StringRef event);
};

class ErrorTextEventsExport {
public:
  static std::string hint(llvm::StringRef name);
};

enum class LibraryEmittersEventMapFitsErrorCode {
  NOT_A_MAP,
  SEVERAL_VALUES,
  SERVICE_EVENT,
};

struct LibraryEmittersEventMapFitsError {
  LibraryEmittersEventMapFitsErrorCode code;
  std::string message;
};

enum class LibraryEventsRegistryListenerThisErrorCode {
  OTHER_RECEIVER,
};

struct LibraryEventsRegistryListenerThisError {
  LibraryEventsRegistryListenerThisErrorCode code;
  std::string message;
};

class ErrorTextEventMap {
public:
  static std::string notMap(llvm::StringRef why);
  static std::string notTuple(llvm::StringRef type);
  static std::string notOne(llvm::StringRef type);
  static std::string optional(llvm::StringRef event);
  static std::string fieldTuple(llvm::StringRef event, llvm::StringRef type);
  static std::string eachEvent();
  static std::string event(llvm::StringRef name);
  static std::string several(llvm::StringRef event, llvm::StringRef args);
  static std::string service(llvm::StringRef event, llvm::StringRef type);
};

class ErrorTextEventName {
public:
  static std::string unknown(llvm::StringRef method);
  static std::string list(const Type *events);
  static std::string didYouMean(llvm::StringRef name);
  static std::string notEmitted(llvm::StringRef key, const Type *events, llvm::StringRef nearest);
  static std::string untyped(llvm::StringRef key, llvm::StringRef place);
  static std::string circular(llvm::StringRef key);
  static std::string noEvent(llvm::StringRef key, const Type *events, llvm::StringRef nearest);
};

class ErrorTextListener {
public:
  static std::string thisType(llvm::StringRef self, llvm::StringRef receiver);
  static std::string asyncThis();
  static std::string notFunc(llvm::StringRef type);
  static std::string gets(llvm::StringRef event, llvm::StringRef args, llvm::StringRef type);
  static std::string getsArrow(llvm::StringRef event, llvm::StringRef args, llvm::StringRef type);
  static std::string asyncRet(llvm::StringRef type);
};

enum class LibraryEventsRegistryListenerEventErrorCode {
  NOT_AN_EVENT,
};

struct LibraryEventsRegistryListenerEventError {
  LibraryEventsRegistryListenerEventErrorCode code;
  std::string message;
};

enum class LibraryEventsRegistryEmitterEventsErrorCode {
  NOT_AN_EMITTER,
};

struct LibraryEventsRegistryEmitterEventsError {
  LibraryEventsRegistryEmitterEventsErrorCode code;
  std::string message;
};

enum class LibraryEventsRegistryOneErrorValueErrorCode {
  SEVERAL_VALUES,
};

struct LibraryEventsRegistryOneErrorValueError {
  LibraryEventsRegistryOneErrorValueErrorCode code;
  std::string message;
};

class ErrorTextEventTarget {
public:
  static std::string listener(llvm::StringRef type);
};

class ErrorTextEventsFunction {
public:
  static std::string notEmitter(llvm::StringRef fn, bool target, llvm::StringRef type);
  static std::string targetOn();
  static std::string endless(llvm::StringRef name, llvm::StringRef fn);
  static std::string options();
  static std::string errorValue(llvm::StringRef fn, llvm::StringRef errors);
};

}
