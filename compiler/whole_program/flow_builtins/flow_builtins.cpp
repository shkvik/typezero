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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

FlowBuiltins::FlowBuiltins(Flows &flows) : flows_(flows) {}

Refs FlowBuiltins::builtin(CallExpr *c, MemberExpr *member, const Refs &receiver,
                           const std::vector<Refs> &args) {
  const Type *receiverType = member ? member->object->type : nullptr;
  int held = flows_.contents(receiverType);
  Refs parts = shifted(receiver, 1);
  if (held >= 0) {
    parts.push_back({held, 0, 0});
  }
  auto insert = [&](size_t from) {
    for (size_t i = from; i < args.size(); ++i) {
      Refs value = c->spreads(i) ? shifted(args[i], 1) : args[i];
      flows_.flowStmts().recordWrite(receiver, true, "", c->spreads(i) ? nullptr : c->args[i],
                                     receiverType ? receiverType->nonOptional() : nullptr,
                                     c->loc());
      if (held >= 0) {
        flows_.flow(value, held);
      }
    }
  };
  Refs given = parts;
  append(given, receiver);
  for (const Refs &a : args) {
    append(given, a);
  }
  Refs results;
  for (const Expr *arg : c->args) {
    flows_.flowCalls().callback(arg, given);
    if (auto *f = llvm::dyn_cast<FunctionExpr>(FlowExprs::ownExpr(arg))) {
      results.push_back({flows_.returned(f->fn), 0, 0});
    }
  }
  switch (c->builtin) {
  case Builtin::LibAs:
  case Builtin::LibChunk:
  case Builtin::LibListener:
    return args.empty() ? Refs() : args[0];
  case Builtin::LibIsChunk:
  case Builtin::LibChunkKind:
  case Builtin::LibTypeName:
  case Builtin::LibOverrides:
  case Builtin::LibFsOpen:
  case Builtin::LibFsRead:
  case Builtin::LibFsWrite:
  case Builtin::LibFsClose:
  case Builtin::LibStdinChunk:
  case Builtin::LibStdWrite:
  case Builtin::LibOrder:
    return {};
  case Builtin::LibAhead:
    return args.empty() ? Refs() : args[0];
  default:
    break;
  }
  if (c->builtin == Builtin::DictNew && args.size() == 1) {
    if (int into = flows_.contents(c->type); into >= 0) {
      flows_.flow(shifted(args[0], c->type->nonOptional()->isSet() ? 1 : 2), into);
      return {};
    }
  }
  switch (c->builtin) {
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
      append(out, holding(a));
    }
    return out;
  }
  case Builtin::ResultUnwrapOr: {
    Refs out = parts;
    for (const Refs &a : args) {
      append(out, a);
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
    return copied(receiver);
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
    return copied(receiver);
  case Builtin::ArrConcat: {
    Refs out = copied(receiver);
    for (size_t i = 0; i < args.size(); ++i) {
      bool array = c->args[i]->type && c->args[i]->type->nonOptional()->isArray();
      append(out, array ? copied(args[i]) : holding(args[i]));
    }
    return out;
  }
  case Builtin::ArrMap:
  case Builtin::ArrFlatMap: {
    Refs out = copied(receiver);
    append(out, holding(results));
    return out;
  }
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight: {
    Refs out = parts;
    append(out, results);
    for (const Refs &a : args) {
      append(out, a);
    }
    return out;
  }
  case Builtin::MapSet:
  case Builtin::SetAdd:
  case Builtin::RecordSet:
  case Builtin::RecordNullishSet:
    if (held >= 0) {
      for (const Refs &a : args) {
        flows_.flow(a, held);
      }
    } else {
      for (const Refs &a : args) {
        flows_.escape(a);
      }
    }
    return receiver;
  default: {
    Refs out = receiver;
    append(out, parts);
    for (const Refs &a : args) {
      append(out, a);
      append(out, shifted(a, 1));
    }
    append(out, results);
    for (size_t i = 0; i < args.size(); ++i) {
      if (holdsReferences(c->args[i]->type)) {
        flows_.escape(args[i]);
      }
    }
    return out;
  }
  }
}

}
