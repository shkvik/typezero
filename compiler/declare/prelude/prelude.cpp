#include "compiler/declare/prelude/prelude.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The utilities that are mapped types, as TypeScript declares them (Omit
// takes keys of T only: a key that is not one is a mistake).
constexpr llvm::StringLiteral kPrelude = R"(type Partial<T> = { [K in keyof T]?: T[K] }
type Required<T> = { [K in keyof T]-?: T[K] }
type Readonly<T> = { readonly [K in keyof T]: T[K] }
type Pick<T, K extends keyof T> = { [P in K]: T[P] }
type Omit<T, K extends keyof T> = { [P in Exclude<keyof T, K>]: T[P] }
type Awaited<T> = T extends Promise<infer U> ? Awaited<U> : T
)";

} // namespace

Prelude::Prelude(Sema &sema) : sema_(sema) {}

void Prelude::declarePrelude() {
  std::unique_ptr<llvm::MemoryBuffer> buffer = llvm::MemoryBuffer::getMemBuffer(kPrelude, "<tz>");
  llvm::StringRef text = buffer->getBuffer();
  sema_.diags_.sourceMgr().AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  Lexer lexer(text, sema_.diags_);
  Parser parser(lexer, sema_.ctx_, sema_.diags_);
  SourceFile prelude = parser.parseSourceFile();
  for (Stmt *item : prelude.items) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(item)) {
      sema_.builtinAliases_[alias->name] = alias;
    }
  }
}

} // namespace tz
