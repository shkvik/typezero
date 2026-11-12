#include "compiler/program/library/weakref/weak_ref_method_table/weak_ref_method_table.h"

namespace tz {

namespace {

const WeakRefMethodInfo kWeakRefMethods[] = {
    {
        .name = "deref",
        .method = WeakRefMethod::DEREF,
    },
};

}

std::optional<WeakRefMethod> WeakRefMethodTable::find(llvm::StringRef name) {
  for (const WeakRefMethodInfo &info : kWeakRefMethods) {
    if (info.name == name) {
      return info.method;
    }
  }

  return std::nullopt;
}

}
