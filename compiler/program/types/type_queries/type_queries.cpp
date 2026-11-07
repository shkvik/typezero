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

int TypeQueries::subtypeDistance(const Type *from, const Type *to) {
  if (from == to) {
    return 0;
  }
  int best = -1;
  for (const Type *base : from->bases()) {
    int d = TypeQueries::subtypeDistance(base, to);
    if (d >= 0 && (best < 0 || d + 1 < best)) {
      best = d + 1;
    }
  }

  return best;
}

bool TypeQueries::mentionsParam(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  case Type::Kind::Param:
    return true;
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return TypeQueries::mentionsParam(type->inner(), seen);
  case Type::Kind::Result:
    return TypeQueries::mentionsParam(type->okType(), seen) ||
           TypeQueries::mentionsParam(type->errType(), seen);
  case Type::Kind::Function:
    return TypeQueries::mentionsParam(type->ret(), seen) ||
           llvm::any_of(type->params(),
                        [&](const Type *p) { return TypeQueries::mentionsParam(p, seen); });
  case Type::Kind::Iterator:
    return TypeQueries::mentionsParam(type->yielded(), seen) ||
           TypeQueries::mentionsParam(type->returned(), seen) ||
           TypeQueries::mentionsParam(type->sent(), seen);
  case Type::Kind::Dict:
    return TypeQueries::mentionsParam(type->keyType(), seen) ||
           TypeQueries::mentionsParam(type->valueType(), seen);
  case Type::Kind::ClassRef:
    return type->isConstructor() && TypeQueries::mentionsParam(type->constructed(), seen);
  case Type::Kind::Union:
    return llvm::any_of(type->members(),
                        [&](const Type *m) { return TypeQueries::mentionsParam(m, seen); });
  case Type::Kind::Object:
    if (type->isAsyncIterator()) {
      return TypeQueries::mentionsParam(type->yielded(), seen) ||
             TypeQueries::mentionsParam(type->returned(), seen) ||
             TypeQueries::mentionsParam(type->sent(), seen);
    }
    if (!type->typeArgs().empty()) {
      return llvm::any_of(type->typeArgs(),
                          [&](const Type *t) { return TypeQueries::mentionsParam(t, seen); });
    }
    if (!seen.insert(type).second) {
      return false;
    }
    if (type->tupleRest() && TypeQueries::mentionsParam(type->tupleRest(), seen)) {
      return true;
    }
    return llvm::any_of(type->fields(),
                        [&](const Field &f) { return TypeQueries::mentionsParam(f.type, seen); });
  default:
    return false;
  }
}

bool TypeQueries::mentionsParam(const Type *type) {
  llvm::SmallPtrSet<const Type *, 4> seen;

  return TypeQueries::mentionsParam(type, seen);
}

}
