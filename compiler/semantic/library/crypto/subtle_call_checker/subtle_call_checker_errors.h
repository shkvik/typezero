#pragma once

#include "compiler/program/library/crypto/subtle_operation_table/subtle_operation_table.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class SubtleCallCheckerKnownFormatErrorCode {
  UNKNOWN_FORMAT,
};

struct SubtleCallCheckerKnownFormatError {
  SubtleCallCheckerKnownFormatErrorCode code;
  std::string message;
};

class ErrorTextKeyFormat {
public:
  static std::string unknown(llvm::StringRef format);
};

class ErrorTextSubtleUsage {
public:
  static std::string unknown(llvm::StringRef usage);
  static std::string notOf(llvm::StringRef usage, llvm::StringRef name, int isPublic, bool isSecret,
                           llvm::ArrayRef<llvm::StringRef> allowed);
  static std::string empty(llvm::ArrayRef<llvm::StringRef> own);
  static std::string pairEmpty(llvm::ArrayRef<llvm::StringRef> own);
};

class ErrorTextSubtleKeyData {
public:
  static std::string jwk(llvm::StringRef type);
  static std::string bytes(llvm::StringRef format, llvm::StringRef type);
};

class ErrorTextSubtleImport {
public:
  static std::string format(llvm::StringRef name, llvm::StringRef format,
                            llvm::ArrayRef<llvm::StringRef> formats);
  static std::string unwrap(llvm::StringRef name, llvm::StringRef format,
                            llvm::ArrayRef<llvm::StringRef> formats);
  static std::string derived(llvm::StringRef name);
};

enum class SubtleCallCheckerKnownAlgorithmErrorCode {
  EXPERIMENTAL,
  OTHER_OPERATION,
  UNKNOWN_ALGORITHM,
};

struct SubtleCallCheckerKnownAlgorithmError {
  SubtleCallCheckerKnownAlgorithmErrorCode code;
  std::string message;
};

enum class SubtleCallCheckerParamsAsObjectErrorCode {
  NEEDS_OBJECT,
};

struct SubtleCallCheckerParamsAsObjectError {
  SubtleCallCheckerParamsAsObjectErrorCode code;
  std::string message;
};

enum class SubtleCallCheckerMembersPresentErrorCode {
  MISSING_MEMBER,
};

struct SubtleCallCheckerMembersPresentError {
  SubtleCallCheckerMembersPresentErrorCode code;
  std::string message;
};

enum class SubtleCallCheckerKnownShaHashErrorCode {
  UNKNOWN_HASH,
  NOT_SHA,
};

struct SubtleCallCheckerKnownShaHashError {
  SubtleCallCheckerKnownShaHashErrorCode code;
  std::string message;
};

enum class SubtleCallCheckerKnownCurveErrorCode {
  UNKNOWN_CURVE,
};

struct SubtleCallCheckerKnownCurveError {
  SubtleCallCheckerKnownCurveErrorCode code;
  std::string message;
};

enum class SubtleCallCheckerAesLengthErrorCode {
  BAD_LENGTH,
};

struct SubtleCallCheckerAesLengthError {
  SubtleCallCheckerAesLengthErrorCode code;
  std::string message;
};

enum class SubtleCallCheckerGcmTagLengthErrorCode {
  BAD_TAG,
};

struct SubtleCallCheckerGcmTagLengthError {
  SubtleCallCheckerGcmTagLengthErrorCode code;
  std::string message;
};

class ErrorTextAlgorithm {
public:
  static std::string verb(SubtleOperation operation);
  static std::string choices(llvm::ArrayRef<llvm::StringRef> names);
  static std::string experiment(llvm::StringRef written, llvm::StringRef verb,
                                llvm::StringRef list);
  static std::string otherOp(llvm::StringRef written, llvm::StringRef op, llvm::StringRef verb,
                             llvm::StringRef list);
  static std::string unknown(llvm::StringRef written, llvm::StringRef op, llvm::StringRef verb,
                             llvm::StringRef list);
  static std::string object(llvm::StringRef name, llvm::StringRef op, llvm::StringRef dictionary,
                            llvm::StringRef member, llvm::StringRef example);
  static std::string needs(llvm::StringRef name, llvm::StringRef op, llvm::StringRef member,
                           llvm::StringRef dictionary, llvm::StringRef example);
  static std::string hash(llvm::StringRef hash, llvm::StringRef dictionary, llvm::StringRef list);
  static std::string notSha(llvm::StringRef hash, llvm::StringRef dictionary);
  static std::string curve(llvm::StringRef curve);
  static std::string aesLength();
  static std::string gcmTag();
};

}
