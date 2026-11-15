#pragma once

#include "compiler/semantic/library_v2/async/library_async.h"
#include "compiler/semantic/library_v2/bytes/library_bytes.h"
#include "compiler/semantic/library_v2/common/library_common.h"
#include "compiler/semantic/library_v2/crypto/library_crypto.h"
#include "compiler/semantic/library_v2/dispatch/library_modules.h"
#include "compiler/semantic/library_v2/events/library_emitters.h"
#include "compiler/semantic/library_v2/intl/library_intl.h"
#include "compiler/semantic/library_v2/json/library_json.h"
#include "compiler/semantic/library_v2/reflect/library_reflect.h"
#include "compiler/semantic/library_v2/result/library_result.h"
#include "compiler/semantic/library_v2/tz_modules/library_tz_modules.h"
#include "compiler/semantic/semantic_state/v2/library/bytes_context/bytes_context.h"
#include "compiler/semantic/semantic_state/v2/library/event_context/event_context.h"
#include "compiler/semantic/semantic_state/v2/library/file_system_context/file_system_context.h"
#include "compiler/semantic/semantic_state/v2/library/library_declaration_context/library_declaration_context.h"
#include "compiler/semantic/semantic_state/v2/library/reflection_context/reflection_context.h"
#include "compiler/semantic/semantic_state/v2/library/stream_context/stream_context.h"

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
  LibraryModules modules;
};

}
