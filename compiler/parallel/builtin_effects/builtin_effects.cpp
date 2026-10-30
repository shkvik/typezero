#include "compiler/parallel/builtin_effects/builtin_effects.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/alias_analysis/alias_analysis.h"
#include "compiler/parallel/call_resolver/call_resolver.h"
#include "compiler/parallel/call_walk/call_walk.h"
#include "compiler/parallel/code_walk/code_walk.h"
#include "compiler/parallel/conflicts/conflicts.h"
#include "compiler/parallel/explainer/explainer.h"
#include "compiler/parallel/expr_walk/expr_walk.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/parallel/pieces/pieces.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tz::parallel {

using llvm::dyn_cast_or_null;

BuiltinEffects::BuiltinEffects(Analysis &analysis) : analysis_(analysis) {}

void BuiltinEffects::builtinEffects(const CallExpr *c, const Expr *recv) {
  llvm::SMLoc at = c->loc();
  Builtin b = c->builtin;
  auto arg = [&](size_t i) -> const Expr * { return i < c->args.size() ? c->args[i] : nullptr; };
  auto readArgs = [&] {
    for (const Expr *a : c->args) {
      analysis_.exprWalk().contents(a, false, at);
    }
  };
  auto field = [&](const Expr *object, llvm::StringRef name, bool write) {
    if (object && holdsReferences(object->type)) {
      analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(object),
                                 analysis_.intern(name), write, at);
    }
  };
  switch (b) {
  case Builtin::ConsoleLog:
  case Builtin::ProcessStdoutWrite:
    readArgs();
    analysis_.resource(Res::Stdout, true, at);
    return;
  case Builtin::ConsoleError:
  case Builtin::ProcessStderrWrite:
    readArgs();
    analysis_.resource(Res::Stderr, true, at);
    return;
  case Builtin::ProcessExit:
    analysis_.resource(Res::Process, true, at);
    analysis_.resource(Res::Stdout, true, at);
    analysis_.resource(Res::Stderr, true, at);
    return;
  case Builtin::ProcessExitCode:
  case Builtin::ProcessEnv:
  case Builtin::EnvMap:
    analysis_.resource(Res::Process, false, at);
    return;
  case Builtin::ProcessSetExitCode:
    analysis_.resource(Res::Process, true, at);
    return;
  case Builtin::ReadLine:
  case Builtin::RlQuestion:
  case Builtin::RlQuestionCallback:
    analysis_.resource(Res::Stdin, true, at);
    analysis_.resource(Res::Stdout, true, at);
    return;
  case Builtin::RlClose:
    analysis_.resource(Res::Stdin, true, at);
    return;
  case Builtin::StringOf:
  case Builtin::JsonStringify:
    readArgs();
    return;
  case Builtin::TupleJoin:
    field(recv, "*", false);
    return;
  case Builtin::ArrPush:
  case Builtin::ArrPop:
  case Builtin::ArrShift:
  case Builtin::ArrUnshift:
  case Builtin::ArrSplice:
    analysis_.exprWalk().elements(recv, true, at);
    if (recv && holdsReferences(recv->type)) {
      analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(recv),
                                 analysis_.length_, true, at);
    }
    return;
  case Builtin::ArrReverse:
  case Builtin::ArrFill:
  case Builtin::ArrSort:
  case Builtin::ArrSortWith:
    analysis_.exprWalk().elements(recv, true, at);
    return;
  case Builtin::ArrJoin:
  case Builtin::ArrToString:
    analysis_.exprWalk().contents(recv, false, at);
    return;
  case Builtin::ArrConcat:
    analysis_.exprWalk().elements(recv, false, at);
    for (const Expr *a : c->args) {
      analysis_.exprWalk().elements(a, false, at);
    }
    return;
  case Builtin::ArrAt:
  case Builtin::ArrSlice:
  case Builtin::ArrIndexOf:
  case Builtin::ArrLastIndexOf:
  case Builtin::ArrIncludes:
  case Builtin::ArrForEach:
  case Builtin::ArrMap:
  case Builtin::ArrFilter:
  case Builtin::ArrFlatMap:
  case Builtin::ArrReduce:
  case Builtin::ArrReduceRight:
  case Builtin::ArrFind:
  case Builtin::ArrFindIndex:
  case Builtin::ArrFindLast:
  case Builtin::ArrFindLastIndex:
  case Builtin::ArrSome:
  case Builtin::ArrEvery:
  case Builtin::ArrKeys:
  case Builtin::ArrValues:
  case Builtin::ArrEntries:
    analysis_.exprWalk().elements(recv, false, at);
    return;
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
    if (const Expr *a = arg(0); a && a->type && a->type->nonOptional()->isObject()) {
      field(a, "*", false);
    } else {
      analysis_.exprWalk().elements(a, false, at);
    }
    return;
  case Builtin::IterNext:
    if (recv) {
      analysis_.codeWalk().iterate(recv, c);
    }
    return;
  case Builtin::IterFrom:
  case Builtin::ArrayFrom:
    if (const Expr *a = arg(0);
        a && a->type &&
        (a->type->nonOptional()->isIterator() || a->type->nonOptional()->isAsyncIterator())) {
      analysis_.codeWalk().iterate(a, c);
    } else {
      analysis_.exprWalk().contents(arg(0), false, at);
    }
    return;
  case Builtin::IterMap:
  case Builtin::IterFilter:
  case Builtin::IterTake:
  case Builtin::IterDrop:
  case Builtin::IterFlatMap:
  case Builtin::IterToArray:
  case Builtin::IterForEach:
  case Builtin::IterReduce:
  case Builtin::IterSome:
  case Builtin::IterEvery:
  case Builtin::IterFind:
    if (recv) {
      analysis_.codeWalk().iterate(recv, c);
    }
    return;
  case Builtin::MapGet:
  case Builtin::MapHas:
  case Builtin::MapSize:
  case Builtin::MapForEach:
  case Builtin::MapKeys:
  case Builtin::MapValues:
  case Builtin::MapEntries:
  case Builtin::SetValues:
  case Builtin::SetEntries:
    analysis_.exprWalk().elements(recv, false, at);
    return;
  case Builtin::MapSet:
  case Builtin::MapDelete:
  case Builtin::MapClear:
  case Builtin::SetAdd:
  case Builtin::RecordSet:
    analysis_.exprWalk().elements(recv, true, at);
    return;
  case Builtin::RecordNullishSet:
    analysis_.exprWalk().elements(recv, false, at);
    analysis_.exprWalk().elements(recv, true, at);
    return;
  case Builtin::SetUnion:
  case Builtin::SetIntersection:
  case Builtin::SetDifference:
  case Builtin::SetSymmetricDifference:
  case Builtin::SetIsSubsetOf:
  case Builtin::SetIsSupersetOf:
  case Builtin::SetIsDisjointFrom:
    analysis_.exprWalk().elements(recv, false, at);
    analysis_.exprWalk().elements(arg(0), false, at);
    return;
  case Builtin::DictNew:
  case Builtin::DictGroupBy:
    analysis_.exprWalk().contents(arg(0), false, at);
    return;
  case Builtin::FsReadFile:
    if (const Expr *a = arg(0); a && a->type && a->type->isInteger()) {
      analysis_.resource(Res::Stdin, true, at);
    }
    analysis_.resource(Res::Files, false, at);
    return;
  case Builtin::FsWriteFile:
    if (const Expr *a = arg(0); a && a->type && a->type->isInteger()) {
      analysis_.resource(Res::Stdout, true, at);
      analysis_.resource(Res::Stderr, true, at);
    }
    analysis_.exprWalk().contents(arg(1), false, at);
    analysis_.resource(Res::Files, true, at);
    return;
  case Builtin::FsExists:
  case Builtin::FsAccess:
  case Builtin::FsStat:
  case Builtin::FsReaddir:
  case Builtin::FsRealpath:
    analysis_.resource(Res::Files, false, at);
    return;
  case Builtin::FsChmod:
  case Builtin::FsTruncate:
  case Builtin::FsMkdir:
  case Builtin::FsMkdtemp:
  case Builtin::FsRmdir:
  case Builtin::FsUnlink:
  case Builtin::FsRm:
  case Builtin::FsRename:
  case Builtin::FsReplace:
  case Builtin::FsCopyFile:
    analysis_.resource(Res::Files, true, at);
    return;
  case Builtin::RegExpTest:
  case Builtin::RegExpExec:
    field(recv, "lastIndex", true);
    return;
  case Builtin::StrMatch:
  case Builtin::StrMatchGlobal:
  case Builtin::StrMatchAll:
  case Builtin::StrSearch:
  case Builtin::StrReplaceRegex:
  case Builtin::StrReplaceRegexFn:
  case Builtin::StrSplitRegex:
    field(arg(0), "lastIndex", true);
    return;
  case Builtin::DateSetTime:
  case Builtin::DateSetUTC:
  case Builtin::DateSetLocal:
    field(recv, "%time", true);
    return;
  case Builtin::DateGetTime:
  case Builtin::DateGetUTC:
  case Builtin::DateGetLocal:
  case Builtin::DateToISOString:
  case Builtin::DateToJSON:
  case Builtin::DateToUTCString:
  case Builtin::DateToLocalString:
  case Builtin::DateToLocale:
    field(recv, "%time", false);
    return;
  case Builtin::DateCopy:
  case Builtin::DtfFormat:
  case Builtin::DtfFormatToParts:
    field(arg(0), "%time", false);
    return;
  case Builtin::BytesFromArray:
  case Builtin::BytesFromBytes:
  case Builtin::BytesOf:
  case Builtin::BufferConcat:
    analysis_.exprWalk().contents(arg(0), false, at);
    return;
  case Builtin::BytesAt:
  case Builtin::BytesIndexOf:
  case Builtin::BytesJoin:
  case Builtin::BytesSlice:
  case Builtin::BytesToReversed:
  case Builtin::BytesToSorted:
  case Builtin::BytesWith:
  case Builtin::ArrayBufferSlice:
  case Builtin::DataViewGet:
  case Builtin::BufferToString:
  case Builtin::BufferIndexOf:
  case Builtin::BufferRead:
  case Builtin::BufferReadVar:
  case Builtin::BufferToJSON:
  case Builtin::BufferEquals:
  case Builtin::BufferCompareTo:
    analysis_.exprWalk().elements(recv, false, at);
    for (const Expr *a : c->args) {
      analysis_.exprWalk().elements(a, false, at);
    }
    return;
  case Builtin::BufferCompare:
    analysis_.exprWalk().elements(arg(0), false, at);
    analysis_.exprWalk().elements(arg(1), false, at);
    return;
  case Builtin::BytesReverse:
  case Builtin::BytesSort:
  case Builtin::BytesFill:
  case Builtin::BytesCopyWithin:
  case Builtin::DataViewSet:
  case Builtin::BufferWrite:
  case Builtin::BufferFill:
  case Builtin::BufferWriteNumber:
  case Builtin::BufferWriteVar:
  case Builtin::BufferSwap:
    analysis_.exprWalk().elements(recv, true, at);
    return;
  case Builtin::BytesSet:
    analysis_.exprWalk().elements(recv, true, at);
    analysis_.exprWalk().contents(arg(0), false, at);
    return;
  case Builtin::BufferCopy:
    analysis_.exprWalk().elements(recv, false, at);
    analysis_.exprWalk().elements(arg(0), true, at);
    return;
  case Builtin::TextEncodeInto:
    analysis_.exprWalk().elements(arg(1), true, at);
    return;
  case Builtin::TextDecode:
    analysis_.exprWalk().elements(arg(0), false, at);
    return;
  case Builtin::MetaDefine:
  case Builtin::MetaDelete:
    analysis_.resource(Res::Metadata, true, at);
    return;
  case Builtin::ChildSpawnSync:
  case Builtin::ChildExecFileSync:
  case Builtin::ChildExecSync:
    readArgs();
    analysis_.resource(Res::Files, true, at);
    analysis_.resource(Res::Stdout, true, at);
    analysis_.resource(Res::Stderr, true, at);
    analysis_.resource(Res::Process, false, at);
    return;
  case Builtin::Crypto: {
    auto *number = dyn_cast_or_null<IntLitExpr>(strip(arg(0)));
    if (!number || number->value >= std::size(kCryptoFunctions)) {
      readArgs();
      return;
    }
    auto fn = CryptoFn(unsigned(number->value));
    llvm::StringRef kinds = kCryptoFunctions[size_t(number->value)].params;
    bool fills = fn == CryptoFn::RandomFill || fn == CryptoFn::GetRandomValues;
    for (size_t i = 1; i < c->args.size(); ++i) {
      const Expr *a = c->args[i];
      char kind = i - 1 < kinds.size() ? kinds[i - 1] : 'd';
      if (!a || !holdsReferences(a->type)) {
        continue;
      }
      bool bytes = a->type->nonOptional()->isBytes();
      if (kind == 'o' && bytes) {
        analysis_.exprWalk().elements(a, fills, at);
      } else if (kind == 'o') {
        analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(a),
                                   analysis_.state_, true, at);
      } else if (bytes) {
        analysis_.exprWalk().elements(a, false, at);
      } else {
        analysis_.exprWalk().contents(a, false, at);
      }
    }
    return;
  }
  case Builtin::CryptoKeyPair:
    readArgs();
    return;
  case Builtin::ReflectGet:
    if (const Expr *a = arg(0); a && holdsReferences(a->type)) {
      analysis_.codeWalk().touch(LocKind::Field, analysis_.aliasAnalysis().refOf(a),
                                 analysis_.star_, false, at);
    }
    return;
  case Builtin::ReflectApply: {
    for (size_t i = 2; i < c->args.size(); ++i) {
      analysis_.exprWalk().contents(c->args[i], false, at);
    }
    CallRec &rec = analysis_.codeWalk().record(c, CallKind::Call, {}, at);
    rec.call = c;
    const Expr *fn = strip(arg(0));
    if (fn && fn->type && fn->type->nonOptional()->isFunction()) {
      rec.byType = fn->type;
      return;
    }
    rec.reflect = CallRec::Reflect::Apply;
    const Expr *self = strip(arg(1));
    rec.bySelf =
        self ? std::optional<Ref>(analysis_.aliasAnalysis().receiverRef(self)) : std::nullopt;
    rec.byObject = self ? self->type : nullptr;
    return;
  }
  case Builtin::ReflectConstruct: {
    for (size_t i = 1; i < c->args.size(); ++i) {
      analysis_.exprWalk().contents(c->args[i], false, at);
    }
    CallRec &rec = analysis_.codeWalk().record(c, CallKind::Call, {}, at);
    rec.call = c;
    rec.reflect = CallRec::Reflect::Construct;
    return;
  }
  case Builtin::MetaGet:
  case Builtin::MetaGetOwn:
  case Builtin::MetaHas:
  case Builtin::MetaHasOwn:
    analysis_.resource(Res::Metadata, false, at);
    return;
  default:
    break;
  }
  if (knownBuiltin(b)) {
    return;
  }
  analysis_.exprWalk().contents(recv, true, at);
  for (const Expr *a : c->args) {
    analysis_.exprWalk().contents(a, true, at);
  }
  analysis_.resource(Res::Runtime, true, at);
}

}
