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
type JsonError = { code: "bad_json" | "bad_field" | "bad_class"; message: string; path: string }
type UriError = { code: "bad_uri"; message: string }
type RegexError = { code: "bad_regex"; message: string }
type UrlError = { code: "bad_url"; message: string; input: string }
type DateError = { code: "bad_date"; message: string; text: string }
type LocaleError = { code: "bad_locale"; message: string; tag: string }
type CryptoError = { code: string; message: string }
type RedirectStatus = 301 | 302 | 303 | 307 | 308
type ReflectError = {
  code:
    | "bad_argument"
    | "bad_arity"
    | "bad_this"
    | "no_variant"
    | "several_variants"
    | "not_function"
    | "not_class"
    | "not_reflected"
    | "abstract"
  message: string
}
type FetchError = { code: string; message: string; cause?: string }
type BodyError = {
  code: "body_used" | "terminated" | "bad_form" | "body_failed" | "ABORT_ERR" | "TIMEOUT_ERR"
  message: string
  cause?: string
}
type SubtleError = {
  code:
    | "NotSupportedError"
    | "DataError"
    | "OperationError"
    | "InvalidAccessError"
    | "SyntaxError"
    | "TypeError"
    | "RangeError"
  message: string
}
type ExecError<Out> = {
  code: string
  message: string
  errno?: int
  syscall?: string
  path?: string
  spawnargs?: string[]
  status?: int
  signal?: string
  stdout?: Out
  stderr?: Out
}
)";

}

Prelude::Prelude(Checker &checker) : checker_(checker) {}

void Prelude::declarePrelude() {
  std::string source = kPrelude.str() + "type HttpStatus =";
  for (int status = 200; status <= 599; ++status) {
    source += (status == 200 ? " " : " | ") + std::to_string(status);
  }
  source += "\n";
  std::unique_ptr<llvm::MemoryBuffer> buffer =
      llvm::MemoryBuffer::getMemBufferCopy(source, "<tz>");
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
