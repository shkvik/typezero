#include "compiler/program/widening_store/widening_store.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"

namespace tz {

void WideningStore::rememberWideningCast(const CastExpr *cast) {
  this->wideningCasts.insert(cast);
}

bool WideningStore::isWideningCast(const CastExpr *cast) const {
  return this->wideningCasts.contains(cast);
}

bool WideningStore::hasWideningCasts() const {
  return !this->wideningCasts.empty();
}

void WideningStore::markFieldsNarrowed() {
  this->fieldsNarrowed = true;
}

bool WideningStore::hasNarrowedFields() const {
  return this->fieldsNarrowed;
}

void WideningStore::rememberWidening(const Type *from, const Type *to, bool narrows) {
  this->narrowingWidenings[{from, to}] = narrows;
}

bool WideningStore::hasKnownWidening(const Type *from, const Type *to) const {
  return this->narrowingWidenings.count({from, to}) > 0;
}

bool WideningStore::isNarrowingWidening(const Type *from, const Type *to) const {
  auto found = this->narrowingWidenings.find({from, to});

  return found != this->narrowingWidenings.end() && found->second;
}

}
