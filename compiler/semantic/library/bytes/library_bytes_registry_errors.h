#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

enum class LibraryBytesRegistryKnownEncodingErrorCode {
  UNKNOWN_ENCODING,
};

struct LibraryBytesRegistryKnownEncodingError {
  LibraryBytesRegistryKnownEncodingErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryBytesIndexingErrorCode {
  NOT_INDEXED,
};

struct LibraryBytesRegistryBytesIndexingError {
  LibraryBytesRegistryBytesIndexingErrorCode code;
  std::string message;
};

class ErrorTextBytesIndex {
public:
  static std::string notIndexed(const Type *type);
  static std::string store(llvm::StringRef target, llvm::StringRef object, llvm::StringRef position,
                           std::optional<int64_t> at);
};

class ErrorTextBytesMember {
public:
  static std::string deprecated(llvm::StringRef name);
  static std::string noMember(llvm::StringRef name, llvm::StringRef type);
  static std::string notCalled(llvm::StringRef name, llvm::StringRef type);
  static std::string narrow(llvm::StringRef name, llvm::StringRef type);
  static std::string lacking(llvm::StringRef name, llvm::StringRef lacking, llvm::StringRef type,
                             llvm::StringRef with);
  static std::string callOnly(llvm::StringRef object, llvm::StringRef name);
};

class ErrorTextBytesString {
public:
  static std::string noText(llvm::StringRef type, bool buffer);
};

class ErrorTextArrayBuffer {
public:
  static std::string notYet(llvm::StringRef name);
  static std::string noMember(llvm::StringRef name);
};

enum class LibraryBytesRegistryCountOfBytesErrorCode {
  NOT_ONE_TO_SIX,
};

struct LibraryBytesRegistryCountOfBytesError {
  LibraryBytesRegistryCountOfBytesErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryDecodedInputErrorCode {
  NOT_BYTES,
};

struct LibraryBytesRegistryDecodedInputError {
  LibraryBytesRegistryDecodedInputErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryVarValueFitsErrorCode {
  VALUE_MAY_NOT_FIT,
};

struct LibraryBytesRegistryVarValueFitsError {
  LibraryBytesRegistryVarValueFitsErrorCode code;
  std::string message;
};

class ErrorTextBuffer {
public:
  static std::string valueArg(llvm::StringRef name, llvm::StringRef type);
  static std::string slice();
  static std::string position(llvm::StringRef name);
  static std::string encoding(llvm::StringRef name);
  static std::string withString();
  static std::string byteCount(llvm::StringRef name);
  static std::string varFits(int64_t count, llvm::StringRef range);
  static std::string decode(llvm::StringRef type);
  static std::string noMember(llvm::StringRef name, llvm::StringRef type);
  static std::string written(llvm::StringRef offset, llvm::StringRef length,
                             llvm::StringRef object);
};

class ErrorTextBufferLabel {
public:
  static std::string comparePosition(size_t index);
  static std::string copyPosition(size_t index);
  static std::string offset();
  static std::string end();
  static std::string length();
  static std::string byte();
  static std::string offsetOf(llvm::StringRef name);
  static std::string options();
};

enum class LibraryBytesRegistrySourceElementErrorCode {
  NOT_ITERABLE,
};

struct LibraryBytesRegistrySourceElementError {
  LibraryBytesRegistrySourceElementErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryConcatSourcesErrorCode {
  NOT_BYTES_LIST,
};

struct LibraryBytesRegistryConcatSourcesError {
  LibraryBytesRegistryConcatSourcesErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryMapResultFitsErrorCode {
  RESULT_MAY_NOT_FIT,
};

struct LibraryBytesRegistryMapResultFitsError {
  LibraryBytesRegistryMapResultFitsErrorCode code;
  std::string message;
};

class ErrorTextBytesStatic {
public:
  static std::string missing(llvm::StringRef object, llvm::StringRef name);
  static std::string notYet(llvm::StringRef object, llvm::StringRef name);
  static std::string from(llvm::StringRef object);
  static std::string source(llvm::StringRef what, llvm::StringRef type);
};

class ErrorTextBufferStatic {
public:
  static std::string offsetOnly(llvm::StringRef type);
  static std::string allocFill(llvm::StringRef type);
  static std::string fillCoding();
  static std::string concat(llvm::StringRef type);
  static std::string byteLength(llvm::StringRef type);
  static std::string unknown(llvm::StringRef name);
  static std::string buffer();
  static std::string size();
  static std::string total();
};

enum class LibraryBytesRegistryValueFitsErrorCode {
  CONSTANT_DOES_NOT_FIT,
  FIT_NOT_PROVEN,
};

struct LibraryBytesRegistryValueFitsError {
  LibraryBytesRegistryValueFitsErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryOffsetAlignedErrorCode {
  MISALIGNED_OFFSET,
};

struct LibraryBytesRegistryOffsetAlignedError {
  LibraryBytesRegistryOffsetAlignedErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryBufferAlignedErrorCode {
  SIZE_NOT_PROVEN,
};

struct LibraryBytesRegistryBufferAlignedError {
  LibraryBytesRegistryBufferAlignedErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryDecoderLabelErrorCode {
  UNKNOWN_LABEL,
};

struct LibraryBytesRegistryDecoderLabelError {
  LibraryBytesRegistryDecoderLabelErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryViewedBufferErrorCode {
  NOT_A_BUFFER,
};

struct LibraryBytesRegistryViewedBufferError {
  LibraryBytesRegistryViewedBufferErrorCode code;
  std::string message;
};

class ErrorTextBytesNew {
public:
  static std::string typeArgs(llvm::StringRef type);
  static std::string buffer();
  static std::string resizable();
  static std::string bufferLen();
  static std::string viewOver(llvm::StringRef type, bool typed);
  static std::string dataView();
  static std::string fatal();
  static std::string the(llvm::StringRef type);
  static std::string offsetOnly(llvm::StringRef type, llvm::StringRef from);
  static std::string lengthOf(llvm::StringRef type);
  static std::string floatLen();
};

class ErrorTextTextDecoder {
public:
  static std::string unknown(llvm::StringRef label);
  static std::string notKnown();
  static std::string options();
};

class ErrorTextBytesSource {
public:
  static std::string notSource(llvm::StringRef target, llvm::StringRef from, bool string);
  static std::string quoted(llvm::StringRef target);
  static std::string notNumbers(llvm::StringRef from, llvm::StringRef target);
  static std::string doubles(llvm::StringRef from, llvm::StringRef target, llvm::StringRef text);
  static std::string hint(llvm::StringRef target, llvm::StringRef text, llvm::StringRef wrap);
  static std::string mayNotFit(llvm::StringRef from, llvm::StringRef fromRange,
                               llvm::StringRef target, llvm::StringRef range, llvm::StringRef hint);
  static std::string unproven(llvm::StringRef from, llvm::StringRef target, llvm::StringRef range,
                              llvm::StringRef hint);
};

enum class LibraryBytesRegistrySetSourceErrorCode {
  ITERATOR_SOURCE,
};

struct LibraryBytesRegistrySetSourceError {
  LibraryBytesRegistrySetSourceErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryTypedMapFitsErrorCode {
  RESULT_MAY_NOT_FIT,
};

struct LibraryBytesRegistryTypedMapFitsError {
  LibraryBytesRegistryTypedMapFitsErrorCode code;
  std::string message;
};

enum class LibraryBytesRegistryWithIndexErrorCode {
  INDEX_NOT_PROVEN,
};

struct LibraryBytesRegistryWithIndexError {
  LibraryBytesRegistryWithIndexErrorCode code;
  std::string message;
};

class ErrorTextTypedArray {
public:
  static std::string mapFits(llvm::StringRef what, llvm::StringRef range, llvm::StringRef hint);
  static std::string flatMap();
  static std::string iterator();
  static std::string index(llvm::StringRef array);
  static std::string locale();
  static std::string noMember(llvm::StringRef name, llvm::StringRef type);
  static std::string offsetOf(llvm::StringRef name);
  static std::string setOffset();
  static std::string setFits(llvm::StringRef offset, llvm::StringRef source,
                             llvm::StringRef target);
};

}
