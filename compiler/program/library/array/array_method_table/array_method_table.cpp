#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const ArrayMethodInfo kArrayMethods[] = {
    {
        .name = "push",
        .method = ArrayMethod::PUSH,
        .builtin = Builtin::ArrPush,
        .form = ResizingArrayMethod{.change = LengthChange::GROWS},
    },
    {
        .name = "unshift",
        .method = ArrayMethod::UNSHIFT,
        .builtin = Builtin::ArrUnshift,
        .form = ResizingArrayMethod{.change = LengthChange::GROWS},
    },
    {
        .name = "pop",
        .method = ArrayMethod::POP,
        .builtin = Builtin::ArrPop,
        .form = ResizingArrayMethod{.change = LengthChange::SHRINKS},
    },
    {
        .name = "shift",
        .method = ArrayMethod::SHIFT,
        .builtin = Builtin::ArrShift,
        .form = ResizingArrayMethod{.change = LengthChange::SHRINKS},
    },
    {
        .name = "splice",
        .method = ArrayMethod::SPLICE,
        .builtin = Builtin::ArrSplice,
        .form = ResizingArrayMethod{.change = LengthChange::GROWS_OR_SHRINKS},
    },
    {
        .name = "fill",
        .method = ArrayMethod::FILL,
        .builtin = Builtin::ArrFill,
        .form = RewritingArrayMethod{.releasesElements = true},
    },
    {
        .name = "sort",
        .method = ArrayMethod::SORT,
        .builtin = Builtin::ArrSort,
        .form = SortingArrayMethod{},
    },
    {
        .name = "reverse",
        .method = ArrayMethod::REVERSE,
        .builtin = Builtin::ArrReverse,
        .form = RewritingArrayMethod{.releasesElements = false},
    },
    {
        .name = "at",
        .method = ArrayMethod::AT,
        .builtin = Builtin::ArrAt,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "slice",
        .method = ArrayMethod::SLICE,
        .builtin = Builtin::ArrSlice,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "concat",
        .method = ArrayMethod::CONCAT,
        .builtin = Builtin::ArrConcat,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "indexOf",
        .method = ArrayMethod::INDEX_OF,
        .builtin = Builtin::ArrIndexOf,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "lastIndexOf",
        .method = ArrayMethod::LAST_INDEX_OF,
        .builtin = Builtin::ArrLastIndexOf,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "includes",
        .method = ArrayMethod::INCLUDES,
        .builtin = Builtin::ArrIncludes,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "join",
        .method = ArrayMethod::JOIN,
        .builtin = Builtin::ArrJoin,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "toString",
        .method = ArrayMethod::TO_STRING,
        .builtin = Builtin::ArrToString,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "keys",
        .method = ArrayMethod::KEYS,
        .builtin = Builtin::ArrKeys,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "values",
        .method = ArrayMethod::VALUES,
        .builtin = Builtin::ArrValues,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "entries",
        .method = ArrayMethod::ENTRIES,
        .builtin = Builtin::ArrEntries,
        .form = ReadingArrayMethod{},
    },
    {
        .name = "forEach",
        .method = ArrayMethod::FOR_EACH,
        .builtin = Builtin::ArrForEach,
        .form = CallbackArrayMethod{.callback = ArrayCallback::FOR_EACH},
    },
    {
        .name = "map",
        .method = ArrayMethod::MAP,
        .builtin = Builtin::ArrMap,
        .form = CallbackArrayMethod{.callback = ArrayCallback::MAP},
    },
    {
        .name = "flatMap",
        .method = ArrayMethod::FLAT_MAP,
        .builtin = Builtin::ArrFlatMap,
        .form = CallbackArrayMethod{.callback = ArrayCallback::FLAT_MAP},
    },
    {
        .name = "filter",
        .method = ArrayMethod::FILTER,
        .builtin = Builtin::ArrFilter,
        .form = CallbackArrayMethod{.callback = ArrayCallback::FILTER},
    },
    {
        .name = "reduce",
        .method = ArrayMethod::REDUCE,
        .builtin = Builtin::ArrReduce,
        .form = CallbackArrayMethod{.callback = ArrayCallback::REDUCE},
    },
    {
        .name = "reduceRight",
        .method = ArrayMethod::REDUCE_RIGHT,
        .builtin = Builtin::ArrReduceRight,
        .form = CallbackArrayMethod{.callback = ArrayCallback::REDUCE},
    },
    {
        .name = "find",
        .method = ArrayMethod::FIND,
        .builtin = Builtin::ArrFind,
        .form = CallbackArrayMethod{.callback = ArrayCallback::FIND},
    },
    {
        .name = "findLast",
        .method = ArrayMethod::FIND_LAST,
        .builtin = Builtin::ArrFindLast,
        .form = CallbackArrayMethod{.callback = ArrayCallback::FIND},
    },
    {
        .name = "findIndex",
        .method = ArrayMethod::FIND_INDEX,
        .builtin = Builtin::ArrFindIndex,
        .form = CallbackArrayMethod{.callback = ArrayCallback::FIND_INDEX},
    },
    {
        .name = "findLastIndex",
        .method = ArrayMethod::FIND_LAST_INDEX,
        .builtin = Builtin::ArrFindLastIndex,
        .form = CallbackArrayMethod{.callback = ArrayCallback::FIND_INDEX},
    },
    {
        .name = "some",
        .method = ArrayMethod::SOME,
        .builtin = Builtin::ArrSome,
        .form = CallbackArrayMethod{.callback = ArrayCallback::TEST},
    },
    {
        .name = "every",
        .method = ArrayMethod::EVERY,
        .builtin = Builtin::ArrEvery,
        .form = CallbackArrayMethod{.callback = ArrayCallback::TEST},
    },
};

}

const ArrayMethodInfo *ArrayMethodTable::find(llvm::StringRef name) {
  for (const ArrayMethodInfo &method : kArrayMethods) {
    if (method.name == name) {
      return &method;
    }
  }

  return nullptr;
}

std::optional<ArrayCallback> ArrayMethodTable::findCallback(llvm::StringRef name) {
  const ArrayMethodInfo *method = ArrayMethodTable::find(name);
  if (method == nullptr) {
    return std::nullopt;
  }
  switch (method->form.kind()) {
    case ArrayMethodKind::CALLBACK:
      return method->form.as<CallbackArrayMethod>()->callback;
    case ArrayMethodKind::SORTING:
      return ArrayCallback::COMPARE;
    case ArrayMethodKind::READING:
    case ArrayMethodKind::RESIZING:
    case ArrayMethodKind::REWRITING:
      return std::nullopt;
  }
  llvm_unreachable("unknown array method kind");
}

llvm::ArrayRef<ArrayMethodInfo> ArrayMethodTable::getMethods() {
  return kArrayMethods;
}

}
