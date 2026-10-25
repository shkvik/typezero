#pragma once

#include "pkg/project.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace tz::pkg {

// The editor's view of a project (docs/modules.md, section 6): TypeScript's
// checker in the editor (tsc in VS Code) reads the project's tsconfig.json,
// which `tz init` writes and which extends .tz/tsconfig.json. tz writes that
// one, and writes it again whenever it builds the project or changes its
// packages: it extends the settings next to tz (types/tsconfig.json, from
// src/types) and says where the packages of the project are in the cache.
// Its paths are those of this computer, so git does not keep it.

// The directory of the declarations next to tz; the driver sets it.
void setTypesDir(std::string dir);

// What `tz init` writes as the project's tsconfig.json.
extern const char kProjectTsconfig[];

// Whether the project in `dir` has its editor set up by tz: its
// tsconfig.json extends .tz/tsconfig.json.
bool editorSetUp(llvm::StringRef dir);

// Writes `dir`/.tz/tsconfig.json for the project with the packages of
// `graph` (none if it is null), unless it holds that already. False after
// reporting an error.
bool writeEditorConfig(llvm::StringRef dir, const Graph *graph);

} // namespace tz::pkg
