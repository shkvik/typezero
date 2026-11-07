#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "compiler/pipeline/check_program/whole_program/flow_builtins/flow_builtins.h"
#include "compiler/pipeline/check_program/whole_program/flow_calls/flow_calls.h"
#include "compiler/pipeline/check_program/whole_program/flow_exprs/flow_exprs.h"
#include "compiler/pipeline/check_program/whole_program/flow_stmts/flow_stmts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

using llvm::isa;

using llvm::dyn_cast_or_null;

Flows::Flows(const WideningStore &wideningStore, NarrowingsOf narrowingsOf,
             ConvertibleTo convertibleTo)
    : narrowings(narrowingsOf),
      convertible(convertibleTo),
      widenings(wideningStore) {
  this->flowStmts = std::make_unique<FlowStmts>(*this);
  this->flowCalls = std::make_unique<FlowCalls>(*this);
  this->flowExprs = std::make_unique<FlowExprs>(*this);
  this->flowBuiltins = std::make_unique<FlowBuiltins>(*this);
  this->graph.unknownNode = this->findNode(NodeKind::Unknown, nullptr, "");
}

Flows::~Flows() = default;

void Flows::walkProgram(SourceFile &file) {
  for (Stmt *item : file.items) {
    this->getFlowStmts().stmt(item);
  }
  for (FunctionDecl *fn : file.instances) {
    this->getFlowStmts().function(fn);
  }
  for (const ValueCall &call : this->valueCalls) {
    for (const FunctionDecl *fn : this->functionValues) {
      size_t count = fn->params.size();
      bool rest = count > 0 && fn->params.back().rest();
      if (call.args.size() > count && !rest) {
        continue;
      }
      const Type *calleeType = call.call->callee ? call.call->callee->type : nullptr;
      if (!this->getFlowCalls().mayCall(calleeType, fn)) {
        continue;
      }
      this->getFlowCalls().passArguments(call.call, fn, call.args);
      int returned = this->returnNode(fn);
      this->graph.edges[size_t(returned)].push_back({call.result, 0, 0});
    }
  }
}

const FlowGraph &Flows::getFlowGraph() const {
  return this->graph;
}

FlowStmts &Flows::getFlowStmts() const {
  return *this->flowStmts;
}

FlowCalls &Flows::getFlowCalls() const {
  return *this->flowCalls;
}

FlowExprs &Flows::getFlowExprs() const {
  return *this->flowExprs;
}

FlowBuiltins &Flows::getFlowBuiltins() const {
  return *this->flowBuiltins;
}

int Flows::getUnknownNode() const {
  return this->graph.unknownNode;
}

int Flows::findNode(NodeKind kind, const void *key, llvm::StringRef name) {
  std::tuple<int, const void *, std::string> id = std::make_tuple(int(kind), key, name.str());
  auto [it, added] = this->ids.try_emplace(id, int(this->graph.edges.size()));
  if (added) {
    this->graph.edges.emplace_back();
  }

  return it->second;
}

int Flows::varNode(const VarSymbol *var) {
  int id = this->findNode(NodeKind::Var, var, "");

  return this->markHolder(id, var->type);
}

int Flows::fieldNode(llvm::StringRef name) {
  return this->findNode(NodeKind::Field, nullptr, name);
}

int Flows::contentsNode(const Type *type) {
  if (!type) {
    return -1;
  }
  type = type->nonOptional();
  if (type->isArray()) {
    int id = this->findNode(NodeKind::Elements, type->inner(), "");

    return this->markHolder(id, type->inner());
  }
  if (type->isDict() || type->isPromise()) {
    return this->findNode(NodeKind::Values, type, "");
  }

  return -1;
}

int Flows::returnNode(const FunctionDecl *fn) {
  int id = this->findNode(NodeKind::Return, fn, "");

  return this->markHolder(id, fn->sym ? fn->sym->ret : nullptr);
}

int Flows::markHolder(int id, const Type *type) {
  if (size_t(id) >= this->plainNodes.size()) {
    this->plainNodes.resize(this->graph.edges.size());
    this->graph.types.resize(this->graph.edges.size());
  }
  if (!Flows::holdsReferences(type)) {
    this->plainNodes[size_t(id)] = true;
  }
  this->graph.types[size_t(id)] = type;

  return id;
}

void Flows::addFlow(const Refs &refs, int to) {
  if (size_t(to) < this->plainNodes.size() && this->plainNodes[size_t(to)]) {
    return;
  }
  for (const Ref &r : refs) {
    this->graph.edges[size_t(r.node)].push_back({to, r.off, r.md});
  }
}

void Flows::escape(const Refs &refs) {
  this->addFlow(refs, this->graph.unknownNode);
}

void Flows::addWrite(const Write &write) {
  this->graph.writes.push_back(write);
}

void Flows::addConversion(const Conversion &conversion) {
  this->graph.conversions.push_back(conversion);
}

bool Flows::isConvertible(const Type *from, const Type *to) const {
  return this->convertible(from, to);
}

bool Flows::isWideningCast(const CastExpr *cast) const {
  return this->widenings.isWideningCast(cast);
}

std::vector<Narrowing> Flows::findNarrowings(const Type *from, const Type *to) const {
  return this->narrowings(from, to);
}

bool Flows::isWalked(const FunctionDecl *fn) const {
  return this->walked.count(fn) > 0;
}

void Flows::markWalked(const FunctionDecl *fn) {
  this->walked.insert(fn);
}

void Flows::enterFunction(const FunctionDecl *fn) {
  this->functions.push_back(fn);
}

void Flows::leaveFunction() {
  this->functions.pop_back();
}

bool Flows::isInsideFunction() const {
  return !this->functions.empty();
}

const FunctionDecl *Flows::getCurrentFunction() const {
  return this->functions.empty() ? nullptr : this->functions.back();
}

const VarSymbol *Flows::findCurrentThis() const {
  for (const FunctionDecl *fn : llvm::reverse(this->functions)) {
    if (fn->thisVar()) {
      return fn->thisVar();
    }
  }

  return nullptr;
}

void Flows::rememberFunctionValue(const FunctionDecl *fn) {
  if (fn && this->functionSet.insert(fn).second) {
    this->functionValues.push_back(fn);
  }
}

void Flows::rememberValueCall(const ValueCall &call) {
  this->valueCalls.push_back(call);
}

void Flows::markRuntimeFunction(const VarSymbol *var) {
  this->runtimeFunctions.insert(var);
}

bool Flows::isRuntimeFunction(const VarSymbol *var) const {
  return this->runtimeFunctions.count(var) > 0;
}

Refs Flows::shifted(const Refs &refs, int levels) {
  Refs out;
  for (const Ref &r : refs) {
    out.push_back({r.node, r.off + levels, std::max(r.md - levels, 0)});
  }

  return out;
}

Refs Flows::copied(const Refs &refs) {
  Refs out;
  for (const Ref &r : refs) {
    out.push_back({r.node, r.off, std::max(r.md, 1)});
  }

  return out;
}

Refs Flows::holding(const Refs &refs) {
  Refs out;
  for (const Ref &r : refs) {
    out.push_back({r.node, r.off - 1, r.md + 1});
  }

  return out;
}

void Flows::append(Refs &to, const Refs &from) {
  to.insert(to.end(), from.begin(), from.end());
}

bool Flows::holdsReferences(const Type *type) {
  if (!type) {
    return true;
  }
  type = type->nonOptional();

  return !(type->isNumeric() || type->isBool() || type->isString() || type->isLiteral() ||
           type->isDecimal() || type->isVoid() || type->isUndefined() || type->isNever() ||
           type->isBytes() || type->isDate() || type->isRegExp() || type->isIntl() ||
           type->isCrypto());
}

const Type *Flows::ownType(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr->type;
}

}
