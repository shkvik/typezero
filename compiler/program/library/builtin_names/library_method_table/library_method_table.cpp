#include "compiler/program/library/builtin_names/library_method_table/library_method_table.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kSetterPrefix = "set";

const LibraryMethodInfo kRows[] = {
    {
        .name = "listenersOf",
        .value = LibraryMethod::LISTENERS_OF,
    },
    {
        .name = "removeListenerWith",
        .value = LibraryMethod::REMOVE_LISTENER_WITH,
    },
    {
        .name = "addFunctionListener",
        .value = LibraryMethod::ADD_FUNCTION_LISTENER,
    },
    {
        .name = "addObjectListener",
        .value = LibraryMethod::ADD_OBJECT_LISTENER,
    },
    {
        .name = "setEncoding",
        .value = LibraryMethod::SET_ENCODING,
    },
    {
        .name = "pushTyped",
        .value = LibraryMethod::PUSH_TYPED,
    },
    {
        .name = "unshiftTyped",
        .value = LibraryMethod::UNSHIFT_TYPED,
    },
    {
        .name = "writeTyped",
        .value = LibraryMethod::WRITE_TYPED,
    },
    {
        .name = "endTyped",
        .value = LibraryMethod::END_TYPED,
    },
    {
        .name = "finishedResult",
        .value = LibraryMethod::FINISHED_RESULT,
    },
    {
        .name = "eosFor",
        .value = LibraryMethod::EOS_FOR,
    },
    {
        .name = "composeAll",
        .value = LibraryMethod::COMPOSE_ALL,
    },
    {
        .name = "pipelineTo",
        .value = LibraryMethod::PIPELINE_TO,
    },
    {
        .name = "pipelineOut",
        .value = LibraryMethod::PIPELINE_OUT,
    },
    {
        .name = "pipelineValue",
        .value = LibraryMethod::PIPELINE_VALUE,
    },
    {
        .name = "pipelineResult",
        .value = LibraryMethod::PIPELINE_RESULT,
    },
    {
        .name = "startTls",
        .value = LibraryMethod::START_TLS,
    },
};

}

std::optional<LibraryMethod> LibraryMethodTable::find(llvm::StringRef name) {
  for (const LibraryMethodInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef LibraryMethodTable::getName(LibraryMethod value) {
  for (const LibraryMethodInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("library method missing from the table");
}

std::string LibraryMethodTable::getSetterName(llvm::StringRef field) {
  std::string first(1, llvm::toUpper(field[0]));

  return kSetterPrefix.str() + first + field.drop_front().str();
}

}
