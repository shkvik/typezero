#include "compiler/program/library/events/listener_change_field_table/listener_change_field_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const ListenerChangeFieldInfo kRows[] = {
    {
        .name = "event",
        .value = ListenerChangeField::EVENT,
    },
    {
        .name = "listener",
        .value = ListenerChangeField::LISTENER,
    },
};

}

std::optional<ListenerChangeField> ListenerChangeFieldTable::find(llvm::StringRef name) {
  for (const ListenerChangeFieldInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef ListenerChangeFieldTable::getName(ListenerChangeField value) {
  for (const ListenerChangeFieldInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("listener change field missing from the table");
}

}
