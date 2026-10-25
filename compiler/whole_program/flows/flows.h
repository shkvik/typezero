#pragma once

#include <memory>
#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace tz {

// The program as a graph of flows between references, for WideWrites.

// A write that the wider type allows and the value's own type does not:
// `depth` levels inside the value (0: the value itself), to the field `name`
// (or to an element of an array), of anything but a `narrow` (none: of
// anything, the field is read-only in the value's type). `owner` is the type
// of the value's part there.
struct Narrowing {
  int depth;
  bool element;
  std::string name;
  const Type *narrow;
  const Type *owner;
};

// Where values flow: a variable, the fields of a name, the elements of
// arrays of a type, the keys and values of a kind of dictionary, what a
// function returns, what a call of a function value gives, a conversion, or
// a place the check does not follow.
enum class NodeKind { Var, Field, Elements, Values, Return, CallResult, Conversion, Unknown };

// A value that holds what `node` holds `off` levels deeper, from its own
// level `md` on: a variable is (var, 0, 0); a field of it (var, 1, 0); a new
// array of its elements (var, 0, 1), whose elements are its elements but
// which is not the array itself.
struct Ref {
  int node;
  int off;
  int md;
};

using Refs = std::vector<Ref>;

// What flows from a node into another: `to` at level j holds what `from`
// holds at level j + off, from j = md on.
struct Edge {
  int to;
  int off;
  int md;
};

// A write to a field or an element of what `target` holds.
struct Write {
  Refs target;
  bool element;
  std::string name;    // empty for an element; "*" for a field not known
  const Type *value;   // what is written, before it is converted; null for any
  const Type *through; // the type of the object written to
  llvm::SMLoc loc;
};

// A conversion that widens what code may write: from where it is, the
// writes the value's type does not allow.
struct Conversion {
  int node;
  const Type *from;
  const Type *to;
  llvm::SMLoc loc;
  std::vector<Narrowing> narrowings;
  // `super(...)`: the node of the `this` of the constructor of the class up
  // the chain, which sets the fields of `this` as the class's own
  // initializers do after it; -1 for another conversion.
  int initializes = -1;
  std::string role;     // how the value goes there, for the message
  llvm::StringRef name; // of the variable converted, if it is one
};

Refs shifted(const Refs &refs, int levels);

// A new container of what `refs` hold: a copy of an array, `[...xs]`.
Refs copied(const Refs &refs);

// A new container that holds the values `refs` hold: `[x]`, `{ f: x }`.
Refs holding(const Refs &refs);

void append(Refs &to, const Refs &from);

// Whether values of `type` may hold references to what a conversion widened,
// or be one: not numbers, strings or booleans, nor the objects of the runtime
// that hold only those (bytes, dates, regular expressions, Intl and crypto
// objects), which no conversion widens.
bool holdsReferences(const Type *type);

// The type of a value before the conversions Sema added to it.
const Type *ownType(const Expr *expr);

// The program as a graph of flows (see the top of the file).
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
  // By node: the type of the variable or of what the function returns, which
  // the value a node holds is one of; null for other nodes.
  std::vector<const Type *> types;
  // What `throw` throws and what a promise is rejected with.
  int thrown_ = 0;
private:
  int node(NodeKind kind, const void *key, llvm::StringRef name);
  int var(const VarSymbol *v);
  int fieldNode(llvm::StringRef name);
  // What containers of `type` hold: the elements of arrays of a type, the
  // keys and values of a kind of dictionary, what promises of a type give;
  // -1 for other types.
  int contents(const Type *type);
  int returned(const FunctionDecl *fn);
  // A node of a type that holds no references (a string, a number) holds
  // nothing a conversion widened, whatever flows to it.
  int holder(int id, const Type *type);
  void flow(const Refs &refs, int to);
  void escape(const Refs &refs);
  const llvm::DenseSet<const CastExpr *> &widenings_;
  llvm::function_ref<std::vector<Narrowing>(const Type *, const Type *)> narrowings_;
  llvm::function_ref<bool(const Type *, const Type *)> convertible_;
  std::map<std::tuple<int, const void *, std::string>, int> ids_;
  std::set<const FunctionDecl *> walked_;
  std::vector<const FunctionDecl *> functions_;
  // The functions the program uses as values, and the calls of values.
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
  std::vector<bool> plain_; // by node: of a type that holds no references
  // Whether the program makes a Result of what is thrown or rejected.
  bool resultOfErrors_ = false;

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

} // namespace tz
