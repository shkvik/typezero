#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/parallel/parallel_model/parallel_model.h"
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

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz::parallel {

const Expr *strip(const Expr *e) {
  while (e) {
    if (auto *c = dyn_cast<CastExpr>(e); c && c->isImplicit()) {
      e = c->operand;
      continue;
    }
    break;
  }

  return e;
}

bool holdsReferences(const Type *type) {
  if (!type) {
    return false;
  }
  type = type->nonOptional();
  switch (type->kind()) {
  case Type::Kind::Array:
  case Type::Kind::Object:
  case Type::Kind::Dict:
  case Type::Kind::Json:
  case Type::Kind::Iterator:
  case Type::Kind::Unknown:
  case Type::Kind::Param:
    return true;
  case Type::Kind::Union:
    return llvm::any_of(type->members(), holdsReferences);
  case Type::Kind::Result:
    return holdsReferences(type->okType()) || holdsReferences(type->errType());
  case Type::Kind::Promise:
    return holdsReferences(type->promised());
  default:
    return false;
  }
}

bool makesNewObject(Builtin builtin) {
  switch (builtin) {
  case Builtin::DictNew:
  case Builtin::DictGroupBy:
  case Builtin::ArrMap:
  case Builtin::ArrFilter:
  case Builtin::ArrFlatMap:
  case Builtin::ArrSlice:
  case Builtin::ArrConcat:
  case Builtin::ArrayFrom:
  case Builtin::IterToArray:
  case Builtin::StrSplit:
  case Builtin::StrSplitRegex:
  case Builtin::StrMatchGlobal:
  case Builtin::ObjectKeys:
  case Builtin::ObjectValues:
  case Builtin::ObjectEntries:
  case Builtin::SetUnion:
  case Builtin::SetIntersection:
  case Builtin::SetDifference:
  case Builtin::SetSymmetricDifference:
  case Builtin::BytesNew:
  case Builtin::BytesFromArray:
  case Builtin::BytesFromBytes:
  case Builtin::BytesOf:
  case Builtin::BytesSlice:
  case Builtin::BytesToReversed:
  case Builtin::BytesToSorted:
  case Builtin::BytesWith:
  case Builtin::BufferFromString:
  case Builtin::BufferAllocFill:
  case Builtin::BufferConcat:
  case Builtin::ArrayBufferNew:
  case Builtin::TextEncode:
  case Builtin::DateNew:
  case Builtin::DateCopy:
  case Builtin::DateFromString:
  case Builtin::DateFromFields:
  case Builtin::JsonParse:
  case Builtin::RegExpNew:
  case Builtin::RegExpParse:
  case Builtin::WeakRefNew:
  case Builtin::CryptoKeyPair:
  case Builtin::ArrKeys:
  case Builtin::ArrValues:
  case Builtin::ArrEntries:
  case Builtin::MapKeys:
  case Builtin::MapValues:
  case Builtin::MapEntries:
  case Builtin::SetValues:
  case Builtin::SetEntries:
  case Builtin::IterFrom:
    return true;
  default:
    return isIteratorStage(builtin);
  }
}

llvm::StringRef libraryClassName(const ClassDecl *cls) {
  if (!cls) {
    return "";
  }

  return cls->generic ? llvm::StringRef(cls->generic->name) : llvm::StringRef(cls->name);
}

bool knownBuiltin(Builtin b) {
  if (b == Builtin::None || isMathBuiltin(b) || isDecimalBuiltin(b) || isIntlBuiltin(b) ||
      isResultBuiltin(b) || isDateBuiltin(b) || isRegexBuiltin(b) || isJsonBuiltin(b) ||
      isDictBuiltin(b) || isBytesBuiltin(b) || isMetaBuiltin(b)) {
    return true;
  }
  if (b >= Builtin::ConsoleLog && b <= Builtin::IterFind) {
    return true;
  }
  switch (b) {
  case Builtin::TupleJoin:
  case Builtin::PathJoin:
  case Builtin::PathResolve:
  case Builtin::PathNormalize:
  case Builtin::PathIsAbsolute:
  case Builtin::PathDirname:
  case Builtin::PathBasename:
  case Builtin::PathExtname:
  case Builtin::PathRelative:
  case Builtin::PathParse:
  case Builtin::PathFormat:
  case Builtin::PathToNamespacedPath:
  case Builtin::OsString:
  case Builtin::OsNumber:
  case Builtin::OsAvailableParallelism:
  case Builtin::OsCpus:
  case Builtin::OsLoadavg:
  case Builtin::OsUserInfo:
  case Builtin::ReadLine:
  case Builtin::FsReadFile:
  case Builtin::FsWriteFile:
  case Builtin::FsExists:
  case Builtin::FsAccess:
  case Builtin::FsChmod:
  case Builtin::FsTruncate:
  case Builtin::FsStat:
  case Builtin::FsReaddir:
  case Builtin::FsMkdir:
  case Builtin::FsMkdtemp:
  case Builtin::FsRmdir:
  case Builtin::FsUnlink:
  case Builtin::FsRm:
  case Builtin::FsRename:
  case Builtin::FsCopyFile:
  case Builtin::FsRealpath:
  case Builtin::FsIsKind:
  case Builtin::FsStatsDate:
  case Builtin::FspReadFile:
  case Builtin::FspWriteFile:
  case Builtin::FspAccess:
  case Builtin::FspChmod:
  case Builtin::FspTruncate:
  case Builtin::FspStat:
  case Builtin::FspReaddir:
  case Builtin::FspMkdir:
  case Builtin::FspMkdtemp:
  case Builtin::FspRmdir:
  case Builtin::FspUnlink:
  case Builtin::FspRm:
  case Builtin::FspRename:
  case Builtin::FspCopyFile:
  case Builtin::FspRealpath:
  case Builtin::FspOpen:
  case Builtin::FhFd:
  case Builtin::FhClose:
  case Builtin::FhReadFile:
  case Builtin::FhWriteFile:
  case Builtin::FhWrite:
  case Builtin::FhStat:
  case Builtin::FhTruncate:
  case Builtin::FhSync:
  case Builtin::RlCreate:
  case Builtin::RlQuestion:
  case Builtin::RlQuestionCallback:
  case Builtin::RlClose:
  case Builtin::PromiseNew:
  case Builtin::PromiseThen:
  case Builtin::PromiseResolve:
  case Builtin::PromiseAll:
  case Builtin::PromiseRace:
  case Builtin::PromiseWithResolvers:
  case Builtin::SetTimeout:
  case Builtin::SetInterval:
  case Builtin::SetImmediate:
  case Builtin::ClearTimeout:
  case Builtin::ClearImmediate:
  case Builtin::QueueMicrotask:
  case Builtin::NextTick:
  case Builtin::TimerRef:
  case Builtin::TimerUnref:
  case Builtin::TimerHasRef:
  case Builtin::TimerRefresh:
  case Builtin::TimersSetTimeout:
  case Builtin::TimersSetImmediate:
  case Builtin::TimersSetInterval:
  case Builtin::WeakRefNew:
  case Builtin::WeakRefDeref:
  case Builtin::ChildSpawnSync:
  case Builtin::ChildExecFileSync:
  case Builtin::ChildExecSync:
  case Builtin::Crypto:
  case Builtin::CryptoKeyPair:
  case Builtin::LibListener:
  case Builtin::LibAs:
  case Builtin::ClassName:
  case Builtin::ReflectGet:
  case Builtin::ReflectApply:
  case Builtin::ReflectConstruct:
    return true;
  default:
    return false;
  }
}

}
