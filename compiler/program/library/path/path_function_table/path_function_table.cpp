#include "compiler/program/library/path/path_function_table/path_function_table.h"

namespace tz {

namespace {

const PathFunctionInfo kPathFunctions[] = {
    {
        .name = "join",
        .builtin = Builtin::PathJoin,
        .form = PathFunctionForm::JOINED,
        .min = 0,
        .max = 0,
    },
    {
        .name = "resolve",
        .builtin = Builtin::PathResolve,
        .form = PathFunctionForm::JOINED,
        .min = 0,
        .max = 0,
    },
    {
        .name = "normalize",
        .builtin = Builtin::PathNormalize,
        .form = PathFunctionForm::TEXT,
        .min = 1,
        .max = 1,
    },
    {
        .name = "isAbsolute",
        .builtin = Builtin::PathIsAbsolute,
        .form = PathFunctionForm::TEST,
        .min = 1,
        .max = 1,
    },
    {
        .name = "dirname",
        .builtin = Builtin::PathDirname,
        .form = PathFunctionForm::TEXT,
        .min = 1,
        .max = 1,
    },
    {
        .name = "basename",
        .builtin = Builtin::PathBasename,
        .form = PathFunctionForm::TEXT,
        .min = 1,
        .max = 2,
    },
    {
        .name = "extname",
        .builtin = Builtin::PathExtname,
        .form = PathFunctionForm::TEXT,
        .min = 1,
        .max = 1,
    },
    {
        .name = "relative",
        .builtin = Builtin::PathRelative,
        .form = PathFunctionForm::TEXT,
        .min = 2,
        .max = 2,
    },
    {
        .name = "parse",
        .builtin = Builtin::PathParse,
        .form = PathFunctionForm::PARSED,
        .min = 1,
        .max = 1,
    },
    {
        .name = "format",
        .builtin = Builtin::PathFormat,
        .form = PathFunctionForm::FORMAT,
        .min = 1,
        .max = 1,
    },
    {
        .name = "toNamespacedPath",
        .builtin = Builtin::PathToNamespacedPath,
        .form = PathFunctionForm::TEXT,
        .min = 1,
        .max = 1,
    },
};

}

const PathFunctionInfo *PathFunctionTable::find(llvm::StringRef name) {
  for (const PathFunctionInfo &info : kPathFunctions) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
