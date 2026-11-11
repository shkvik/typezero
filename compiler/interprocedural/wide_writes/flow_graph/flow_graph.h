#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_types.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <string>
#include <vector>

namespace tz {

enum class NodeKind { Var, Field, Elements, Values, Return, CallResult, Conversion, Unknown };

struct Ref {
  int node;
  int off;
  int md;
};

using Refs = std::vector<Ref>;

struct Edge {
  int to;
  int off;
  int md;
};

struct Write {
  Refs target;
  bool element;
  std::string name;
  const Type *value;
  const Type *through;
  llvm::SMLoc loc;
};

struct Conversion {
  int node;
  const Type *from;
  const Type *to;
  llvm::SMLoc loc;
  std::vector<NarrowedPart> narrowings;
  int initializes = -1;
  std::string role;
  llvm::StringRef name;
};

struct ValueCall {
  const CallExpr *call;
  std::vector<Refs> args;
  int result;
};

struct FlowGraph {
  std::vector<std::vector<Edge>> edges;
  std::vector<Write> writes;
  std::vector<Conversion> conversions;
  std::vector<const Type *> types;
  int unknownNode = 0;
};

}
