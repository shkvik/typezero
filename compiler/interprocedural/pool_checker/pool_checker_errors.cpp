#include "compiler/interprocedural/pool_checker/pool_checker_errors.h"

namespace tz {

std::string ErrorTextPoolObject::section() {
  return "(docs/async.md, section 11)";
}

std::string ErrorTextPoolObject::runStore() {
  return "a store that holds the object from a Pool is written as an "
         "object literal right in 'run', e.g. 'run({ connection }, "
         "fn)': then the compiler sees where the object goes " +
         ErrorTextPoolObject::section();
}

std::string ErrorTextPoolObject::awaitRun() {
  return "'run' with the object from a Pool in its store must be "
         "awaited (or returned) inside 'use': after 'use' the pool "
         "gives the object to another request, while the code inside "
         "'run' could still use it " +
         ErrorTextPoolObject::section();
}

std::string ErrorTextPoolObject::storeCopy() {
  return "this store holds the object from a Pool, so a copy of it "
         "would keep the object after 'use'; copy the fields you "
         "need instead " +
         ErrorTextPoolObject::section();
}

std::string ErrorTextPoolObject::voidCall() {
  return "'void' does not wait for this call, but it uses the object "
         "from a Pool, which 'use' gives to another request as soon as "
         "its function ends; await the call instead " +
         ErrorTextPoolObject::section();
}

std::string ErrorTextPoolObject::keptFn(llvm::StringRef where) {
  return "a function that uses the object from a Pool can't be "
         "kept " +
         where.str() +
         ": it could run after 'use', when the pool has given "
         "the object to another request; take what you need "
         "from the object first " +
         ErrorTextPoolObject::section();
}

std::string ErrorTextPoolObject::kept(llvm::StringRef where) {
  return "the object from a Pool lives only inside 'use' (after it "
         "the pool gives it to another request), so it can't be "
         "kept " +
         where.str() + " " + ErrorTextPoolObject::section();
}

std::string ErrorTextPoolObject::readsIt(llvm::StringRef through) {
  return through.empty() ? std::string("it reads the store with getStore()")
                         : "it reads the store through '" + through.str() + "'";
}

std::string ErrorTextPoolObject::voidStore(llvm::StringRef how) {
  return "'void' does not wait for this call, but " + how.str() +
         ", and this code can run inside 'run' whose store holds "
         "the object from a Pool, which 'use' gives to another "
         "request as soon as its function ends; await the call "
         "instead " +
         ErrorTextPoolObject::section();
}

std::string ErrorTextPoolObject::later(llvm::StringRef how) {
  return "this function runs later, but " + how.str() +
         ", and this code can run inside 'run' whose store holds the object "
         "from a Pool: by then 'use' may have given the object to another "
         "request; do the work before, or outside the transaction " +
         ErrorTextPoolObject::section();
}

std::string ErrorTextPoolObject::returned() {
  return "this function returns the object from the pool, but "
         "after 'use' the pool gives it to another request; return "
         "what you need from it instead " +
         ErrorTextPoolObject::section();
}

}
