#pragma once

#include "compiler/program/ast/node/node.h"

namespace tz {

class Type;

class Expr : public Node {
public:
  enum class Kind {
    IntLit,
    FloatLit,
    BoolLit,
    StringLit,
    Undefined,
    Template,
    ArrayLit,
    ObjectLit,
    Name,
    Member,
    Index,
    Unary,
    Binary,
    Assign,
    Update,
    Call,
    New,
    Conditional,
    Cast,
    Sequence,
    OptionalChain,
    Function,
    Yield,
    RegexLit,
    Await,
    Class,
  };

  Kind kind() const { return kind_; }
  llvm::SMLoc loc() const { return loc_; }

  const Type *type = nullptr;

protected:
  Expr(Kind kind, llvm::SMLoc loc) : kind_(kind), loc_(loc) {}

private:
  Kind kind_;
  llvm::SMLoc loc_;
};

}
