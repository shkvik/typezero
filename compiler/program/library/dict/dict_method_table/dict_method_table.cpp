#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const DictMethodInfo kDictMethods[] = {
    {
        .name = "get",
        .method = DictMethod::GET,
        .form = MapOnlyDictMethod{.builtin = Builtin::MapGet},
    },
    {
        .name = "set",
        .method = DictMethod::SET,
        .form = MapOnlyDictMethod{.builtin = Builtin::MapSet},
    },
    {
        .name = "add",
        .method = DictMethod::ADD,
        .form = SetOnlyDictMethod{.builtin = Builtin::SetAdd},
    },
    {
        .name = "has",
        .method = DictMethod::HAS,
        .form = SharedDictMethod{.builtin = Builtin::MapHas},
    },
    {
        .name = "delete",
        .method = DictMethod::DELETE_KEY,
        .form = SharedDictMethod{.builtin = Builtin::MapDelete},
    },
    {
        .name = "clear",
        .method = DictMethod::CLEAR,
        .form = SharedDictMethod{.builtin = Builtin::MapClear},
    },
    {
        .name = "keys",
        .method = DictMethod::KEYS,
        .form = IteratingDictMethod{.onMap = Builtin::MapKeys, .onSet = Builtin::SetValues},
    },
    {
        .name = "values",
        .method = DictMethod::VALUES,
        .form = IteratingDictMethod{.onMap = Builtin::MapValues, .onSet = Builtin::SetValues},
    },
    {
        .name = "entries",
        .method = DictMethod::ENTRIES,
        .form = IteratingDictMethod{.onMap = Builtin::MapEntries, .onSet = Builtin::SetEntries},
    },
    {
        .name = "forEach",
        .method = DictMethod::FOR_EACH,
        .form = SharedDictMethod{.builtin = Builtin::MapForEach},
    },
    {
        .name = "union",
        .method = DictMethod::UNION,
        .form = SetCombineDictMethod{.builtin = Builtin::SetUnion},
    },
    {
        .name = "intersection",
        .method = DictMethod::INTERSECTION,
        .form = SetCombineDictMethod{.builtin = Builtin::SetIntersection},
    },
    {
        .name = "difference",
        .method = DictMethod::DIFFERENCE,
        .form = SetCombineDictMethod{.builtin = Builtin::SetDifference},
    },
    {
        .name = "symmetricDifference",
        .method = DictMethod::SYMMETRIC_DIFFERENCE,
        .form = SetCombineDictMethod{.builtin = Builtin::SetSymmetricDifference},
    },
    {
        .name = "isSubsetOf",
        .method = DictMethod::IS_SUBSET_OF,
        .form = SetTestDictMethod{.builtin = Builtin::SetIsSubsetOf},
    },
    {
        .name = "isSupersetOf",
        .method = DictMethod::IS_SUPERSET_OF,
        .form = SetTestDictMethod{.builtin = Builtin::SetIsSupersetOf},
    },
    {
        .name = "isDisjointFrom",
        .method = DictMethod::IS_DISJOINT_FROM,
        .form = SetTestDictMethod{.builtin = Builtin::SetIsDisjointFrom},
    },
};

}

bool DictMethodTable::isOnMap(const DictMethodInfo &method) {
  switch (method.form.kind()) {
    case DictMethodKind::MAP_ONLY:
    case DictMethodKind::SHARED:
    case DictMethodKind::ITERATING:
      return true;
    case DictMethodKind::SET_ONLY:
    case DictMethodKind::SET_COMBINE:
    case DictMethodKind::SET_TEST:
      return false;
  }
  llvm_unreachable("unknown dict method kind");
}

bool DictMethodTable::isOnSet(const DictMethodInfo &method) {
  switch (method.form.kind()) {
    case DictMethodKind::SET_ONLY:
    case DictMethodKind::SHARED:
    case DictMethodKind::ITERATING:
    case DictMethodKind::SET_COMBINE:
    case DictMethodKind::SET_TEST:
      return true;
    case DictMethodKind::MAP_ONLY:
      return false;
  }
  llvm_unreachable("unknown dict method kind");
}

const DictMethodInfo *DictMethodTable::find(llvm::StringRef name) {
  for (const DictMethodInfo &method : kDictMethods) {
    if (method.name == name) {
      return &method;
    }
  }

  return nullptr;
}

Builtin DictMethodTable::getBuiltin(const DictMethodInfo &method, bool onMap) {
  switch (method.form.kind()) {
    case DictMethodKind::MAP_ONLY:
      return method.form.as<MapOnlyDictMethod>()->builtin;
    case DictMethodKind::SET_ONLY:
      return method.form.as<SetOnlyDictMethod>()->builtin;
    case DictMethodKind::SHARED:
      return method.form.as<SharedDictMethod>()->builtin;
    case DictMethodKind::ITERATING: {
      const IteratingDictMethod *iterating = method.form.as<IteratingDictMethod>();
      return onMap ? iterating->onMap : iterating->onSet;
    }
    case DictMethodKind::SET_COMBINE:
      return method.form.as<SetCombineDictMethod>()->builtin;
    case DictMethodKind::SET_TEST:
      return method.form.as<SetTestDictMethod>()->builtin;
  }
  llvm_unreachable("unknown dict method kind");
}

llvm::StringRef DictMethodTable::getName(DictMethod method) {
  for (const DictMethodInfo &info : kDictMethods) {
    if (info.method == method) {
      return info.name;
    }
  }
  llvm_unreachable("dict method missing from the table");
}

llvm::ArrayRef<DictMethodInfo> DictMethodTable::getMethods() {
  return kDictMethods;
}

}
