#include "compiler/interprocedural/wide_writes/flow_graph/flow_exprs/flow_exprs.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_calls/flow_calls.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_stmts/flow_stmts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <tuple>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

FlowExprs::FlowExprs(FlowGraphBuilder &owner)
    : builder(owner) {}

const Expr *FlowExprs::ownExpr(const Expr *e) const {
  while (auto *conv = llvm::dyn_cast<CastExpr>(e)) {
    e = conv->operand;
  }

  return e;
}

void FlowExprs::startExpr(Expr *e, FlowRun &run) {
  if (!e) {
    return run.answers.push_back(Refs());
  }
  auto parts = this->exprParts(e, run);
  this->builder.pushSteps(run, parts);
}

std::vector<FlowRun::Step> FlowExprs::exprParts(Expr *e, FlowRun &run) {
  switch (e->kind()) {
    case Expr::Kind::IntLit:
    case Expr::Kind::FloatLit:
    case Expr::Kind::BoolLit:
    case Expr::Kind::StringLit:
    case Expr::Kind::Undefined:
    case Expr::Kind::RegexLit:
    case Expr::Kind::Class:
      this->builder.pushAnswer(run, e, Refs());
      return {};
    case Expr::Kind::Template:
      return this->templateParts(llvm::cast<TemplateExpr>(e));
    case Expr::Kind::ArrayLit:
      return this->arrayParts(llvm::cast<ArrayLitExpr>(e));
    case Expr::Kind::ObjectLit:
      return this->objectParts(llvm::cast<ObjectLitExpr>(e), run);
    case Expr::Kind::Name: {
      auto *n = llvm::cast<NameExpr>(e);
      if (n->func()) {
        this->builder.rememberFunctionValue(n->func()->decl);
      }
      Refs refs = this->builder.getFlowStmts().name(n);
      this->builder.pushAnswer(run, e, refs);
      return {};
    }
    case Expr::Kind::Member: {
      auto *member = llvm::cast<MemberExpr>(e);
      return {FlowRun::WalkExpr{member->object}, FlowRun::FinishMember{member}};
    }
    case Expr::Kind::Index: {
      auto *index = llvm::cast<IndexExpr>(e);
      return {FlowRun::WalkExpr{index->object}, FlowRun::WalkExpr{index->index},
              FlowRun::FinishIndex{index}};
    }
    case Expr::Kind::Unary:
      return {FlowRun::WalkExpr{llvm::cast<UnaryExpr>(e)->operand}, FlowRun::DropAnswer{},
              FlowRun::EmptyAnswer{}};
    case Expr::Kind::Binary:
      return this->binaryParts(llvm::cast<BinaryExpr>(e));
    case Expr::Kind::Assign: {
      auto *a = llvm::cast<AssignExpr>(e);
      auto assign = FlowRun::AssignTarget{
          .target = a->target,
          .valueExpr = a->value,
          .isCompound = a->op.has_value(),
          .isUpdate = false,
          .whole = a,
      };
      return {FlowRun::WalkExpr{a->value}, FlowRun::StartAssign{assign}};
    }
    case Expr::Kind::Update: {
      auto *update = llvm::cast<UpdateExpr>(e);
      run.answers.push_back(Refs());
      auto assign = FlowRun::AssignTarget{
          .target = update->target,
          .valueExpr = nullptr,
          .isCompound = true,
          .isUpdate = true,
          .whole = update,
      };
      return {FlowRun::StartAssign{assign}};
    }
    case Expr::Kind::Call:
      return this->builder.getFlowCalls().callParts(llvm::cast<CallExpr>(e), run);
    case Expr::Kind::New:
      return this->newParts(llvm::cast<NewExpr>(e));
    case Expr::Kind::Conditional: {
      auto *c = llvm::cast<ConditionalExpr>(e);
      return {FlowRun::WalkExpr{c->cond}, FlowRun::DropAnswer{}, FlowRun::WalkExpr{c->thenExpr},
              FlowRun::WalkExpr{c->elseExpr}, FlowRun::FinishJoin{c}};
    }
    case Expr::Kind::Cast: {
      auto *conv = llvm::cast<CastExpr>(e);
      return {FlowRun::WalkExpr{conv->operand}, FlowRun::FinishCast{conv}};
    }
    case Expr::Kind::Sequence:
      return this->sequenceParts(llvm::cast<SequenceExpr>(e), run);
    case Expr::Kind::OptionalChain:
      return {FlowRun::WalkExpr{llvm::cast<OptionalChainExpr>(e)->chain}, FlowRun::FinishFilter{e}};
    case Expr::Kind::Function: {
      FunctionDecl *fn = llvm::cast<FunctionExpr>(e)->fn;
      return {FlowRun::WalkFunction{fn}, FlowRun::FinishFunctionExpr{fn}};
    }
    case Expr::Kind::Yield: {
      auto *y = llvm::cast<YieldExpr>(e);
      return {FlowRun::WalkExpr{y->value}, FlowRun::FinishYield{y}};
    }
    case Expr::Kind::Await: {
      auto *waited = llvm::cast<AwaitExpr>(e);
      return {FlowRun::WalkExpr{waited->value}, FlowRun::FinishAwait{waited}};
    }
  }

  return {};
}

std::vector<FlowRun::Step> FlowExprs::templateParts(TemplateExpr *text) const {
  auto parts = std::vector<FlowRun::Step>();
  for (Expr *part : text->exprs) {
    parts.push_back(FlowRun::WalkExpr{part});
    parts.push_back(FlowRun::DropAnswer{});
  }
  parts.push_back(FlowRun::EmptyAnswer{});

  return parts;
}

std::vector<FlowRun::Step> FlowExprs::arrayParts(ArrayLitExpr *literal) const {
  auto parts = std::vector<FlowRun::Step>();
  for (ArrayLitExpr::Element &el : literal->elements) {
    parts.push_back(FlowRun::WalkExpr{el.expr});
  }
  parts.push_back(FlowRun::FinishArray{literal});

  return parts;
}

std::vector<FlowRun::Step> FlowExprs::objectParts(ObjectLitExpr *literal, FlowRun &run) const {
  run.answers.push_back(Refs());
  auto parts = std::vector<FlowRun::Step>();
  for (ObjectLitExpr::Property &p : literal->props) {
    if (p.key()) {
      parts.push_back(FlowRun::WalkExpr{p.key()});
      parts.push_back(FlowRun::DropAnswer{});
    }
    parts.push_back(FlowRun::WalkExpr{p.value});
    parts.push_back(FlowRun::FinishProperty{&p});
  }
  if (literal->cls) {
    for (FunctionDecl *fn : literal->cls->methods) {
      parts.push_back(FlowRun::WalkFunction{fn});
      parts.push_back(FlowRun::PassObjectThis{fn});
    }
  }
  parts.push_back(FlowRun::FinishFilter{literal});

  return parts;
}

std::vector<FlowRun::Step> FlowExprs::binaryParts(BinaryExpr *binary) const {
  bool isPick = binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr ||
                binary->op == BinaryOp::Nullish;
  if (!isPick) {
    return {FlowRun::WalkExpr{binary->lhs}, FlowRun::DropAnswer{}, FlowRun::WalkExpr{binary->rhs},
            FlowRun::DropAnswer{}, FlowRun::EmptyAnswer{}};
  }

  return {FlowRun::WalkExpr{binary->lhs}, FlowRun::WalkExpr{binary->rhs},
          FlowRun::FinishJoin{binary}};
}

std::vector<FlowRun::Step> FlowExprs::newParts(NewExpr *create) const {
  CallExpr *ctor = create->ctor();
  auto parts = std::vector<FlowRun::Step>();
  for (Expr *arg : ctor ? ctor->args : create->args) {
    parts.push_back(FlowRun::WalkExpr{arg});
  }
  parts.push_back(FlowRun::FinishNew{create});

  return parts;
}

std::vector<FlowRun::Step> FlowExprs::sequenceParts(SequenceExpr *sequence, FlowRun &run) const {
  if (sequence->exprs.empty()) {
    this->builder.pushAnswer(run, sequence, Refs());
    return {};
  }
  auto parts = std::vector<FlowRun::Step>();
  for (size_t i = 0; i < sequence->exprs.size(); ++i) {
    parts.push_back(FlowRun::WalkExpr{sequence->exprs[i]});
    if (i + 1 < sequence->exprs.size()) {
      parts.push_back(FlowRun::DropAnswer{});
    }
  }
  parts.push_back(FlowRun::FinishFilter{sequence});

  return parts;
}

void FlowExprs::finishArray(ArrayLitExpr *literal, FlowRun &run) {
  std::vector<Refs> values = this->builder.takeAnswers(run, literal->elements.size());
  Refs out;
  for (size_t i = 0; i < values.size(); ++i) {
    bool isSpread = literal->elements[i].spread;
    FlowGraphBuilder::append(out, isSpread ? FlowGraphBuilder::copied(values[i])
                                           : FlowGraphBuilder::holding(values[i]));
  }
  this->builder.pushAnswer(run, literal, out);
}

void FlowExprs::finishProperty(ObjectLitExpr::Property *property, FlowRun &run) {
  Refs value = this->builder.takeAnswer(run);
  if (property->keyed()) {
    this->builder.addFlow(value, this->builder.getUnknownNode());
  } else if (!property->spread() && !property->name.empty()) {
    this->builder.addFlow(value, this->builder.fieldNode(property->name));
  }
  FlowGraphBuilder::append(run.answers.back(), property->spread()
                                                   ? FlowGraphBuilder::copied(value)
                                                   : FlowGraphBuilder::holding(value));
}

void FlowExprs::passObjectThis(FunctionDecl *fn, FlowRun &run) {
  if (fn->thisVar()) {
    this->builder.addFlow(run.answers.back(), this->builder.varNode(fn->thisVar()));
  }
}

void FlowExprs::finishFilter(const Expr *expr, FlowRun &run) {
  Refs refs = this->builder.takeAnswer(run);
  this->builder.pushAnswer(run, expr, refs);
}

void FlowExprs::finishMember(MemberExpr *member, FlowRun &run) {
  Refs object = this->builder.takeAnswer(run);
  if (member->method()) {
    this->builder.getFlowCalls().passThis(object, member->method()->decl);
    this->builder.rememberFunctionValue(member->method()->decl);
    return this->builder.pushAnswer(run, member, Refs());
  }
  Refs out = FlowGraphBuilder::shifted(object, 1);
  out.push_back({this->builder.fieldNode(member->name), 0, 0});
  this->builder.pushAnswer(run, member, out);
}

void FlowExprs::finishIndex(IndexExpr *index, FlowRun &run) {
  this->builder.takeAnswer(run);
  Refs object = this->builder.takeAnswer(run);
  Refs out = FlowGraphBuilder::shifted(object, 1);
  const Type *objectType = index->object->type;
  if (int held = this->builder.contentsNode(objectType); held >= 0) {
    out.push_back({held, 0, 0});
  } else if (!index->field().empty()) {
    out.push_back({this->builder.fieldNode(index->field()), 0, 0});
  } else if (objectType && objectType->nonOptional()->isTuple()) {
    if (auto *key = llvm::dyn_cast<IntLitExpr>(index->index)) {
      out.push_back({this->builder.fieldNode(std::to_string(key->value)), 0, 0});
    }
  }
  this->builder.pushAnswer(run, index, out);
}

void FlowExprs::finishJoin(const Expr *expr, FlowRun &run) {
  Refs right = this->builder.takeAnswer(run);
  Refs left = this->builder.takeAnswer(run);
  FlowGraphBuilder::append(left, right);
  this->builder.pushAnswer(run, expr, left);
}

void FlowExprs::finishNew(NewExpr *create, FlowRun &run) {
  CallExpr *ctor = create->ctor();
  size_t count = ctor ? ctor->args.size() : create->args.size();
  std::vector<Refs> args = this->builder.takeAnswers(run, count);
  Refs out;
  for (const Refs &arg : args) {
    FlowGraphBuilder::append(out, FlowGraphBuilder::holding(arg));
  }
  const FunctionDecl *target = !ctor                                    ? nullptr
                               : ctor->method() && ctor->method()->decl ? ctor->method()->decl
                               : ctor->func()                           ? ctor->func()->decl
                                                                        : nullptr;
  if (!target) {
    return this->builder.pushAnswer(run, create, out);
  }
  this->builder.getFlowCalls().passArguments(ctor, target, args);
  if (target->thisVar()) {
    out.push_back({this->builder.varNode(target->thisVar()), 0, 0});
    if (create->cls() && target->cls() && target->cls() != create->cls()) {
      this->builder.getFlowCalls().passThis(
          this->builder.getFlowCalls().receiverConversion(
              create, create->cls()->type, target, {}, target->thisVar(),
              "as 'this' of the constructor of '" + target->cls()->name.str() + "'"),
          target);
    }
  }
  this->builder.pushAnswer(run, create, out);
}

void FlowExprs::finishCast(CastExpr *conversion, FlowRun &run) {
  Refs out = this->builder.takeAnswer(run);
  if (this->builder.isWideningCast(conversion)) {
    int at = this->builder.findNode(NodeKind::Conversion, conversion, "");
    auto *n = llvm::dyn_cast<NameExpr>(this->ownExpr(conversion->operand));
    this->builder.addConversion(
        {at, conversion->operand->type, conversion->type, conversion->loc(),
         this->builder.findNarrowings(conversion->operand->type, conversion->type), -1, "",
         n ? n->name : llvm::StringRef()});
    out.push_back({at, 0, 0});
  }
  this->builder.pushAnswer(run, conversion, out);
}

void FlowExprs::finishFunctionExpr(FunctionDecl *fn, FlowRun &run) {
  this->builder.rememberFunctionValue(fn);
  run.answers.push_back(Refs());
}

void FlowExprs::finishYield(YieldExpr *yield, FlowRun &run) {
  Refs value = this->builder.takeAnswer(run);
  if (!this->builder.isInsideFunction()) {
    this->builder.escape(value);
  } else {
    this->builder.addFlow(yield->delegate ? value : FlowGraphBuilder::holding(value),
                          this->builder.returnNode(this->builder.getCurrentFunction()));
  }
  run.answers.push_back(Refs());
}

void FlowExprs::finishAwait(AwaitExpr *await, FlowRun &run) {
  Refs out = this->builder.takeAnswer(run);
  if (int held = this->builder.contentsNode(await->value->type); held >= 0) {
    out.push_back({held, 0, 0});
  }
  this->builder.pushAnswer(run, await, out);
}

}
