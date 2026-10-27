#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/node/node.h"

namespace tz {

class Stmt : public Node {
public:
  enum class Kind {
    Block,
    VarDecl,
    Function,
    Expr,
    If,
    While,
    DoWhile,
    For,
    ForOf,
    Switch,
    Labeled,
    Return,
    Break,
    Continue,
    Empty,
    TypeAlias,
    Enum,
    Debugger,
    Class,
  };

  Kind kind() const { return kind_; }
  llvm::SMLoc loc() const { return loc_; }

protected:
  Stmt(Kind kind, llvm::SMLoc loc) : kind_(kind), loc_(loc) {}

private:
  Kind kind_;
  llvm::SMLoc loc_;
};

}
