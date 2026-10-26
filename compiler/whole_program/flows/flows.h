#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace tz {

struct Narrowing {
  int depth;
  bool element;
  std::string name;
  const Type *narrow;
  const Type *owner;
};

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
  std::vector<Narrowing> narrowings;
  int initializes = -1;
  std::string role;
  llvm::StringRef name;
};

Refs shifted(const Refs &refs, int levels);

Refs copied(const Refs &refs);

Refs holding(const Refs &refs);

void append(Refs &to, const Refs &from);

bool holdsReferences(const Type *type);

const Type *ownType(const Expr *expr);

class FlowStmts;
class FlowCalls;
class FlowBuiltins;
class FlowExprs;

class Flows {
public:
  Flows(const llvm::DenseSet<const CastExpr *> &widenings,
        llvm::function_ref<std::vector<Narrowing>(const Type *, const Type *)> narrowings,
        llvm::function_ref<bool(const Type *, const Type *)> convertible);
  void walkProgram(SourceFile &file);
  std::vector<std::vector<Edge>> edges;
  std::vector<Write> writes;
  std::vector<Conversion> conversions;
  int unknown_ = 0;
  std::vector<const Type *> types;
  int thrown_ = 0;

private:
  int node(NodeKind kind, const void *key, llvm::StringRef name);
  int var(const VarSymbol *v);
  int fieldNode(llvm::StringRef name);
  int contents(const Type *type);
  int returned(const FunctionDecl *fn);
  int holder(int id, const Type *type);
  void flow(const Refs &refs, int to);
  void escape(const Refs &refs);
  const llvm::DenseSet<const CastExpr *> &widenings_;
  llvm::function_ref<std::vector<Narrowing>(const Type *, const Type *)> narrowings_;
  llvm::function_ref<bool(const Type *, const Type *)> convertible_;
  std::map<std::tuple<int, const void *, std::string>, int> ids_;
  std::set<const FunctionDecl *> walked_;
  std::vector<const FunctionDecl *> functions_;
  struct ValueCall {
    const CallExpr *call;
    std::vector<Refs> args;
    int result;
  };
  std::vector<const FunctionDecl *> functionValues_;
  std::set<const FunctionDecl *> functionSet_;
  std::vector<ValueCall> valueCalls_;
  std::set<const VarSymbol *> runtimeFunctions_;
  std::set<const VarSymbol *> runtimeRejects_;
  std::vector<bool> plain_;

public:
  ~Flows();

  FlowStmts &flowStmts() const { return *flowStmtsPart_; }
  FlowCalls &flowCalls() const { return *flowCallsPart_; }
  FlowBuiltins &flowBuiltins() const { return *flowBuiltinsPart_; }
  FlowExprs &flowExprs() const { return *flowExprsPart_; }

private:
  friend class FlowStmts;
  friend class FlowCalls;
  friend class FlowBuiltins;
  friend class FlowExprs;
  std::unique_ptr<FlowStmts> flowStmtsPart_;
  std::unique_ptr<FlowCalls> flowCallsPart_;
  std::unique_ptr<FlowBuiltins> flowBuiltinsPart_;
  std::unique_ptr<FlowExprs> flowExprsPart_;
};

}
