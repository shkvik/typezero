#pragma once

#include "compiler/program/library/crypto/subtle_operation_table/subtle_operation_table.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>

namespace tz {

enum class LibraryCryptoRegistryKeyObjectTypeErrorCode {
  NOT_A_KEY_OBJECT,
};

struct LibraryCryptoRegistryKeyObjectTypeError {
  LibraryCryptoRegistryKeyObjectTypeErrorCode code;
  std::string message;
};

class ErrorTextCryptoData {
public:
  static std::string notData(llvm::StringRef what, bool strings, bool buffers, bool keys,
                             const Type *type);
  static std::string encoding(llvm::StringRef fn);
};

class ErrorTextCryptoOptions {
public:
  static std::string literal(llvm::StringRef fn, llvm::StringRef first);
  static std::string byName(llvm::StringRef fn, llvm::StringRef first);
  static std::string unknown(llvm::StringRef fn, llvm::StringRef name,
                             llvm::ArrayRef<llvm::StringRef> allowed);
  static std::string needsKey(llvm::StringRef fn);
};

class ErrorTextCryptoChoice {
public:
  static std::string list(llvm::ArrayRef<llvm::StringRef> values);
  static std::string literal(llvm::StringRef what, llvm::ArrayRef<llvm::StringRef> values);
  static std::string unknown(llvm::StringRef what, llvm::StringRef text,
                             llvm::ArrayRef<llvm::StringRef> values);
};

class ErrorTextCryptoHash {
public:
  static std::string anyLength(llvm::StringRef text);
  static std::string turnedOff(llvm::StringRef text);
  static std::string unknown(llvm::StringRef text);
};

class ErrorTextCryptoCipher {
public:
  static std::string turnedOff(llvm::StringRef text);
  static std::string unknown(llvm::StringRef text);
  static std::string keyBytes(llvm::StringRef name, int64_t want, int64_t have);
  static std::string emptyIv(llvm::StringRef name);
  static std::string ivBytes(llvm::StringRef name, int64_t want, int64_t have);
};

class ErrorTextCryptoNumber {
public:
  static std::string whole(llvm::StringRef name, llvm::StringRef least, llvm::StringRef value);
};

class ErrorTextCryptoKey {
public:
  static std::string keyObject(llvm::StringRef what, llvm::StringRef type);
  static std::string callback(llvm::StringRef fn, llvm::StringRef hint);
};

class ErrorTextCryptoLabel {
public:
  static std::string key();
  static std::string privateKey();
  static std::string publicKey();
  static std::string otherKey();
  static std::string signatureEncoding();
  static std::string signature();
  static std::string data();
  static std::string password();
  static std::string salt();
  static std::string iterations();
  static std::string keylen();
  static std::string ikm();
  static std::string info();
  static std::string iv();
  static std::string candidate();
  static std::string size();
  static std::string aad();
  static std::string tag();
  static std::string subjectOption();
};

class ErrorTextCryptoMethods {
public:
  static std::string of(Type::Crypto kind);
};

class ErrorTextCryptoValue {
public:
  static std::string constants();
  static std::string isType(llvm::StringRef name);
  static std::string constant(llvm::StringRef name);
};

class ErrorTextCryptoMember {
public:
  static std::string keyDetails();
  static std::string issuerCert();
  static std::string onlyCalled(llvm::StringRef name, llvm::StringRef type,
                                llvm::StringRef methods);
};

class ErrorTextCryptoMake {
public:
  static std::string notNode(llvm::StringRef cls);
  static std::string x509();
};

class ErrorTextX509 {
public:
  static std::string noStatic(llvm::StringRef name);
  static std::string parseArgs();
  static std::string data();
};

class ErrorTextGlobalCrypto {
public:
  static std::string subtleCall();
  static std::string noMember(llvm::StringRef name);
  static std::string onlyCalled(llvm::StringRef name);
};

enum class LibraryCryptoRegistryFilledTargetErrorCode {
  NOT_INTEGERS,
  NOT_BYTES,
};

struct LibraryCryptoRegistryFilledTargetError {
  LibraryCryptoRegistryFilledTargetErrorCode code;
  std::string message;
};

enum class LibraryCryptoRegistryCurveIsKnownErrorCode {
  UNKNOWN_CURVE,
};

struct LibraryCryptoRegistryCurveIsKnownError {
  LibraryCryptoRegistryCurveIsKnownErrorCode code;
  std::string message;
};

class ErrorTextCryptoFunction {
public:
  static std::string callback(llvm::StringRef fn);
  static std::string isClass(llvm::StringRef fn);
  static std::string randomArgs();
  static std::string randomInt(llvm::StringRef type);
  static std::string greater(bool minimum);
  static std::string randomVals(llvm::StringRef type);
  static std::string fillsBytes(llvm::StringRef shown, llvm::StringRef type);
  static std::string fillFits(llvm::StringRef count, llvm::StringRef at);
  static std::string together(llvm::StringRef first, llvm::StringRef second);
  static std::string costN(llvm::StringRef cost);
  static std::string needs(llvm::StringRef shown, llvm::StringRef required);
  static std::string tagLength(llvm::StringRef name);
  static std::string keyCoding();
  static std::string keyLength(llvm::StringRef shown);
  static std::string aesLength();
  static std::string curve(llvm::StringRef curve);
  static std::string dhKeys();
  static std::string bigint();
};

class ErrorTextCryptoFnLabel {
public:
  static std::string offset();
  static std::string size();
  static std::string keyBits();
  static std::string first();
  static std::string second();
  static std::string argon();
  static std::string keyFormat();
  static std::string keyType();
  static std::string privateKey();
  static std::string publicKey();
};

class ErrorTextCryptoPoint {
public:
  static std::string format();
};

enum class LibraryCryptoRegistryIssuerErrorCode {
  NOT_A_CERTIFICATE,
};

struct LibraryCryptoRegistryIssuerError {
  LibraryCryptoRegistryIssuerErrorCode code;
  std::string message;
};

class ErrorTextCryptoMethod {
public:
  static std::string issuer(llvm::StringRef type);
  static std::string stream(llvm::StringRef name, llvm::StringRef type, Type::Crypto kind,
                            bool cipher);
  static std::string noMethod(llvm::StringRef name, llvm::StringRef type, llvm::StringRef methods);
};

enum class LibraryCryptoRegistryKeyOptionsErrorCode {
  NO_MODULUS_LENGTH,
  NO_NAMED_CURVE,
};

struct LibraryCryptoRegistryKeyOptionsError {
  LibraryCryptoRegistryKeyOptionsErrorCode code;
  std::string message;
};

class ErrorTextKeyPair {
public:
  static std::string option(llvm::StringRef name);
  static std::string modulus(llvm::StringRef type);
  static std::string curve();
  static std::string small(llvm::StringRef bits);
  static std::string noFormat(llvm::StringRef option);
  static std::string noType(llvm::StringRef option, bool publicKey);
  static std::string passphrase();
};

class ErrorTextKeyPairLabel {
public:
  static std::string pairType();
  static std::string format();
  static std::string keyType();
};

enum class LibraryCryptoRegistryKnownFormatErrorCode {
  UNKNOWN_FORMAT,
};

struct LibraryCryptoRegistryKnownFormatError {
  LibraryCryptoRegistryKnownFormatErrorCode code;
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

enum class LibraryCryptoRegistryKnownAlgorithmErrorCode {
  EXPERIMENTAL,
  OTHER_OPERATION,
  UNKNOWN_ALGORITHM,
};

struct LibraryCryptoRegistryKnownAlgorithmError {
  LibraryCryptoRegistryKnownAlgorithmErrorCode code;
  std::string message;
};

enum class LibraryCryptoRegistryParamsAsObjectErrorCode {
  NEEDS_OBJECT,
};

struct LibraryCryptoRegistryParamsAsObjectError {
  LibraryCryptoRegistryParamsAsObjectErrorCode code;
  std::string message;
};

enum class LibraryCryptoRegistryMembersPresentErrorCode {
  MISSING_MEMBER,
};

struct LibraryCryptoRegistryMembersPresentError {
  LibraryCryptoRegistryMembersPresentErrorCode code;
  std::string message;
};

enum class LibraryCryptoRegistryKnownShaHashErrorCode {
  UNKNOWN_HASH,
  NOT_SHA,
};

struct LibraryCryptoRegistryKnownShaHashError {
  LibraryCryptoRegistryKnownShaHashErrorCode code;
  std::string message;
};

enum class LibraryCryptoRegistryKnownCurveErrorCode {
  UNKNOWN_CURVE,
};

struct LibraryCryptoRegistryKnownCurveError {
  LibraryCryptoRegistryKnownCurveErrorCode code;
  std::string message;
};

enum class LibraryCryptoRegistryAesLengthErrorCode {
  BAD_LENGTH,
};

struct LibraryCryptoRegistryAesLengthError {
  LibraryCryptoRegistryAesLengthErrorCode code;
  std::string message;
};

enum class LibraryCryptoRegistryGcmTagLengthErrorCode {
  BAD_TAG,
};

struct LibraryCryptoRegistryGcmTagLengthError {
  LibraryCryptoRegistryGcmTagLengthErrorCode code;
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

enum class LibraryCryptoRegistrySubtleShapeErrorCode {
  NOT_AN_INTRINSIC,
};

struct LibraryCryptoRegistrySubtleShapeError {
  LibraryCryptoRegistrySubtleShapeErrorCode code;
  std::string message;
};

enum class LibraryCryptoRegistryLiteralAlgorithmErrorCode {
  NOT_A_LITERAL,
};

struct LibraryCryptoRegistryLiteralAlgorithmError {
  LibraryCryptoRegistryLiteralAlgorithmErrorCode code;
  std::string message;
};

enum class LibraryCryptoRegistrySubtleAvailableErrorCode {
  NO_SUBTLE,
};

struct LibraryCryptoRegistrySubtleAvailableError {
  LibraryCryptoRegistrySubtleAvailableErrorCode code;
  std::string message;
};

class ErrorTextSubtle {
public:
  static std::string literal(llvm::StringRef name);
  static std::string noSubtle();
};

}
