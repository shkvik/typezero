#include "compiler/program/types/type_queries/type_queries.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <tuple>

namespace tz {

int subtypeDistance(const Type *from, const Type *to) {
  if (from == to) {
    return 0;
  }
  int best = -1;
  for (const Type *base : from->bases()) {
    int d = subtypeDistance(base, to);
    if (d >= 0 && (best < 0 || d + 1 < best)) {
      best = d + 1;
    }
  }

  return best;
}

static bool mentionsParam(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  case Type::Kind::Param:
    return true;
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return mentionsParam(type->inner(), seen);
  case Type::Kind::Result:
    return mentionsParam(type->okType(), seen) || mentionsParam(type->errType(), seen);
  case Type::Kind::Function:
    return mentionsParam(type->ret(), seen) ||
           llvm::any_of(type->params(), [&](const Type *p) { return mentionsParam(p, seen); });
  case Type::Kind::Iterator:
    return mentionsParam(type->yielded(), seen) || mentionsParam(type->returned(), seen) ||
           mentionsParam(type->sent(), seen);
  case Type::Kind::Dict:
    return mentionsParam(type->keyType(), seen) || mentionsParam(type->valueType(), seen);
  case Type::Kind::ClassRef: // `Constructor<T>`
    return type->isConstructor() && mentionsParam(type->constructed(), seen);
  case Type::Kind::Union:
    return llvm::any_of(type->members(), [&](const Type *m) { return mentionsParam(m, seen); });
  case Type::Kind::Object:
    // An async iterator, as an iterator: by what it gives, returns and takes.
    if (type->isAsyncIterator()) {
      return mentionsParam(type->yielded(), seen) || mentionsParam(type->returned(), seen) ||
             mentionsParam(type->sent(), seen);
    }
    // A class made from a generic one is generic code by its type
    // arguments, whatever its fields.
    if (!type->typeArgs().empty()) {
      return llvm::any_of(type->typeArgs(), [&](const Type *t) { return mentionsParam(t, seen); });
    }
    // A recursive type mentions itself.
    if (!seen.insert(type).second) {
      return false;
    }
    if (type->tupleRest() && mentionsParam(type->tupleRest(), seen)) {
      return true;
    }
    return llvm::any_of(type->fields(),
                        [&](const Field &f) { return mentionsParam(f.type, seen); });
  default:
    return false;
  }
}

bool mentionsParam(const Type *type) {
  llvm::SmallPtrSet<const Type *, 4> seen;

  return mentionsParam(type, seen);
}

} // namespace tz
