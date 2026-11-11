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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

FlowBuiltins::FlowBuiltins(FlowGraphBuilder &owner)
    : builder(owner) {}

Refs FlowBuiltins::builtin(CallExpr *c, MemberExpr *member, const Refs &receiver,
                           const std::vector<Refs> &args) {
  const Type *receiverType = member ? member->object->type : nullptr;
  int held = this->builder.contentsNode(receiverType);
  Refs parts = FlowGraphBuilder::shifted(receiver, 1);
  if (held >= 0) {
    parts.push_back({held, 0, 0});
  }
  auto insert = [&](size_t from) {
    for (size_t i = from; i < args.size(); ++i) {
      Refs value = c->spreads(i) ? FlowGraphBuilder::shifted(args[i], 1) : args[i];
      this->builder.getFlowStmts().recordWrite(
          receiver, true, "", c->spreads(i) ? nullptr : c->args[i],
          receiverType ? receiverType->nonOptional() : nullptr, c->loc());
      if (held >= 0) {
        this->builder.addFlow(value, held);
      }
    }
  };
  Refs given = parts;
  FlowGraphBuilder::append(given, receiver);
  for (const Refs &a : args) {
    FlowGraphBuilder::append(given, a);
  }
  Refs results;
  for (const Expr *arg : c->args) {
    this->builder.getFlowCalls().callback(arg, given);
    if (auto *f = llvm::dyn_cast<FunctionExpr>(this->builder.getFlowExprs().ownExpr(arg))) {
      results.push_back({this->builder.returnNode(f->fn), 0, 0});
    }
  }
  switch (c->builtin()) {
    case Builtin::LibAs:
    case Builtin::LibChunk:
    case Builtin::LibListener:
      return args.empty() ? Refs() : args[0];
    case Builtin::LibBoundListener: {
      Refs both = args.empty() ? Refs() : args[0];
      if (args.size() > 1) {
        FlowGraphBuilder::append(both, args[1]);
      }
      return both;
    }
    case Builtin::LibIsChunk:
    case Builtin::LibChunkKind:
    case Builtin::LibTypeName:
    case Builtin::LibOverrides:
    case Builtin::LibFsOpen:
    case Builtin::LibFsRead:
    case Builtin::LibFsWrite:
    case Builtin::LibFsClose:
    case Builtin::LibStdinChunk:
    case Builtin::LibStdinFailed:
    case Builtin::LibStdWrite:
    case Builtin::LibOrder:
      return {};
    case Builtin::LibAhead:
    case Builtin::LibAheadMovable:
      return args.empty() ? Refs() : args[0];
    default:
      break;
  }
  if (c->builtin() == Builtin::DictNew && args.size() == 1) {
    if (int into = this->builder.contentsNode(c->type); into >= 0) {
      this->builder.addFlow(
          FlowGraphBuilder::shifted(args[0], c->type->nonOptional()->isSet() ? 1 : 2), into);
      return {};
    }
  }
  switch (c->builtin()) {
    case Builtin::ConsoleLog:
    case Builtin::ConsoleError:
    case Builtin::StringOf:
    case Builtin::JsonStringify:
    case Builtin::ArrJoin:
    case Builtin::ArrIndexOf:
    case Builtin::ArrLastIndexOf:
    case Builtin::ArrIncludes:
    case Builtin::ArrForEach:
    case Builtin::ArrSome:
    case Builtin::ArrEvery:
    case Builtin::ArrFindIndex:
    case Builtin::ArrFindLastIndex:
    case Builtin::ArrToString:
    case Builtin::ObjectKeys:
    case Builtin::MapHas:
    case Builtin::MapDelete:
    case Builtin::MapClear:
    case Builtin::MapSize:
    case Builtin::ResultIsOk:
    case Builtin::ResultIsErr:
      return {};
    case Builtin::ResultOk:
    case Builtin::ResultErr: {
      Refs out;
      for (const Refs &a : args) {
        FlowGraphBuilder::append(out, FlowGraphBuilder::holding(a));
      }
      return out;
    }
    case Builtin::ResultUnwrapOr: {
      Refs out = parts;
      for (const Refs &a : args) {
        FlowGraphBuilder::append(out, a);
      }
      return out;
    }
    case Builtin::ArrPush:
    case Builtin::ArrUnshift:
      insert(0);
      return {};
    case Builtin::ArrFill:
      insert(0);
      return receiver;
    case Builtin::ArrSplice:
      insert(2);
      return FlowGraphBuilder::copied(receiver);
    case Builtin::ArrReverse:
    case Builtin::ArrSort:
    case Builtin::ArrSortWith:
      return receiver;
    case Builtin::ArrPop:
    case Builtin::ArrShift:
    case Builtin::ArrAt:
    case Builtin::ArrFind:
    case Builtin::ArrFindLast:
    case Builtin::MapGet:
      return parts;
    case Builtin::ArrSlice:
    case Builtin::ArrFilter:
    case Builtin::ObjectValues:
    case Builtin::ObjectEntries:
    case Builtin::MapValues:
    case Builtin::MapEntries:
    case Builtin::MapKeys:
    case Builtin::SetValues:
    case Builtin::SetEntries:
    case Builtin::ArrValues:
    case Builtin::ArrEntries:
      return FlowGraphBuilder::copied(receiver);
    case Builtin::ArrConcat: {
      Refs out = FlowGraphBuilder::copied(receiver);
      for (size_t i = 0; i < args.size(); ++i) {
        bool isArray = c->args[i]->type && c->args[i]->type->nonOptional()->isArray();
        FlowGraphBuilder::append(out, isArray ? FlowGraphBuilder::copied(args[i])
                                              : FlowGraphBuilder::holding(args[i]));
      }
      return out;
    }
    case Builtin::ArrMap:
    case Builtin::ArrFlatMap: {
      Refs out = FlowGraphBuilder::copied(receiver);
      FlowGraphBuilder::append(out, FlowGraphBuilder::holding(results));
      return out;
    }
    case Builtin::ArrReduce:
    case Builtin::ArrReduceRight: {
      Refs out = parts;
      FlowGraphBuilder::append(out, results);
      for (const Refs &a : args) {
        FlowGraphBuilder::append(out, a);
      }
      return out;
    }
    case Builtin::MapSet:
    case Builtin::SetAdd:
    case Builtin::RecordSet:
    case Builtin::RecordNullishSet:
      if (held >= 0) {
        for (const Refs &a : args) {
          this->builder.addFlow(a, held);
        }
      } else {
        for (const Refs &a : args) {
          this->builder.escape(a);
        }
      }
      return receiver;
    default: {
      Refs out = receiver;
      FlowGraphBuilder::append(out, parts);
      for (const Refs &a : args) {
        FlowGraphBuilder::append(out, a);
        FlowGraphBuilder::append(out, FlowGraphBuilder::shifted(a, 1));
      }
      FlowGraphBuilder::append(out, results);
      for (size_t i = 0; i < args.size(); ++i) {
        if (FlowGraphBuilder::holdsReferences(c->args[i]->type)) {
          this->builder.escape(args[i]);
        }
      }
      return out;
    }
  }
}

}
