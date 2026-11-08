#include "compiler/semantic/library/reflect/metadata_checker/metadata_checker_errors.h"

namespace tz {

std::string ErrorTextMetadataType::unknown(llvm::StringRef label) {
  return "the records of '" + label.str() +
         "' have the type of the first one written, "
         "'Reflect.defineMetadata(" +
         label.str() +
         ", value, target)', and none is written "
         "before this; write one first, give the type where this one goes (e.g. "
         "'const x: string[] = ...'), or use a string key, whose records are 'unknown'";
}

}
