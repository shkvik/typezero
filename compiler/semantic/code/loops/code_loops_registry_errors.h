#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeLoopsRegistryCheckJumpErrorCode {
  LABEL_NOT_FOUND,
  BREAK_OUTSIDE,
  CONTINUE_OUTSIDE,
  CONTINUE_NOT_LOOP,
};

struct CodeLoopsRegistryCheckJumpError {
  CodeLoopsRegistryCheckJumpErrorCode code;
  std::string message;
};

enum class CodeLoopsRegistryLabelFreshErrorCode {
  DUPLICATE_LABEL,
};

struct CodeLoopsRegistryLabelFreshError {
  CodeLoopsRegistryLabelFreshErrorCode code;
  std::string message;
};

enum class CodeLoopsRegistryLabelBodyErrorCode {
  DECLARATION_LABELED,
};

struct CodeLoopsRegistryLabelBodyError {
  CodeLoopsRegistryLabelBodyErrorCode code;
  std::string message;
};

enum class CodeLoopsRegistryIterableTypeErrorCode {
  MAY_BE_UNDEFINED,
  TUPLE_NOT_ITERABLE,
  RECORD_NOT_ITERABLE,
  ASYNC_ONLY,
  CONCURRENT_MAP,
  NOT_ITERABLE,
};

struct CodeLoopsRegistryIterableTypeError {
  CodeLoopsRegistryIterableTypeErrorCode code;
  std::string message;
};

enum class CodeLoopsRegistryBindingTypeErrorCode {
  FOR_IN_ANNOTATED,
  FOR_OF_ANNOTATED,
};

struct CodeLoopsRegistryBindingTypeError {
  CodeLoopsRegistryBindingTypeErrorCode code;
  std::string message;
};

enum class CodeLoopsRegistryBindingShapeErrorCode {
  NOT_AN_ARRAY,
};

struct CodeLoopsRegistryBindingShapeError {
  CodeLoopsRegistryBindingShapeErrorCode code;
  std::string message;
};

enum class CodeLoopsRegistryWalkableErrorCode {
  INDICES_AS_STRINGS,
  MAY_BE_UNDEFINED,
  DICT_PROPERTIES,
  NOT_AN_OBJECT,
};

struct CodeLoopsRegistryWalkableError {
  CodeLoopsRegistryWalkableErrorCode code;
  std::string message;
};

class ErrorTextJump {
public:
  static std::string noLabel(llvm::StringRef label);
  static std::string breakOut();
  static std::string continueOut();
  static std::string notLoop(llvm::StringRef keyword, llvm::StringRef label);
};

class ErrorTextLabel {
public:
  static std::string duplicate(llvm::StringRef label);
};

class ErrorTextLabelBody {
public:
  static std::string declared();
};

class ErrorTextForOf {
public:
  static std::string mayBeUndef(llvm::StringRef type);
  static std::string tuple();
  static std::string record();
  static std::string later(llvm::StringRef type);
  static std::string concurrent();
  static std::string cannot(llvm::StringRef type);
  static std::string loop();
};

class ErrorTextLoopBinding {
public:
  static std::string forIn();
  static std::string forOf();
};

class ErrorTextLoopShape {
public:
  static std::string notArray(llvm::StringRef element);
};

class ErrorTextForIn {
public:
  static std::string indices(llvm::StringRef what, llvm::StringRef text, bool array);
  static std::string undefined();
  static std::string dict(llvm::StringRef text, bool isMap);
  static std::string notObject(llvm::StringRef type);
  static std::string anArray();
  static std::string aString();
  static std::string aTuple();
};

}
