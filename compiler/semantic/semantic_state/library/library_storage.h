#pragma once

#include "compiler/semantic/library/async/library_async.h"
#include "compiler/semantic/library/bytes/library_bytes.h"
#include "compiler/semantic/library/common/library_common.h"
#include "compiler/semantic/library/crypto/library_crypto.h"
#include "compiler/semantic/library/dispatch/library_builtin_modules.h"
#include "compiler/semantic/library/events/library_emitters.h"
#include "compiler/semantic/library/intl/library_intl.h"
#include "compiler/semantic/library/json/library_json.h"
#include "compiler/semantic/library/reflect/library_reflect.h"
#include "compiler/semantic/library/result/library_result.h"
#include "compiler/semantic/library/tz_modules/library_tz_modules.h"
#include "compiler/semantic/semantic_state/library/bytes_context/bytes_context.h"
#include "compiler/semantic/semantic_state/library/event_context/event_context.h"
#include "compiler/semantic/semantic_state/library/file_system_context/file_system_context.h"
#include "compiler/semantic/semantic_state/library/library_declaration_context/library_declaration_context.h"
#include "compiler/semantic/semantic_state/library/reflection_context/reflection_context.h"
#include "compiler/semantic/semantic_state/library/stream_context/stream_context.h"

namespace tz {

struct LibraryStorage {
  BytesContext decoder;
  EventContext events;
  StreamContext streams;
  FileSystemContext filesystem;
  ReflectionContext reflection;
  LibraryDeclarationContext declarations;
  LibraryCommon common;
  LibraryJson json;
  LibraryResult result;
  LibraryIntl intl;
  LibraryAsync async;
  LibraryEmitters emitters;
  LibraryReflect reflect;
  LibraryBytes bytes;
  LibraryCrypto crypto;
  LibraryTzModules tzModules;
  LibraryBuiltinModules modules;
};

}
