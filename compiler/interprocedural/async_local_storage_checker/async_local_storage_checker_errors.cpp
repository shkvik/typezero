#include "compiler/interprocedural/context_checker/context_checker_errors.h"

namespace tz {

std::string ErrorTextAsyncStore::section() {
  return "(docs/async.md, section 10)";
}

std::string ErrorTextAsyncStore::kept(llvm::StringRef where) {
  return "the store of an AsyncLocalStorage lives only inside "
         "'run', so it can't be kept " +
         where.str() + ": copy the fields you need instead " + ErrorTextAsyncStore::section();
}

std::string ErrorTextAsyncStore::made() {
  return "an AsyncLocalStorage is made only as a 'const' at the top "
         "level of a module, e.g. 'export const requestContext = new "
         "AsyncLocalStorage<RequestContext>()' " +
         ErrorTextAsyncStore::section();
}

std::string ErrorTextAsyncStore::runStore() {
  return "the store given to 'run' is a new object written right "
         "here, e.g. 'run({ requestId }, ...)': an object that "
         "already exists could get into two runs at once " +
         ErrorTextAsyncStore::section();
}

std::string ErrorTextAsyncStore::readsIt(llvm::StringRef through) {
  return through.empty() ? std::string("this function reads it with getStore()")
                         : "this function reads it through '" + through.str() + "'";
}

std::string ErrorTextAsyncStore::interval(llvm::StringRef how) {
  return "setInterval does not carry the store of an AsyncLocalStorage (an "
         "interval outlives 'run'), but " +
         how.str() +
         "; read what it needs before setInterval and keep it in the "
         "interval's object " +
         ErrorTextAsyncStore::section();
}

std::string ErrorTextAsyncStore::restart(llvm::StringRef how) {
  return "a setTimeout that restarts itself does not carry the store of an "
         "AsyncLocalStorage (like an interval, it outlives 'run'), but " +
         how.str() + "; read what it needs before and keep it in a field " +
         ErrorTextAsyncStore::section();
}

}
