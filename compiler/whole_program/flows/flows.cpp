#include "compiler/whole_program/flows/flows.h"
#include "compiler/whole_program/flow_builtins/flow_builtins.h"
#include "compiler/whole_program/flow_calls/flow_calls.h"
#include "compiler/whole_program/flow_exprs/flow_exprs.h"
#include "compiler/whole_program/flow_stmts/flow_stmts.h"
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

Refs shifted(const Refs &refs, int levels) {
  Refs out;
  for (const Ref &r : refs) {
    out.push_back({r.node, r.off + levels, std::max(r.md - levels, 0)});
  }

  return out;
}

Refs copied(const Refs &refs) {
  Refs out;
  for (const Ref &r : refs) {
    out.push_back({r.node, r.off, std::max(r.md, 1)});
  }

  return out;
}

Refs holding(const Refs &refs) {
  Refs out;
  for (const Ref &r : refs) {
    out.push_back({r.node, r.off - 1, r.md + 1});
  }

  return out;
}

void append(Refs &to, const Refs &from) { to.insert(to.end(), from.begin(), from.end()); }

bool holdsReferences(const Type *type) {
  if (!type) {
    return true;
  }
  type = type->nonOptional();

  return !(type->isNumeric() || type->isBool() || type->isString() || type->isLiteral() ||
           type->isDecimal() || type->isVoid() || type->isUndefined() || type->isNever() ||
           type->isBytes() || type->isDate() || type->isRegExp() || type->isIntl() ||
           type->isCrypto());
}

const Type *ownType(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr->type;
}

Flows::Flows(const llvm::DenseSet<const CastExpr *> &widenings,
             llvm::function_ref<std::vector<Narrowing>(const Type *, const Type *)> narrowings,
             llvm::function_ref<bool(const Type *, const Type *)> convertible)
    : widenings_(widenings), narrowings_(narrowings), convertible_(convertible) {
  flowStmtsPart_ = std::make_unique<FlowStmts>(*this);
  flowCallsPart_ = std::make_unique<FlowCalls>(*this);
  flowBuiltinsPart_ = std::make_unique<FlowBuiltins>(*this);
  flowExprsPart_ = std::make_unique<FlowExprs>(*this);
  unknown_ = node(NodeKind::Unknown, nullptr, "");
  thrown_ = node(NodeKind::Unknown, nullptr, "thrown");
}

void Flows::walkProgram(SourceFile &file) {
  for (Stmt *item : file.items) {
    flowStmts().stmt(item);
  }
  for (FunctionDecl *fn : file.instances) {
    flowStmts().function(fn);
  }
  for (const ValueCall &call : valueCalls_) {
    for (const FunctionDecl *fn : functionValues_) {
      size_t count = fn->params.size();
      bool rest = count > 0 && fn->params.back().rest;
      if (call.args.size() > count && !rest) {
        continue;
      }
      if (!flowCalls().mayCall(call.call->callee ? call.call->callee->type : nullptr, fn)) {
        continue;
      }
      flowCalls().passArguments(call.call, fn, call.args);
      edges[size_t(returned(fn))].push_back({call.result, 0, 0});
    }
  }
}

int Flows::node(NodeKind kind, const void *key, llvm::StringRef name) {
  auto [it, added] =
      ids_.try_emplace(std::make_tuple(int(kind), key, name.str()), int(edges.size()));
  if (added) {
    edges.emplace_back();
  }

  return it->second;
}

int Flows::var(const VarSymbol *v) { return holder(node(NodeKind::Var, v, ""), v->type); }

int Flows::fieldNode(llvm::StringRef name) { return node(NodeKind::Field, nullptr, name); }

int Flows::contents(const Type *type) {
  if (!type) {
    return -1;
  }
  type = type->nonOptional();
  if (type->isArray()) {
    return holder(node(NodeKind::Elements, type->inner(), ""), type->inner());
  }
  if (type->isDict() || type->isPromise()) {
    return node(NodeKind::Values, type, "");
  }

  return -1;
}

int Flows::returned(const FunctionDecl *fn) {
  return holder(node(NodeKind::Return, fn, ""), fn->sym ? fn->sym->ret : nullptr);
}

int Flows::holder(int id, const Type *type) {
  if (size_t(id) >= plain_.size()) {
    plain_.resize(edges.size());
    types.resize(edges.size());
  }
  if (!holdsReferences(type)) {
    plain_[size_t(id)] = true;
  }
  types[size_t(id)] = type;

  return id;
}

void Flows::flow(const Refs &refs, int to) {
  if (size_t(to) < plain_.size() && plain_[size_t(to)]) {
    return;
  }
  for (const Ref &r : refs) {
    edges[size_t(r.node)].push_back({to, r.off, r.md});
  }
}

void Flows::escape(const Refs &refs) { flow(refs, unknown_); }

Flows::~Flows() = default;
}
