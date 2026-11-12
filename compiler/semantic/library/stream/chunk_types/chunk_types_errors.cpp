#include "compiler/semantic/library/stream/chunk_types/chunk_types_errors.h"
#include "compiler/program/types/types.h"

namespace tz {

std::string ErrorTextStreamHook::params(const FuncSymbol *hook) {
  auto params = std::string();
  for (size_t i = 0; i < hook->params.size(); ++i) {
    if (i) {
      params += ", ";
    }
    llvm::StringRef name = hook->decl != nullptr && i < hook->decl->params.size()
                               ? hook->decl->params[i].name
                               : llvm::StringRef("arg");
    params += name.str() + ": " + hook->params[i]->name().str();
  }

  return params;
}

std::string ErrorTextStreamHook::other(llvm::StringRef name, llvm::StringRef params, bool chunks) {
  auto tail = std::string();
  if (chunks) {
    tail = "; for chunks of another type, give the class it extends type arguments, e.g. "
           "'extends Writable<string>'";
  }

  return "'" + name.str() + "' is what node:stream calls, as '" + name.str() + "(" + params.str() +
         ")', and a method with other parameters is another "
         "variant of the name, which it never calls; take those parameters, "
         "or leave out their types" +
         tail;
}

}
