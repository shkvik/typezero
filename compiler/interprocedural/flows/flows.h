#pragma once

#include "compiler/semantic/semantic_external.h"
#include "compiler/semantic/semantic_types.h"
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

class FlowStmts;
class FlowCalls;
class FlowBuiltins;
class FlowExprs;

class Flows {
public:
  using NarrowingsOf = llvm::function_ref<std::vector<NarrowedPart>(const Type *, const Type *)>;
  using ConvertibleTo = llvm::function_ref<bool(const Type *, const Type *)>;

  Flows(const WideningStore &wideningStore, NarrowingsOf narrowingsOf, ConvertibleTo convertibleTo);
  ~Flows();

  void walkProgram(SourceFile &file);
  const FlowGraph &getFlowGraph() const;

  FlowStmts &getFlowStmts() const;
  FlowCalls &getFlowCalls() const;
  FlowExprs &getFlowExprs() const;
  FlowBuiltins &getFlowBuiltins() const;

  int getUnknownNode() const;
  int fieldNode(llvm::StringRef name);
  int varNode(const VarSymbol *var);
  int returnNode(const FunctionDecl *fn);
  int contentsNode(const Type *type);
  int findNode(NodeKind kind, const void *key, llvm::StringRef name);

  void escape(const Refs &refs);
  void addFlow(const Refs &refs, int to);
  void addWrite(const Write &write);
  void addConversion(const Conversion &conversion);

  bool isConvertible(const Type *from, const Type *to) const;
  bool isWideningCast(const CastExpr *cast) const;
  std::vector<NarrowedPart> findNarrowings(const Type *from, const Type *to) const;

  void leaveFunction();
  bool isInsideFunction() const;
  void markWalked(const FunctionDecl *fn);
  void enterFunction(const FunctionDecl *fn);
  bool isWalked(const FunctionDecl *fn) const;
  const VarSymbol *findCurrentThis() const;
  const FunctionDecl *getCurrentFunction() const;

  void rememberValueCall(const ValueCall &call);
  void markRuntimeFunction(const VarSymbol *var);
  bool isRuntimeFunction(const VarSymbol *var) const;
  void rememberFunctionValue(const FunctionDecl *fn);

  static Refs copied(const Refs &refs);
  static Refs holding(const Refs &refs);
  static void append(Refs &to, const Refs &from);
  static Refs shifted(const Refs &refs, int levels);
  static const Type *ownType(const Expr *expr);
  static bool holdsReferences(const Type *type);

private:
  int markHolder(int id, const Type *type);

  FlowGraph graph;
  NarrowingsOf narrowings;
  ConvertibleTo convertible;
  std::vector<bool> plainNodes;
  std::vector<ValueCall> valueCalls;
  const WideningStore &widenings;
  std::set<const FunctionDecl *> walked;
  std::unique_ptr<FlowStmts> flowStmts;
  std::unique_ptr<FlowCalls> flowCalls;
  std::unique_ptr<FlowExprs> flowExprs;
  std::set<const FunctionDecl *> functionSet;
  std::unique_ptr<FlowBuiltins> flowBuiltins;
  std::set<const VarSymbol *> runtimeFunctions;
  std::vector<const FunctionDecl *> functions;
  std::vector<const FunctionDecl *> functionValues;
  std::map<std::tuple<int, const void *, std::string>, int> ids;
};

}
