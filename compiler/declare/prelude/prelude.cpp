#include "compiler/declare/prelude/prelude.h"
#include "compiler/check/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kPrelude = R"(type Partial<T> = { [K in keyof T]?: T[K] }
type Required<T> = { [K in keyof T]-?: T[K] }
type Readonly<T> = { readonly [K in keyof T]: T[K] }
type Pick<T, K extends keyof T> = { [P in K]: T[P] }
type Omit<T, K extends keyof T> = { [P in Exclude<keyof T, K>]: T[P] }
type Awaited<T> = T extends Promise<infer U> ? Awaited<U> : T
type JsonError = {
  kind: "JSON_ERROR"
  code: "ERR_INVALID_JSON" | "ERR_CIRCULAR_STRUCTURE"
  message: string
}
type BufferEncoding =
  | "ascii"
  | "utf8"
  | "utf-8"
  | "utf16le"
  | "utf-16le"
  | "ucs2"
  | "ucs-2"
  | "base64"
  | "base64url"
  | "latin1"
  | "binary"
  | "hex"
type UriError = { kind: "URI_ERROR"; code: "ERR_INVALID_URI"; message: string }
interface StreamError {
  message: string
}
type QuestionError = { kind: "QUESTION_ERROR"; code: "ERR_USE_AFTER_CLOSE"; message: string }
type RegexError = { kind: "REGEX_ERROR"; code: "ERR_INVALID_REGEX"; message: string }
type UrlError = { kind: "URL_ERROR"; code: "ERR_INVALID_URL"; message: string; input: string }
type DateError = { kind: "DATE_ERROR"; code: "ERR_INVALID_DATE"; message: string; text: string }
type LocaleError = { kind: "LOCALE_ERROR"; code: "ERR_INVALID_LOCALE"; message: string; tag: string }
type DecodeError = { kind: "DECODE_ERROR"; code: "ERR_ENCODING_INVALID_ENCODED_DATA"; message: string }
type EncodingError = { kind: "ENCODING_ERROR"; code: "ERR_ENCODING_NOT_SUPPORTED"; message: string }
type DecimalError = { kind: "DECIMAL_ERROR"; code: "ERR_INVALID_DECIMAL"; message: string; text: string }
type NumberError = { kind: "NUMBER_ERROR"; code: "ERR_INVALID_NUMBER"; message: string }
type RedirectStatus = 301 | 302 | 303 | 307 | 308
type ReflectError = {
  kind: "REFLECT_ERROR"
  code:
    | "ERR_INVALID_ARGUMENT"
    | "ERR_INVALID_ARITY"
    | "ERR_INVALID_THIS"
    | "ERR_NO_VARIANT"
    | "ERR_SEVERAL_VARIANTS"
    | "ERR_NOT_FUNCTION"
    | "ERR_NOT_CLASS"
    | "ERR_NOT_REFLECTED"
    | "ERR_ABSTRACT_CLASS"
  message: string
}
type TlsCode =
  | "UNABLE_TO_GET_ISSUER_CERT"
  | "UNABLE_TO_GET_CRL"
  | "UNABLE_TO_DECRYPT_CERT_SIGNATURE"
  | "UNABLE_TO_DECRYPT_CRL_SIGNATURE"
  | "UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY"
  | "CERT_SIGNATURE_FAILURE"
  | "CRL_SIGNATURE_FAILURE"
  | "CERT_NOT_YET_VALID"
  | "CERT_HAS_EXPIRED"
  | "CRL_NOT_YET_VALID"
  | "CRL_HAS_EXPIRED"
  | "ERROR_IN_CERT_NOT_BEFORE_FIELD"
  | "ERROR_IN_CERT_NOT_AFTER_FIELD"
  | "ERROR_IN_CRL_LAST_UPDATE_FIELD"
  | "ERROR_IN_CRL_NEXT_UPDATE_FIELD"
  | "OUT_OF_MEM"
  | "DEPTH_ZERO_SELF_SIGNED_CERT"
  | "SELF_SIGNED_CERT_IN_CHAIN"
  | "UNABLE_TO_GET_ISSUER_CERT_LOCALLY"
  | "UNABLE_TO_VERIFY_LEAF_SIGNATURE"
  | "CERT_CHAIN_TOO_LONG"
  | "CERT_REVOKED"
  | "INVALID_CA"
  | "PATH_LENGTH_EXCEEDED"
  | "INVALID_PURPOSE"
  | "CERT_UNTRUSTED"
  | "CERT_REJECTED"
  | "HOSTNAME_MISMATCH"
  | "UNSPECIFIED"
  | "ERR_TLS_CERT_ALTNAME_INVALID"
  | "ERR_TLS_HANDSHAKE"
type FetchError<E = never> =
  | {
      kind: "FETCH_ERROR_REQUEST"
      code:
        | "ERR_INVALID_URL"
        | "ERR_CREDENTIALS_IN_URL"
        | "ERR_INVALID_METHOD"
        | "ERR_BODY_WITH_GET"
        | "ERR_DUPLEX_REQUIRED"
        | "ERR_REQUEST_USED"
        | "ERR_UNSUPPORTED_SCHEME"
        | "ERR_BLOCKED_PORT"
        | "ERR_BODY_FAILED"
      message: string
      url: string
    }
  | { kind: "FETCH_ERROR_REQUEST"; code: "ERR_STREAM_FAILED"; message: string; url: string; details: E }
  | {
      kind: "FETCH_ERROR_NETWORK"
      code:
        | "ENOTFOUND"
        | "EAI_AGAIN"
        | "ECONNREFUSED"
        | "ETIMEDOUT"
        | "ENETUNREACH"
        | "EHOSTUNREACH"
        | "EADDRNOTAVAIL"
        | "ECONNRESET"
        | "ECONNABORTED"
        | "EPIPE"
        | "EMFILE"
        | "ENOBUFS"
        | "UNKNOWN"
      message: string
      syscall: string
      host: string
      port: int
    }
  | {
      kind: "FETCH_ERROR_RESPONSE"
      code: "ERR_CLOSED" | "ERR_INVALID_RESPONSE" | "ERR_HEAD_TOO_LARGE"
      message: string
      host: string
      port: int
    }
  | { kind: "FETCH_ERROR_TLS"; code: TlsCode; message: string; host: string }
  | {
      kind: "FETCH_ERROR_REDIRECT"
      code:
        | "ERR_UNEXPECTED_REDIRECT"
        | "ERR_TOO_MANY_REDIRECTS"
        | "ERR_INVALID_LOCATION"
        | "ERR_REDIRECT_WITH_STREAM"
      message: string
      location: string
    }
  | { kind: "FETCH_ERROR_TIMEOUT"; code: "TIMEOUT_ERR"; message: string; timeout: int }
  | {
      kind: "FETCH_ERROR_PROXY"
      code: "ERR_INVALID_PROXY_URL" | "ERR_PROXY_AUTH_REQUIRED" | "ERR_PROXY_REFUSED"
      message: string
      proxy: string
      status: int
    }
interface BodyErrorUsed extends StreamError {
  kind: "BODY_ERROR_USED"
  code: "ERR_BODY_USED" | "ERR_BODY_LOCKED"
}
interface BodyErrorNetwork extends StreamError {
  kind: "BODY_ERROR_NETWORK"
  code: "ERR_CLOSED" | "ECONNRESET" | "ECONNABORTED" | "ETIMEDOUT" | "ERR_TLS_FAILED" | "UNKNOWN"
}
interface BodyErrorTimeout extends StreamError {
  kind: "BODY_ERROR_TIMEOUT"
  code: "TIMEOUT_ERR"
  timeout: int
}
interface BodyErrorTooLarge extends StreamError {
  kind: "BODY_ERROR_TOO_LARGE"
  code: "ERR_BODY_TOO_LARGE"
  limit: double
}
interface BodyErrorMalformed extends StreamError {
  kind: "BODY_ERROR_MALFORMED"
  code: "ERR_INVALID_CHUNKED" | "ERR_INVALID_COMPRESSION"
}
interface BodyErrorStream extends StreamError {
  kind: "BODY_ERROR_STREAM"
  code: "ERR_STREAM_FAILED"
}
type BodyError =
  | BodyErrorUsed
  | BodyErrorNetwork
  | BodyErrorTimeout
  | BodyErrorTooLarge
  | BodyErrorMalformed
  | BodyErrorStream
interface ReaderError extends StreamError {
  kind: "READER_ERROR"
  code: "ERR_STREAM_LOCKED" | "ERR_READER_RELEASED"
}
interface ControllerClosedError extends StreamError {
  kind: "CONTROLLER_CLOSED_ERROR"
  code: "ERR_INVALID_STATE"
}
type TextError = BodyError
type BytesError = BodyError
type JsonBodyError =
  | BodyError
  | { kind: "JSON_BODY_ERROR_CONTENT"; code: "ERR_INVALID_JSON"; message: string; contentType?: string; start: string }
type FormDataError =
  | BodyError
  | {
      kind: "FORM_DATA_ERROR_CONTENT"
      code: "ERR_NOT_A_FORM" | "ERR_NO_BOUNDARY" | "ERR_INVALID_MULTIPART"
      message: string
      contentType?: string
    }
type SubtleDigestError =
  | { kind: "SUBTLE_DIGEST_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_DIGEST_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_DIGEST_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleGenerateKeyError =
  | { kind: "SUBTLE_GENERATE_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_GENERATE_KEY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_GENERATE_KEY_ERROR_WRONG_USAGE"; code: "ERR_INVALID_KEY_USAGE"; message: string }
  | { kind: "SUBTLE_GENERATE_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleImportKeyError =
  | { kind: "SUBTLE_IMPORT_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_IMPORT_KEY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_IMPORT_KEY_ERROR_WRONG_USAGE"; code: "ERR_INVALID_KEY_USAGE"; message: string }
  | { kind: "SUBTLE_IMPORT_KEY_ERROR_NOT_A_KEY"; code: "ERR_INVALID_DATA"; message: string }
  | { kind: "SUBTLE_IMPORT_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleExportKeyError =
  | { kind: "SUBTLE_EXPORT_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_EXPORT_KEY_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_EXPORT_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleSignError =
  | { kind: "SUBTLE_SIGN_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_SIGN_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_SIGN_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_SIGN_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleVerifyError =
  | { kind: "SUBTLE_VERIFY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_VERIFY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_VERIFY_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_VERIFY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleEncryptError =
  | { kind: "SUBTLE_ENCRYPT_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_ENCRYPT_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_ENCRYPT_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_ENCRYPT_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleDecryptError =
  | { kind: "SUBTLE_DECRYPT_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_DECRYPT_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_DECRYPT_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_DECRYPT_ERROR_DECRYPTION_FAILED"; code: "ERR_OPERATION_FAILED"; message: string }
  | { kind: "SUBTLE_DECRYPT_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleDeriveBitsError =
  | { kind: "SUBTLE_DERIVE_BITS_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_DERIVE_BITS_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_DERIVE_BITS_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_DERIVE_BITS_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleDeriveKeyError =
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_WRONG_USAGE"; code: "ERR_INVALID_KEY_USAGE"; message: string }
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_NOT_A_KEY"; code: "ERR_INVALID_DATA"; message: string }
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleWrapKeyError =
  | { kind: "SUBTLE_WRAP_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_WRAP_KEY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_WRAP_KEY_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_WRAP_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleUnwrapKeyError =
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_WRONG_USAGE"; code: "ERR_INVALID_KEY_USAGE"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_NOT_A_KEY"; code: "ERR_INVALID_DATA"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_DECRYPTION_FAILED"; code: "ERR_OPERATION_FAILED"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
)";

}

Prelude::Prelude(Checker &checker) : checker_(checker) {}

void Prelude::declarePrelude() {
  std::string source = kPrelude.str() + "type HttpStatus =";
  for (int status = 200; status <= 599; ++status) {
    source += (status == 200 ? " " : " | ") + std::to_string(status);
  }
  source += "\n";
  std::unique_ptr<llvm::MemoryBuffer> buffer = llvm::MemoryBuffer::getMemBufferCopy(source, "<tz>");
  llvm::StringRef text = buffer->getBuffer();
  checker_.diags_.sourceMgr().AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  Lexer lexer(text, checker_.diags_);
  Parser parser(lexer, checker_.ctx_, checker_.diags_);
  SourceFile prelude = parser.parseSourceFile();
  for (Stmt *item : prelude.items) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
      checker_.builtinAliases_[alias->name] = alias;
    }
  }
}

const Type *Prelude::builtinType(llvm::StringRef name) {
  TypeAliasDecl *alias = checker_.builtinAliases_.lookup(name);
  assert(alias && "the prelude declares the type");

  return checker_.typeDeclarer().resolveAlias(alias);
}

const Type *Prelude::builtinType(llvm::StringRef name, std::vector<const Type *> args) {
  TypeAliasDecl *alias = checker_.builtinAliases_.lookup(name);
  assert(alias && "the prelude declares the type");

  return checker_.aliasInstantiator().instantiateAlias(alias, std::move(args), alias->loc());
}

}
