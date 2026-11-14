#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/source_file/source_file.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

namespace tz {

class LiteralContext {
public:
  explicit LiteralContext(const SourceFile &loaded);

  void markPrechecked(const Expr *expr);
  void addFieldName(llvm::StringRef name);
  void forgetPrechecked(const Expr *expr);
  void markPairLiteral(const Expr *literal);
  void forgetPairLiteral(const Expr *literal);


  bool isPrechecked(const Expr *expr) const;
  bool hasFieldName(llvm::StringRef name) const;
  bool isPairLiteral(const Expr *literal) const;

private:
  const SourceFile &file;
  llvm::StringSet<> fieldNames;
  llvm::SmallPtrSet<const Expr *, 4> prechecked;
  llvm::SmallPtrSet<const Expr *, 4> pairLiterals;
};

}
