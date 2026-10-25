#include "compiler/program/types/union_slots/union_slots.h"
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

UnionSlot unionSlot(const Type *type) {
  switch (type->kind()) {
  case Type::Kind::String:
    return UnionSlot::String;
  case Type::Kind::Int:
    return UnionSlot::Int;
  case Type::Kind::Long:
    return UnionSlot::Long;
  case Type::Kind::Double:
    return UnionSlot::Double;
  case Type::Kind::Bool:
    return UnionSlot::Bool;
  case Type::Kind::Decimal:
    return UnionSlot::Decimal;
  case Type::Kind::Object:
    return UnionSlot::Object;
  case Type::Kind::Dict:
    return UnionSlot::Object; // its layout tells it apart
  case Type::Kind::Promise:
    return UnionSlot::Object; // so does a Promise's
  case Type::Kind::ClassRef:
    return UnionSlot::Object; // and a class's
  case Type::Kind::Array:
    return UnionSlot::Array;
  case Type::Kind::Function:
    return UnionSlot::Function;
  case Type::Kind::Iterator:
    return UnionSlot::Iterator;
  case Type::Kind::Param:
    return UnionSlot::Param;
  case Type::Kind::Literal: {
    const Type *base = type->literalBase();
    return base->isString() ? UnionSlot::Atom : unionSlot(base);
  }
  case Type::Kind::Union:
    return type->isTagged() ? UnionSlot::None : type->slots().front();
  default:
    return UnionSlot::Other;
  }
}

std::string unionProblem(const Type *type) {
  type = type->nonOptional();
  if (!type->isUnion()) {
    return "";
  }
  const Type *number = nullptr, *array = nullptr, *function = nullptr, *iterator = nullptr,
             *promise = nullptr, *intl = nullptr, *crypto = nullptr;
  for (const Type *m : type->members()) {
    if (m->isVoid()) {
      return "'void' cannot be part of a union";
    }
    if (m->isJson()) {
      return "a Json already holds strings, numbers, booleans, arrays and objects, which code "
             "tells apart by checking it; use 'Json'";
    }
    if (m->isResult()) {
      return "a Result cannot be part of a union: what it holds is told by its halves; make a "
             "half the union instead, e.g. 'Result<int, A | B>'";
    }
    // Two members of slots that one check cannot tell apart.
    auto clash = [&](const Type *&seen, const char *why, const std::string &fix) -> std::string {
      if (!seen) {
        seen = m;
        return "";
      }

      return "'" + seen->name().str() + "' and '" + m->name().str() +
             "' cannot be told apart when the program runs: " + why + "; " + fix;
    };
    std::string problem;
    UnionSlot slot = unionSlot(m);
    if (slot == UnionSlot::Int || slot == UnionSlot::Long || slot == UnionSlot::Double) {
      if (!number || unionSlot(number) != slot) {
        problem = clash(number, "'typeof' gives \"number\" for both",
                        "use one number type, e.g. 'double'");
      }
    } else if (slot == UnionSlot::Array) {
      problem = clash(array, "both are arrays",
                      "use one array type, e.g. '(" +
                          (array ? array->inner()->name().str() : std::string()) + " | " +
                          m->inner()->name().str() + ")[]'");
    } else if (slot == UnionSlot::Function) {
      problem = clash(function, "'typeof' gives \"function\" for both", "use one function type");
    } else if (slot == UnionSlot::Iterator) {
      problem = clash(iterator, "both are iterators", "use one iterator type");
    } else if (m->isIntl()) {
      // The runtime makes them, and they carry no number of their own.
      problem = clash(intl,
                      "both are objects of Intl, which carry nothing a check could tell "
                      "apart by",
                      "keep each in a variable of its own type");
    } else if (m->isCrypto()) {
      // So are those of node:crypto.
      problem = clash(crypto,
                      "both are objects of node:crypto, which carry nothing a check "
                      "could tell apart by",
                      "keep each in a variable of its own type");
    } else if (m->isPromise()) {
      problem = clash(promise, "both are promises",
                      "use one Promise type, e.g. 'Promise<" +
                          (promise ? promise->promised()->name().str() : std::string()) + " | " +
                          m->promised()->name().str() + ">'");
    }
    if (!problem.empty()) {
      return problem;
    }
  }

  return "";
}

const Type *unionWithout(const Type *type, const Type *without) {
  llvm::ArrayRef<const Type *> removed =
      without->isUnion() ? without->members() : llvm::ArrayRef<const Type *>(without);
  llvm::ArrayRef<const Type *> members =
      type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
  std::vector<const Type *> rest;
  for (const Type *m : members) {
    // boolean without true is false.
    if (m->isBool()) {
      bool noTrue = llvm::is_contained(removed, Type::boolLiteral(true));
      bool noFalse = llvm::is_contained(removed, Type::boolLiteral(false));
      if (noTrue != noFalse) {
        rest.push_back(Type::boolLiteral(noFalse));
        continue;
      }
    }
    if (llvm::none_of(removed, [&](const Type *r) {
          return m == r || (m->isObject() && r->isObject() && m->isSubtypeOf(r));
        })) {
      rest.push_back(m);
    }
  }
  if (rest.empty()) {
    return nullptr;
  }

  return Type::unionOf(rest);
}

} // namespace tz
