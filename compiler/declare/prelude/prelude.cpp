#include "compiler/declare/prelude/prelude.h"
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
type JsonError = { code: "bad_json" | "bad_field"; message: string; path: string }
type UriError = { code: "bad_uri"; message: string }
type RegexError = { code: "bad_regex"; message: string }
type UrlError = { code: "bad_url"; message: string; input: string }
type DateError = { code: "bad_date"; message: string; text: string }
)";

}

Prelude::Prelude(Checker &checker) : checker_(checker) {}

void Prelude::declarePrelude() {
  std::unique_ptr<llvm::MemoryBuffer> buffer = llvm::MemoryBuffer::getMemBuffer(kPrelude, "<tz>");
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

}
