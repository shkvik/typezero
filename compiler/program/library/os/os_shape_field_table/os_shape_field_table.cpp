#include "compiler/program/library/os/os_shape_field_table/os_shape_field_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const OsShapeFieldInfo kRows[] = {
    {
        .name = "user",
        .value = OsShapeField::CPU_USER,
    },
    {
        .name = "nice",
        .value = OsShapeField::CPU_NICE,
    },
    {
        .name = "sys",
        .value = OsShapeField::CPU_SYS,
    },
    {
        .name = "idle",
        .value = OsShapeField::CPU_IDLE,
    },
    {
        .name = "irq",
        .value = OsShapeField::CPU_IRQ,
    },
    {
        .name = "model",
        .value = OsShapeField::CPU_MODEL,
    },
    {
        .name = "speed",
        .value = OsShapeField::CPU_SPEED,
    },
    {
        .name = "times",
        .value = OsShapeField::CPU_TIMES,
    },
    {
        .name = "uid",
        .value = OsShapeField::USER_ID,
    },
    {
        .name = "gid",
        .value = OsShapeField::GROUP_ID,
    },
    {
        .name = "username",
        .value = OsShapeField::USER_NAME,
    },
    {
        .name = "homedir",
        .value = OsShapeField::HOME_DIRECTORY,
    },
    {
        .name = "shell",
        .value = OsShapeField::SHELL,
    },
};

}

std::optional<OsShapeField> OsShapeFieldTable::find(llvm::StringRef name) {
  for (const OsShapeFieldInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef OsShapeFieldTable::getName(OsShapeField value) {
  for (const OsShapeFieldInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("os shape field missing from the table");
}

}
