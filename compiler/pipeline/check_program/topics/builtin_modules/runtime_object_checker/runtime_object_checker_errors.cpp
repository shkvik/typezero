#include "compiler/pipeline/check_program/topics/builtin_modules/runtime_object_checker/runtime_object_checker_errors.h"

namespace tz {

std::string ErrorTextRuntimeObject::members(Type::Runtime kind) {
  switch (kind) {
    case Type::Runtime::Readline:
    case Type::Runtime::ReadlinePromises:
      return "; it has 'question()' and 'close()', and 'for await (const line of rl)' gives the "
             "lines";
    case Type::Runtime::AsyncIterator:
      return "; iterate it with 'for await'";
    default:
      return "";
  }
}

std::string ErrorTextRuntimeObject::noMember(llvm::StringRef name, llvm::StringRef type,
                                             Type::Runtime kind) {
  return "property '" + name.str() + "' does not exist on type '" + type.str() + "'" +
         ErrorTextRuntimeObject::members(kind);
}

std::string ErrorTextRuntimeObject::assigned(llvm::StringRef name, llvm::StringRef type,
                                             Type::Runtime kind) {
  return "property '" + name.str() + "' of type '" + type.str() + "' cannot be assigned to" +
         ErrorTextRuntimeObject::members(kind);
}

std::string ErrorTextReadlineMethod::callback() {
  return "'question' of node:readline takes a callback for the answer, "
         "'rl.question(query, answer => ...)'; for a Promise, import "
         "'createInterface' from \"node:readline/promises\"";
}

std::string ErrorTextReadlineMethod::options() {
  return "the options of 'question' with a callback are not "
         "supported yet";
}

std::string ErrorTextReadlineMethod::events(llvm::StringRef name) {
  return "'" + name.str() +
         "' of readline is not supported yet: its Interface is no "
         "EventEmitter yet (docs/events.md); read the lines with "
         "'for await (const line of rl)' or ask with 'question()'";
}

std::string ErrorTextAsyncIterator::noThrow() {
  return "an iterator has no 'throw()' in tz: an error is a "
         "value, not something thrown into a generator";
}

std::string ErrorTextAsyncIterator::notYet(llvm::StringRef name) {
  return "'" + name.str() +
         "()' of an async iterator is not supported yet; "
         "iterate it with 'for await'";
}

}
