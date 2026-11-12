#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const IteratorMethodInfo kIteratorMethods[] = {
    {
        .name = "next",
        .method = IteratorMethod::NEXT,
        .form = IteratorStepMethod{.builtin = Builtin::IterNext},
    },
    {
        .name = "return",
        .method = IteratorMethod::RETURN,
        .form = IteratorClosingMethod{},
    },
    {
        .name = "throw",
        .method = IteratorMethod::THROW,
        .form = IteratorClosingMethod{},
    },
    {
        .name = "map",
        .method = IteratorMethod::MAP,
        .form = IteratorStageMethod{.builtin = Builtin::IterMap},
    },
    {
        .name = "filter",
        .method = IteratorMethod::FILTER,
        .form = IteratorStageMethod{.builtin = Builtin::IterFilter},
    },
    {
        .name = "take",
        .method = IteratorMethod::TAKE,
        .form = IteratorStageMethod{.builtin = Builtin::IterTake},
    },
    {
        .name = "drop",
        .method = IteratorMethod::DROP,
        .form = IteratorStageMethod{.builtin = Builtin::IterDrop},
    },
    {
        .name = "flatMap",
        .method = IteratorMethod::FLAT_MAP,
        .form = IteratorStageMethod{.builtin = Builtin::IterFlatMap},
    },
    {
        .name = "toArray",
        .method = IteratorMethod::TO_ARRAY,
        .form = IteratorConsumerMethod{.builtin = Builtin::IterToArray},
    },
    {
        .name = "forEach",
        .method = IteratorMethod::FOR_EACH,
        .form = IteratorConsumerMethod{.builtin = Builtin::IterForEach},
    },
    {
        .name = "reduce",
        .method = IteratorMethod::REDUCE,
        .form = IteratorConsumerMethod{.builtin = Builtin::IterReduce},
    },
    {
        .name = "some",
        .method = IteratorMethod::SOME,
        .form = IteratorConsumerMethod{.builtin = Builtin::IterSome},
    },
    {
        .name = "every",
        .method = IteratorMethod::EVERY,
        .form = IteratorConsumerMethod{.builtin = Builtin::IterEvery},
    },
    {
        .name = "find",
        .method = IteratorMethod::FIND,
        .form = IteratorConsumerMethod{.builtin = Builtin::IterFind},
    },
};

}

const IteratorMethodInfo *IteratorMethodTable::find(llvm::StringRef name) {
  for (const IteratorMethodInfo &method : kIteratorMethods) {
    if (method.name == name) {
      return &method;
    }
  }

  return nullptr;
}

llvm::StringRef IteratorMethodTable::getName(IteratorMethod method) {
  for (const IteratorMethodInfo &info : kIteratorMethods) {
    if (info.method == method) {
      return info.name;
    }
  }
  llvm_unreachable("iterator method missing from the table");
}

}
