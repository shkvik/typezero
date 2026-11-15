#pragma once

#include "compiler/semantic/declaration/classes/declaration_classes_registry.h"
#include "compiler/semantic/declaration/files/declaration_files_registry.h"
#include "compiler/semantic/declaration/functions/declaration_functions_registry.h"
#include "compiler/semantic/declaration/links/declaration_links_registry.h"
#include "compiler/semantic/declaration/modules/declaration_modules_registry.h"
#include "compiler/semantic/declaration/names/declaration_names_registry.h"
#include "compiler/semantic/declaration/prelude/declaration_prelude_registry.h"
#include "compiler/semantic/declaration/types/declaration_types_registry.h"

namespace tz {

struct DeclarationRegistries {
  DeclarationFilesRegistry files;
  DeclarationLinksRegistry links;
  DeclarationNamesRegistry names;
  DeclarationTypesRegistry types;
  DeclarationClassesRegistry classes;
  DeclarationModulesRegistry modules;
  DeclarationPreludeRegistry prelude;
  DeclarationFunctionsRegistry functions;
};

}
