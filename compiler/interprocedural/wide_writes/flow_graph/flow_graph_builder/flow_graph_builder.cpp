#include "compiler/interprocedural/wide_writes/flow_graph/flow_graph_builder/flow_graph_builder.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_builtins/flow_builtins.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_calls/flow_calls.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_exprs/flow_exprs.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_stmts/flow_stmts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

using llvm::isa;

using llvm::dyn_cast_or_null;

FlowGraphBuilder::FlowGraphBuilder(const WideningStore &wideningStore, NarrowingsOf narrowingsOf,
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

FlowGraphBuilder::~FlowGraphBuilder() = default;

FlowGraph FlowGraphBuilder::buildGraph(SourceFile &file) {
  for (Stmt *item : file.items) {
    this->walk(FlowRun::WalkStmt{item});
  }
  for (FunctionDecl *fn : file.inventory.instances) {
    this->walk(FlowRun::WalkFunction{fn});
  }
  for (const ValueCall &call : this->valueCalls) {
    for (const FunctionDecl *fn : this->functionValues) {
      size_t count = fn->params.size();
      bool hasRest = count > 0 && fn->params.back().rest();
      if (call.args.size() > count && !hasRest) {
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

  return std::move(this->graph);
}

void FlowGraphBuilder::walk(FlowRun::Step first) {
  auto run = FlowRun{.steps = {std::move(first)}, .answers = {}};
  while (!run.steps.empty()) {
    auto step = std::move(run.steps.back());
    run.steps.pop_back();
    this->applyStep(step, run);
  }
}

void FlowGraphBuilder::applyStep(const FlowRun::Step &step, FlowRun &run) {
  FlowStmts &stmts = this->getFlowStmts();
  FlowExprs &exprs = this->getFlowExprs();
  switch (step.kind()) {
    case FlowRun::StepKind::WALK_STMT:
      return stmts.startStmt(step.as<FlowRun::WalkStmt>()->stmt, run);
    case FlowRun::StepKind::WALK_FUNCTION:
      return stmts.startFunction(step.as<FlowRun::WalkFunction>()->fn, run);
    case FlowRun::StepKind::WALK_CLASS:
      return stmts.startClass(step.as<FlowRun::WalkClass>()->cls, run);
    case FlowRun::StepKind::LEAVE_FUNCTION:
      return this->leaveFunction();
    case FlowRun::StepKind::WALK_EXPR:
      return exprs.startExpr(step.as<FlowRun::WalkExpr>()->expr, run);
    case FlowRun::StepKind::DROP_ANSWER:
      return run.answers.pop_back();
    case FlowRun::StepKind::EMPTY_ANSWER:
      return run.answers.push_back(Refs());
    case FlowRun::StepKind::BIND_VAR:
      return stmts.bindVar(step.as<FlowRun::BindVar>()->var, run);
    case FlowRun::StepKind::BIND_FIELD:
      return stmts.bindField(step.as<FlowRun::BindField>()->field, run);
    case FlowRun::StepKind::BIND_FOR_OF:
      return stmts.bindForOf(step.as<FlowRun::BindForOf>()->loop, run);
    case FlowRun::StepKind::FINISH_RETURN:
      return stmts.finishReturn(run);
    case FlowRun::StepKind::FINISH_ARRAY:
      return exprs.finishArray(step.as<FlowRun::FinishArray>()->literal, run);
    case FlowRun::StepKind::FINISH_PROPERTY:
      return exprs.finishProperty(step.as<FlowRun::FinishProperty>()->property, run);
    case FlowRun::StepKind::PASS_OBJECT_THIS:
      return exprs.passObjectThis(step.as<FlowRun::PassObjectThis>()->fn, run);
    case FlowRun::StepKind::FINISH_FILTER:
      return exprs.finishFilter(step.as<FlowRun::FinishFilter>()->expr, run);
    case FlowRun::StepKind::FINISH_MEMBER:
      return exprs.finishMember(step.as<FlowRun::FinishMember>()->member, run);
    case FlowRun::StepKind::FINISH_INDEX:
      return exprs.finishIndex(step.as<FlowRun::FinishIndex>()->index, run);
    case FlowRun::StepKind::FINISH_JOIN:
      return exprs.finishJoin(step.as<FlowRun::FinishJoin>()->expr, run);
    case FlowRun::StepKind::START_ASSIGN:
      return stmts.startAssign(step.as<FlowRun::StartAssign>()->assign, run);
    case FlowRun::StepKind::FINISH_ASSIGN:
      return stmts.finishAssign(step.as<FlowRun::FinishAssign>()->assign, run);
    case FlowRun::StepKind::FINISH_CALL:
      return this->getFlowCalls().finishCall(step.as<FlowRun::FinishCall>()->call, run);
    case FlowRun::StepKind::FINISH_NEW:
      return exprs.finishNew(step.as<FlowRun::FinishNew>()->create, run);
    case FlowRun::StepKind::FINISH_CAST:
      return exprs.finishCast(step.as<FlowRun::FinishCast>()->conversion, run);
    case FlowRun::StepKind::FINISH_FUNCTION_EXPR:
      return exprs.finishFunctionExpr(step.as<FlowRun::FinishFunctionExpr>()->fn, run);
    case FlowRun::StepKind::FINISH_YIELD:
      return exprs.finishYield(step.as<FlowRun::FinishYield>()->yield, run);
    case FlowRun::StepKind::FINISH_AWAIT:
      return exprs.finishAwait(step.as<FlowRun::FinishAwait>()->await, run);
  }
}

void FlowGraphBuilder::pushSteps(FlowRun &run, const std::vector<FlowRun::Step> &parts) const {
  run.steps.insert(run.steps.end(), parts.rbegin(), parts.rend());
}

void FlowGraphBuilder::pushAnswer(FlowRun &run, const Expr *expr, Refs refs) const {
  bool isPlain =
      expr != nullptr && expr->type != nullptr && !FlowGraphBuilder::holdsReferences(expr->type);
  if (isPlain) {
    refs.clear();
  }
  run.answers.push_back(std::move(refs));
}

Refs FlowGraphBuilder::takeAnswer(FlowRun &run) const {
  Refs answer = std::move(run.answers.back());
  run.answers.pop_back();

  return answer;
}

std::vector<Refs> FlowGraphBuilder::takeAnswers(FlowRun &run, size_t count) const {
  auto taken = std::vector<Refs>(count);
  for (size_t i = count; i > 0; --i) {
    taken[i - 1] = this->takeAnswer(run);
  }

  return taken;
}

FlowStmts &FlowGraphBuilder::getFlowStmts() const {
  return *this->flowStmts;
}

FlowCalls &FlowGraphBuilder::getFlowCalls() const {
  return *this->flowCalls;
}

FlowExprs &FlowGraphBuilder::getFlowExprs() const {
  return *this->flowExprs;
}

FlowBuiltins &FlowGraphBuilder::getFlowBuiltins() const {
  return *this->flowBuiltins;
}

int FlowGraphBuilder::getUnknownNode() const {
  return this->graph.unknownNode;
}

int FlowGraphBuilder::findNode(NodeKind kind, const void *key, llvm::StringRef name) {
  std::tuple<int, const void *, std::string> id = std::make_tuple(int(kind), key, name.str());
  auto [it, isAdded] = this->ids.try_emplace(id, int(this->graph.edges.size()));
  if (isAdded) {
    this->graph.edges.emplace_back();
  }

  return it->second;
}

int FlowGraphBuilder::varNode(const VarSymbol *var) {
  int id = this->findNode(NodeKind::Var, var, "");

  return this->markHolder(id, var->type);
}

int FlowGraphBuilder::fieldNode(llvm::StringRef name) {
  return this->findNode(NodeKind::Field, nullptr, name);
}

int FlowGraphBuilder::contentsNode(const Type *type) {
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

int FlowGraphBuilder::returnNode(const FunctionDecl *fn) {
  int id = this->findNode(NodeKind::Return, fn, "");

  return this->markHolder(id, fn->sym ? fn->sym->getKnownReturnType() : nullptr);
}

int FlowGraphBuilder::markHolder(int id, const Type *type) {
  if (size_t(id) >= this->plainNodes.size()) {
    this->plainNodes.resize(this->graph.edges.size());
    this->graph.types.resize(this->graph.edges.size());
  }
  if (!FlowGraphBuilder::holdsReferences(type)) {
    this->plainNodes[size_t(id)] = true;
  }
  this->graph.types[size_t(id)] = type;

  return id;
}

void FlowGraphBuilder::addFlow(const Refs &refs, int to) {
  if (size_t(to) < this->plainNodes.size() && this->plainNodes[size_t(to)]) {
    return;
  }
  for (const Ref &r : refs) {
    this->graph.edges[size_t(r.node)].push_back({to, r.off, r.md});
  }
}

void FlowGraphBuilder::escape(const Refs &refs) {
  this->addFlow(refs, this->graph.unknownNode);
}

void FlowGraphBuilder::addWrite(const Write &write) {
  this->graph.writes.push_back(write);
}

void FlowGraphBuilder::addConversion(const Conversion &conversion) {
  this->graph.conversions.push_back(conversion);
}

bool FlowGraphBuilder::isConvertible(const Type *from, const Type *to) const {
  return this->convertible(from, to);
}

bool FlowGraphBuilder::isWideningCast(const CastExpr *cast) const {
  return this->widenings.isWideningCast(cast);
}

std::vector<NarrowedPart> FlowGraphBuilder::findNarrowings(const Type *from, const Type *to) const {
  return this->narrowings(from, to);
}

bool FlowGraphBuilder::isWalked(const FunctionDecl *fn) const {
  return this->walked.count(fn) > 0;
}

void FlowGraphBuilder::markWalked(const FunctionDecl *fn) {
  this->walked.insert(fn);
}

void FlowGraphBuilder::enterFunction(const FunctionDecl *fn) {
  this->functions.push_back(fn);
}

void FlowGraphBuilder::leaveFunction() {
  this->functions.pop_back();
}

bool FlowGraphBuilder::isInsideFunction() const {
  return !this->functions.empty();
}

const FunctionDecl *FlowGraphBuilder::getCurrentFunction() const {
  return this->functions.empty() ? nullptr : this->functions.back();
}

const VarSymbol *FlowGraphBuilder::findCurrentThis() const {
  for (const FunctionDecl *fn : llvm::reverse(this->functions)) {
    if (fn->thisVar()) {
      return fn->thisVar();
    }
  }

  return nullptr;
}

void FlowGraphBuilder::rememberFunctionValue(const FunctionDecl *fn) {
  if (fn == nullptr) {
    return;
  }
  auto [_, isInserted] = this->functionSet.insert(fn);
  if (isInserted) {
    this->functionValues.push_back(fn);
  }
}

void FlowGraphBuilder::rememberValueCall(const ValueCall &call) {
  this->valueCalls.push_back(call);
}

void FlowGraphBuilder::markRuntimeFunction(const VarSymbol *var) {
  this->runtimeFunctions.insert(var);
}

bool FlowGraphBuilder::isRuntimeFunction(const VarSymbol *var) const {
  return this->runtimeFunctions.count(var) > 0;
}

Refs FlowGraphBuilder::shifted(const Refs &refs, int levels) {
  Refs out;
  for (const Ref &r : refs) {
    out.push_back({r.node, r.off + levels, std::max(r.md - levels, 0)});
  }

  return out;
}

Refs FlowGraphBuilder::copied(const Refs &refs) {
  Refs out;
  for (const Ref &r : refs) {
    out.push_back({r.node, r.off, std::max(r.md, 1)});
  }

  return out;
}

Refs FlowGraphBuilder::holding(const Refs &refs) {
  Refs out;
  for (const Ref &r : refs) {
    out.push_back({r.node, r.off - 1, r.md + 1});
  }

  return out;
}

void FlowGraphBuilder::append(Refs &to, const Refs &from) {
  to.insert(to.end(), from.begin(), from.end());
}

bool FlowGraphBuilder::holdsReferences(const Type *type) {
  if (!type) {
    return true;
  }
  type = type->nonOptional();

  return !(type->isNumeric() || type->isBool() || type->isString() || type->isLiteral() ||
           type->isDecimal() || type->isVoid() || type->isUndefined() || type->isNever() ||
           type->isBytes() || type->isDate() || type->isRegExp() || type->isIntl() ||
           type->isCrypto());
}

const Type *FlowGraphBuilder::ownType(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr->type;
}

}
