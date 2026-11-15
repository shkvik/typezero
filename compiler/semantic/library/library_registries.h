#pragma once

#include "compiler/semantic/library/async/library_async_registry.h"
#include "compiler/semantic/library/bytes/library_bytes_registry.h"
#include "compiler/semantic/library/child_process/library_child_process_registry.h"
#include "compiler/semantic/library/common/library_common_registry.h"
#include "compiler/semantic/library/console/library_console_registry.h"
#include "compiler/semantic/library/crypto/library_crypto_registry.h"
#include "compiler/semantic/library/date/library_date_registry.h"
#include "compiler/semantic/library/decimal/library_decimal_registry.h"
#include "compiler/semantic/library/dispatch/library_dispatch_registry.h"
#include "compiler/semantic/library/events/library_events_registry.h"
#include "compiler/semantic/library/fs/library_fs_registry.h"
#include "compiler/semantic/library/intl/library_intl_registry.h"
#include "compiler/semantic/library/json/library_json_registry.h"
#include "compiler/semantic/library/math/library_math_registry.h"
#include "compiler/semantic/library/os/library_os_registry.h"
#include "compiler/semantic/library/path/library_path_registry.h"
#include "compiler/semantic/library/process/library_process_registry.h"
#include "compiler/semantic/library/reflect/library_reflect_registry.h"
#include "compiler/semantic/library/regex/library_regex_registry.h"
#include "compiler/semantic/library/result/library_result_registry.h"
#include "compiler/semantic/library/stream/library_stream_registry.h"
#include "compiler/semantic/library/tz_modules/library_tz_modules_registry.h"
#include "compiler/semantic/library/values/library_values_registry.h"
#include "compiler/semantic/library/weakref/library_weakref_registry.h"

namespace tz {

struct LibraryRegistries {
  LibraryFsRegistry fs;
  LibraryOsRegistry os;
  LibraryDateRegistry date;
  LibraryIntlRegistry intl;
  LibraryJsonRegistry json;
  LibraryMathRegistry math;
  LibraryPathRegistry path;
  LibraryAsyncRegistry async;
  LibraryBytesRegistry bytes;
  LibraryRegexRegistry regex;
  LibraryCommonRegistry common;
  LibraryCryptoRegistry crypto;
  LibraryEventsRegistry events;
  LibraryResultRegistry result;
  LibraryStreamRegistry stream;
  LibraryValuesRegistry values;
  LibraryConsoleRegistry console;
  LibraryDecimalRegistry decimal;
  LibraryProcessRegistry process;
  LibraryReflectRegistry reflect;
  LibraryWeakrefRegistry weakref;
  LibraryDispatchRegistry dispatch;
  LibraryTzModulesRegistry tzModules;
  LibraryChildProcessRegistry childProcess;
};

}
