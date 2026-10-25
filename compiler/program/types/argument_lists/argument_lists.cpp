#include "compiler/program/types/argument_lists/argument_lists.h"
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

const Type *spreadTuple(llvm::ArrayRef<const Type *> parts, const std::vector<bool> &spread,
                        std::string &why) {
  std::vector<const Type *> elements;
  size_t min = 0;
  const Type *rest = nullptr;
  bool generic = false;
  for (size_t i = 0; i < parts.size(); ++i) {
    const Type *part = parts[i];
    if (part->isError()) {
      return part;
    }
    if (rest) {
      why = "nothing can follow the array spread into a list: it takes the arguments to the end";
      return nullptr;
    }
    if (i >= spread.size() || !spread[i]) {
      elements.push_back(part);
      min = elements.size(); // the elements before it are needed then
      continue;
    }
    // A list of types in generic code stays as it is, spread there.
    if (part->isListParam()) {
      elements.push_back(part);
      min = elements.size();
      generic = true;
      continue;
    }
    if (part->isArray()) {
      rest = part;
      continue;
    }
    if (!part->isTuple()) {
      why = "only a list of types ('A extends unknown[]'), a tuple or an array can be spread into "
            "a tuple type, not '" +
            part->name().str() + "'";
      return nullptr;
    }
    bool needed = min == elements.size();
    for (const Field &f : part->fields()) {
      elements.push_back(f.type);
    }
    generic |= llvm::any_of(part->fields(), [](const Field &f) { return f.type->isListParam(); });
    min = needed ? elements.size() - part->fields().size() + part->tupleMin() : min;
    rest = part->tupleRest();
  }
  if (generic) {
    if (rest) {
      why = "a list of types and an array cannot be spread into one tuple type";
      return nullptr;
    }
    // `[...A]` is A.
    if (elements.size() == 1 && elements[0]->isListParam()) {
      return elements[0];
    }
    return Type::tuple(elements);
  }

  return Type::argumentList(elements, min, rest);
}

bool flattenArguments(llvm::ArrayRef<const Type *> params, size_t &minArgs, bool &hasRest,
                      std::vector<const Type *> &flat) {
  if (!hasRest || params.empty()) {
    return false;
  }
  const Type *list = params.back();
  if (!list->isTuple() ||
      llvm::any_of(list->fields(), [](const Field &f) { return f.type->isListParam(); })) {
    return false;
  }
  size_t fixed = params.size() - 1;
  flat.assign(params.begin(), params.end() - 1);
  for (const Field &f : list->fields()) {
    flat.push_back(f.type);
  }
  // The elements a call must give are needed if every parameter before them
  // is; and after them, the rest parameter of the list, if it has one.
  if (minArgs >= fixed) {
    minArgs = fixed + list->tupleMin();
  }
  hasRest = list->tupleRest() != nullptr;
  if (hasRest) {
    flat.push_back(list->tupleRest());
  }

  return true;
}

} // namespace tz
