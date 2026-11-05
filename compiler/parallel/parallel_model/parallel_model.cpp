#include "compiler/parallel/parallel_model/parallel_model.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
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

void merge(Active &into, llvm::ArrayRef<unsigned> from) {
  for (unsigned s : from) {
    auto it = std::lower_bound(into.begin(), into.end(), s);
    if (it == into.end() || *it != s) {
      into.insert(it, s);
    }
  }
}

FnInfo &FunctionTable::operator[](FunctionId id) { return this->items[unsigned(id)]; }

const FnInfo &FunctionTable::operator[](FunctionId id) const { return this->items[unsigned(id)]; }

FnInfo &FunctionTable::add() { return this->items.emplace_back(); }

size_t FunctionTable::size() const { return this->items.size(); }

std::deque<FnInfo>::iterator FunctionTable::begin() { return this->items.begin(); }

std::deque<FnInfo>::iterator FunctionTable::end() { return this->items.end(); }

std::deque<FnInfo>::const_iterator FunctionTable::begin() const { return this->items.begin(); }

std::deque<FnInfo>::const_iterator FunctionTable::end() const { return this->items.end(); }

}
