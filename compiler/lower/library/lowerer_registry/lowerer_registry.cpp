#include "compiler/lower/library/lowerer_registry/lowerer_registry.h"
#include "compiler/lower/library/bytes/bytes_library_lowerer/bytes_library_lowerer.h"
#include "compiler/lower/library/child_process/child_process_library_lowerer/child_process_library_lowerer.h"
#include "compiler/lower/library/console/console_library_lowerer/console_library_lowerer.h"
#include "compiler/lower/library/crypto/crypto_library_lowerer/crypto_library_lowerer.h"
#include "compiler/lower/library/date/date_library_lowerer/date_library_lowerer.h"
#include "compiler/lower/library/decimal/decimal_library_lowerer/decimal_library_lowerer.h"
#include "compiler/lower/library/fs/fs_library_lowerer/fs_library_lowerer.h"
#include "compiler/lower/library/intl/intl_library_lowerer/intl_library_lowerer.h"
#include "compiler/lower/library/json/json_library_lowerer/json_library_lowerer.h"
#include "compiler/lower/library/math/math_library_lowerer/math_library_lowerer.h"
#include "compiler/lower/library/os/os_library_lowerer/os_library_lowerer.h"
#include "compiler/lower/library/path/path_library_lowerer/path_library_lowerer.h"
#include "compiler/lower/library/process/process_library_lowerer/process_library_lowerer.h"
#include "compiler/lower/library/reflect/reflect_library_lowerer/reflect_library_lowerer.h"
#include "compiler/lower/library/regex/regex_library_lowerer/regex_library_lowerer.h"
#include "compiler/lower/library/result/result_library_lowerer/result_library_lowerer.h"
#include "compiler/lower/library/tz_modules/tz_modules_library_lowerer/tz_modules_library_lowerer.h"
#include "compiler/lower/library/weakref/weakref_library_lowerer/weakref_library_lowerer.h"

namespace tz {

LowererRegistry::LowererRegistry() {
  this->lowerers.push_back(std::make_unique<TzModulesLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<ResultLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<JsonLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<ReflectLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<ConsoleLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<ProcessLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<MathLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<RegexLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<DateLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<IntlLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<DecimalLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<CryptoLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<WeakRefLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<BytesLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<PathLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<OsLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<FsLibraryLowerer>());
  this->lowerers.push_back(std::make_unique<ChildProcessLibraryLowerer>());
}

const LibraryLowerer *LowererRegistry::find(Builtin builtin) const {
  for (const std::unique_ptr<LibraryLowerer> &lowerer : this->lowerers) {
    if (lowerer->lowers(builtin)) {
      return lowerer.get();
    }
  }

  return nullptr;
}

}
