#pragma once

#include "compiler/program/library/intl/intl_service_table/intl_service_table.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>

namespace tz {

enum class LibraryIntlRegistryIntlCreationErrorCode {
  UNKNOWN_SERVICE,
  NEEDS_NEW,
  TOO_MANY_ARGUMENTS,
  NO_DISPLAY_TYPE,
};

struct LibraryIntlRegistryIntlCreationError {
  LibraryIntlRegistryIntlCreationErrorCode code;
  std::string message;
};

enum class LibraryIntlRegistryLocalePresentErrorCode {
  NO_LOCALE_CLASS,
  OPTIONS_GIVEN,
};

struct LibraryIntlRegistryLocalePresentError {
  LibraryIntlRegistryLocalePresentErrorCode code;
  std::string message;
};

enum class LibraryIntlRegistryLanguageTagsErrorCode {
  UNKNOWN_TAGS,
  NOT_A_TAG,
};

struct LibraryIntlRegistryLanguageTagsError {
  LibraryIntlRegistryLanguageTagsErrorCode code;
  std::string message;
};

class ErrorTextIntlNew {
public:
  static std::string unknown(llvm::StringRef name);
  static std::string withNew(llvm::StringRef name);
  static std::string noType();
  static std::string label(llvm::StringRef name);
};

class ErrorTextLanguageTag {
public:
  static std::string unknown(bool constructor, llvm::StringRef what);
  static std::string notTag(llvm::StringRef tag);
  static std::string takes(llvm::StringRef what, llvm::StringRef type, bool array);
  static std::string list(llvm::StringRef what, llvm::StringRef type);
  static std::string listOpen(llvm::StringRef what);
  static std::string spread(llvm::StringRef what);
};

class ErrorTextIntlLocale {
public:
  static std::string missing();
  static std::string options();
  static std::string label();
  static std::string withNew();
};

class ErrorTextIntlFunction {
public:
  static std::string canonical();
  static std::string values();
  static std::string staticOf(llvm::StringRef service, llvm::StringRef member);
  static std::string onlyLocale(llvm::StringRef what);
};

class ErrorTextIntlMember {
public:
  static std::string dateFormat(llvm::StringRef name);
  static std::string onlyCalled(llvm::StringRef name, llvm::StringRef type);
};

enum class LibraryIntlRegistryIntlNumberErrorCode {
  NOT_A_NUMBER,
};

struct LibraryIntlRegistryIntlNumberError {
  LibraryIntlRegistryIntlNumberErrorCode code;
  std::string message;
};

enum class LibraryIntlRegistryClosedArgErrorCode {
  UNKNOWN_TEXT,
  NOT_ALLOWED,
};

struct LibraryIntlRegistryClosedArgError {
  LibraryIntlRegistryClosedArgErrorCode code;
  std::string message;
};

class ErrorTextIntlNumber {
public:
  static std::string notNumber(llvm::StringRef method, const Type *type);
};

class ErrorTextClosedArg {
public:
  static std::string unknown(llvm::StringRef what, llvm::StringRef first, llvm::StringRef second);
  static std::string notAllowed(llvm::StringRef text, llvm::StringRef what);
  static std::string unit();
  static std::string form();
};

class ErrorTextIntlMethod {
public:
  static std::string range(llvm::StringRef name);
  static std::string selectSpan();
  static std::string badUnit(llvm::StringRef unit);
  static std::string noMember(llvm::StringRef name, llvm::StringRef service);
  static std::string badForm(llvm::StringRef form);
};

enum class LibraryIntlRegistryOptionsGivenErrorCode {
  NO_DISPLAY_TYPE,
  NOT_AN_OBJECT,
};

struct LibraryIntlRegistryOptionsGivenError {
  LibraryIntlRegistryOptionsGivenErrorCode code;
  std::string message;
};

class ErrorTextIntlOptions {
public:
  static std::string noType();
  static std::string notObject(llvm::StringRef what, llvm::StringRef type);
};

class ErrorTextIntlField {
public:
  static std::string values(const std::vector<llvm::StringLiteral> &values);
  static std::string unknown(llvm::StringRef name, llvm::StringRef what,
                             const IntlService *service);
  static std::string kind(llvm::StringRef name, IntlOptionKind kind, llvm::StringRef type);
  static std::string kindName(IntlOptionKind kind);
  static std::string badValue(llvm::StringRef text, llvm::StringRef name,
                              const std::vector<llvm::StringLiteral> &values);
  static std::string open(llvm::StringRef name, const std::vector<llvm::StringLiteral> &values);
  static std::string currency(llvm::StringRef code);
  static std::string unit(llvm::StringRef unit);
  static std::string keyword(llvm::StringRef value, llvm::StringRef name);
  static std::string styleCur();
  static std::string styleUnit();
  static std::string range(llvm::StringRef name, int min, int max);
  static std::string rangeOpen(llvm::StringRef name, int min, int max);
  static std::string above(llvm::StringRef min, llvm::StringRef max, bool known);
  static std::string incSig();
  static std::string incSame();
  static std::string increments(bool known);
};

enum class LibraryIntlRegistryOptionsObjectErrorCode {
  NOT_AN_OBJECT,
};

struct LibraryIntlRegistryOptionsObjectError {
  LibraryIntlRegistryOptionsObjectErrorCode code;
  std::string message;
};

class ErrorTextDateOptions {
public:
  static std::string method(int kind);
  static std::string notObject(llvm::StringRef method, llvm::StringRef type);
  static std::string unknown(llvm::StringRef name, llvm::StringRef method);
  static std::string kind(llvm::StringRef name, bool boolean, bool number, llvm::StringRef type);
  static std::string styleOnly(llvm::StringRef name, llvm::StringRef method);
  static std::string styleParts();
  static std::string digitsName();
  static std::string digits();
  static std::string unknownVal(llvm::StringRef name, llvm::StringRef value, llvm::StringRef first,
                                llvm::StringRef second);
  static std::string badValue(llvm::StringRef text, llvm::StringRef name,
                              llvm::ArrayRef<llvm::StringLiteral> allowed);
};

class ErrorTextDateFormat {
public:
  static std::string arity(size_t count);
  static std::string formatArg(llvm::StringRef name, llvm::StringRef type);
  static std::string range(llvm::StringRef name);
  static std::string noMember(llvm::StringRef name);
};

}
