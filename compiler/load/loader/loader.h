#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"

#include "llvm/Support/SourceMgr.h"

namespace tz {

namespace pkg {
struct Graph;
}

// Reads the entry file of a program and every file it imports, directly or
// not, and joins them into one program: `program.modules` lists the files in
// the order they load, as in JavaScript (what a file imports first, in the
// order of its imports, then the file itself; each file once), and
// `program.items` holds their statements in that order. Import paths are
// relative: "./math" and "./math.tz" are math.tz, and a directory is its
// index.tz (docs/modules.md). A name that is not a path is
// a package of `packages`, the project of the entry file and what it depends
// on (docs/packages.md); without a tz.json there are none. Problems with
// imports are reported to `diags`; returns false only if the entry file
// cannot be read.
bool loadProgram(llvm::StringRef entryPath, llvm::SourceMgr &sourceMgr, AstContext &ctx,
                 Diagnostics &diags, SourceFile &program, const pkg::Graph *packages = nullptr);

} // namespace tz
