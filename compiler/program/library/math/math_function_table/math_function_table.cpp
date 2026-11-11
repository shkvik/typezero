#include "compiler/program/library/math/math_function_table/math_function_table.h"

namespace tz {

namespace {

const MathFunctionInfo kMathFunctions[] = {
    {
        .name = "floor",
        .builtin = Builtin::MathFloor,
        .form = MathFunctionForm::ROUNDING,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "ceil",
        .builtin = Builtin::MathCeil,
        .form = MathFunctionForm::ROUNDING,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "round",
        .builtin = Builtin::MathRound,
        .form = MathFunctionForm::ROUNDING,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "trunc",
        .builtin = Builtin::MathTrunc,
        .form = MathFunctionForm::ROUNDING,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "abs",
        .builtin = Builtin::MathAbs,
        .form = MathFunctionForm::SAME,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "sign",
        .builtin = Builtin::MathSign,
        .form = MathFunctionForm::SAME,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "min",
        .builtin = Builtin::MathMin,
        .form = MathFunctionForm::MIN_MAX,
        .minArgs = 0,
        .maxArgs = MathFunctionTable::kAnyArgs,
    },
    {
        .name = "max",
        .builtin = Builtin::MathMax,
        .form = MathFunctionForm::MIN_MAX,
        .minArgs = 0,
        .maxArgs = MathFunctionTable::kAnyArgs,
    },
    {
        .name = "pow",
        .builtin = Builtin::MathPow,
        .form = MathFunctionForm::POW,
        .minArgs = 2,
        .maxArgs = 2,
    },
    {
        .name = "sqrt",
        .builtin = Builtin::MathSqrt,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "cbrt",
        .builtin = Builtin::MathCbrt,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "exp",
        .builtin = Builtin::MathExp,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "expm1",
        .builtin = Builtin::MathExpm1,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "log",
        .builtin = Builtin::MathLog,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "log1p",
        .builtin = Builtin::MathLog1p,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "log2",
        .builtin = Builtin::MathLog2,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "log10",
        .builtin = Builtin::MathLog10,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "sin",
        .builtin = Builtin::MathSin,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "cos",
        .builtin = Builtin::MathCos,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "tan",
        .builtin = Builtin::MathTan,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "asin",
        .builtin = Builtin::MathAsin,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "acos",
        .builtin = Builtin::MathAcos,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "atan",
        .builtin = Builtin::MathAtan,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "atan2",
        .builtin = Builtin::MathAtan2,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 2,
        .maxArgs = 2,
    },
    {
        .name = "sinh",
        .builtin = Builtin::MathSinh,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "cosh",
        .builtin = Builtin::MathCosh,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "tanh",
        .builtin = Builtin::MathTanh,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "asinh",
        .builtin = Builtin::MathAsinh,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "acosh",
        .builtin = Builtin::MathAcosh,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "atanh",
        .builtin = Builtin::MathAtanh,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "hypot",
        .builtin = Builtin::MathHypot,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 0,
        .maxArgs = MathFunctionTable::kAnyArgs,
    },
    {
        .name = "fround",
        .builtin = Builtin::MathFround,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "clz32",
        .builtin = Builtin::MathClz32,
        .form = MathFunctionForm::INT32,
        .minArgs = 1,
        .maxArgs = 1,
    },
    {
        .name = "imul",
        .builtin = Builtin::MathImul,
        .form = MathFunctionForm::INT32,
        .minArgs = 2,
        .maxArgs = 2,
    },
    {
        .name = "random",
        .builtin = Builtin::MathRandom,
        .form = MathFunctionForm::FLOAT,
        .minArgs = 0,
        .maxArgs = 0,
    },
    {
        .name = "clamp",
        .builtin = Builtin::MathClamp,
        .form = MathFunctionForm::CLAMP,
        .minArgs = 3,
        .maxArgs = 3,
    },
};

}

const MathFunctionInfo *MathFunctionTable::find(llvm::StringRef name) {
  for (const MathFunctionInfo &info : kMathFunctions) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
