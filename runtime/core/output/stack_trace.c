#include "runtime/core/output/stack_trace.h"
#include "runtime/core/output/output.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

enum { kMaxFrames = 64, kMaxShown = 24 };

typedef DWORD(WINAPI *SymSetOptionsFn)(DWORD);
typedef BOOL(WINAPI *SymInitializeWFn)(HANDLE, PCWSTR, BOOL);
typedef BOOL(WINAPI *SymCleanupFn)(HANDLE);
typedef DWORD64(WINAPI *SymLoadModuleExWFn)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD,
                                            PMODLOAD_DATA, DWORD);
typedef BOOL(WINAPI *SymFromAddrWFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFOW);
typedef BOOL(WINAPI *SymGetLineFromAddrW64Fn)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINEW64);
typedef DWORD(WINAPI *SymAddrIncludeInlineTraceFn)(HANDLE, DWORD64);
typedef BOOL(WINAPI *SymQueryInlineTraceFn)(HANDLE, DWORD64, DWORD, DWORD64, DWORD64, LPDWORD,
                                            LPDWORD);
typedef BOOL(WINAPI *SymFromInlineContextWFn)(HANDLE, DWORD64, ULONG, PDWORD64, PSYMBOL_INFOW);
typedef BOOL(WINAPI *SymGetLineFromInlineContextWFn)(HANDLE, DWORD64, ULONG, DWORD64, PDWORD,
                                                     PIMAGEHLP_LINEW64);

typedef struct {
  DWORD64 frames[kMaxFrames];
  int count;
  int exact;
  uintptr_t base;
  uintptr_t end;
  int shown;
  int repeats;
  size_t lastLength;
  char last[1600];
  HANDLE process;
  SymFromAddrWFn fromAddr;
  SymGetLineFromAddrW64Fn lineFromAddr;
  SymAddrIncludeInlineTraceFn inlineCount;
  SymQueryInlineTraceFn inlineQuery;
  SymFromInlineContextWFn fromInline;
  SymGetLineFromInlineContextWFn lineFromInline;
  union {
    SYMBOL_INFOW info;
    char space[sizeof(SYMBOL_INFOW) + 512 * sizeof(WCHAR)];
  } symbol;
} Trace;

static void put(const char *text, size_t len) { tz_err_raw(text, len); }

static size_t utf8_of(const WCHAR *text, char *out, size_t size) {
  int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, (int)size, NULL, NULL);
  if (n <= 0) {
    out[0] = 0;
    return 0;
  }

  return (size_t)n - 1;
}

static int is_tz_file(const WCHAR *file) {
  size_t n = wcslen(file);

  return n > 3 && _wcsicmp(file + n - 3, L".tz") == 0;
}

static void flush_repeats(Trace *trace) {
  if (trace->repeats == 0) {
    return;
  }
  char text[80];
  int n =
      snprintf(text, sizeof text, "    [previous line repeated %d more times]\n", trace->repeats);
  if (n > 0) {
    put(text, (size_t)n);
  }
  trace->repeats = 0;
}

static void show(Trace *trace, const WCHAR *name, const IMAGEHLP_LINEW64 *line) {
  if (!is_tz_file(line->FileName) || trace->shown > kMaxShown) {
    return;
  }
  char text[1600], function[512], file[1024];
  utf8_of(name, function, sizeof function);
  utf8_of(line->FileName, file, sizeof file);
  int n =
      function[0] == '<' || function[0] == 0
          ? snprintf(text, sizeof text, "    at %s:%lu\n", file, line->LineNumber)
          : snprintf(text, sizeof text, "    at %s (%s:%lu)\n", function, file, line->LineNumber);
  if (n <= 0) {
    return;
  }
  size_t length = (size_t)n < sizeof text ? (size_t)n : sizeof text - 1;
  if (length == trace->lastLength && memcmp(text, trace->last, length) == 0) {
    ++trace->repeats;
    return;
  }
  flush_repeats(trace);
  if (trace->shown == kMaxShown) {
    put("    ...\n", 8);
    ++trace->shown;
    return;
  }
  put(text, length);
  memcpy(trace->last, text, length);
  trace->lastLength = length;
  ++trace->shown;
}

static void symbol_reset(Trace *trace) {
  memset(&trace->symbol.info, 0, sizeof trace->symbol.info);
  trace->symbol.info.SizeOfStruct = sizeof(SYMBOL_INFOW);
  trace->symbol.info.MaxNameLen = 512;
}

static void show_address(Trace *trace, DWORD64 address) {
  DWORD64 displacement = 0;
  DWORD lineDisplacement = 0;
  IMAGEHLP_LINEW64 line;
  DWORD inlines = trace->inlineCount ? trace->inlineCount(trace->process, address) : 0;
  DWORD context = 0, index = 0;
  if (inlines > 0 &&
      trace->inlineQuery(trace->process, address, 0, address, address, &context, &index)) {
    for (DWORD i = 0; i < inlines; ++i, ++context) {
      symbol_reset(trace);
      memset(&line, 0, sizeof line);
      line.SizeOfStruct = sizeof line;
      if (trace->fromInline(trace->process, address, context, &displacement, &trace->symbol.info) &&
          trace->lineFromInline(trace->process, address, context, 0, &lineDisplacement, &line)) {
        show(trace, trace->symbol.info.Name, &line);
      }
    }
  }
  symbol_reset(trace);
  memset(&line, 0, sizeof line);
  line.SizeOfStruct = sizeof line;
  if (trace->fromAddr(trace->process, address, &displacement, &trace->symbol.info) &&
      trace->lineFromAddr(trace->process, address, &lineDisplacement, &line)) {
    show(trace, trace->symbol.info.Name, &line);
  }
}

static DWORD WINAPI symbolize(void *arg) {
  Trace *trace = arg;
  HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!dbghelp) {
    return 0;
  }
  SymSetOptionsFn setOptions = (SymSetOptionsFn)(void *)GetProcAddress(dbghelp, "SymSetOptions");
  SymInitializeWFn initialize = (SymInitializeWFn)(void *)GetProcAddress(dbghelp, "SymInitializeW");
  SymCleanupFn cleanup = (SymCleanupFn)(void *)GetProcAddress(dbghelp, "SymCleanup");
  SymLoadModuleExWFn loadModule =
      (SymLoadModuleExWFn)(void *)GetProcAddress(dbghelp, "SymLoadModuleExW");
  trace->fromAddr = (SymFromAddrWFn)(void *)GetProcAddress(dbghelp, "SymFromAddrW");
  trace->lineFromAddr =
      (SymGetLineFromAddrW64Fn)(void *)GetProcAddress(dbghelp, "SymGetLineFromAddrW64");
  trace->inlineCount =
      (SymAddrIncludeInlineTraceFn)(void *)GetProcAddress(dbghelp, "SymAddrIncludeInlineTrace");
  trace->inlineQuery =
      (SymQueryInlineTraceFn)(void *)GetProcAddress(dbghelp, "SymQueryInlineTrace");
  trace->fromInline =
      (SymFromInlineContextWFn)(void *)GetProcAddress(dbghelp, "SymFromInlineContextW");
  trace->lineFromInline = (SymGetLineFromInlineContextWFn)(void *)GetProcAddress(
      dbghelp, "SymGetLineFromInlineContextW");
  if (!trace->inlineQuery || !trace->fromInline || !trace->lineFromInline) {
    trace->inlineCount = NULL;
  }
  if (!setOptions || !initialize || !cleanup || !loadModule || !trace->fromAddr ||
      !trace->lineFromAddr) {
    return 0;
  }
  WCHAR exe[MAX_PATH], dir[MAX_PATH];
  DWORD length = GetModuleFileNameW(NULL, exe, MAX_PATH);
  if (length == 0 || length == MAX_PATH) {
    return 0;
  }
  wcscpy(dir, exe);
  WCHAR *slash = wcsrchr(dir, L'\\');
  if (slash) {
    *slash = 0;
  }
  trace->process = GetCurrentProcess();
  setOptions(SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_UNDNAME);
  if (!initialize(trace->process, dir, FALSE)) {
    return 0;
  }
  if (loadModule(trace->process, NULL, exe, NULL, trace->base, (DWORD)(trace->end - trace->base),
                 NULL, 0)) {
    for (int i = 0; i < trace->count; ++i) {
      DWORD64 address = trace->frames[i];
      if (address <= trace->base || address >= trace->end) {
        continue;
      }
      show_address(trace, i == 0 && trace->exact ? address : address - 1);
    }
    flush_repeats(trace);
    if (trace->shown > 0 && trace->shown <= kMaxShown && trace->count == kMaxFrames) {
      put("    ...\n", 8);
    }
  }
  cleanup(trace->process);

  return 0;
}

static int walk(CONTEXT *context, DWORD64 *frames, int max) {
  int n = 0;
#if defined(_M_X64) || defined(__x86_64__)
  while (n < max && context->Rip != 0) {
    frames[n++] = context->Rip;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context->Rip, &base, NULL);
    if (!entry) {
      context->Rip = *(DWORD64 *)context->Rsp;
      context->Rsp += 8;
      continue;
    }
    void *handler = NULL;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context->Rip, entry, context, &handler, &frame, NULL);
  }
#else
  (void)context;
  n = RtlCaptureStackBackTrace(0, (DWORD)max, (void **)frames, NULL);
#endif

  return n;
}

Trace tz_stack_trace;
volatile LONG tz_stack_busy;

void tz_stack_print(const void *context) {
  Trace *trace = &tz_stack_trace;
  if (InterlockedExchange(&tz_stack_busy, 1)) {
    return;
  }
  CONTEXT copy;
  if (context) {
    memcpy(&copy, context, sizeof copy);
    trace->exact = 1;
  } else {
    RtlCaptureContext(&copy);
    trace->exact = 0;
  }
  HMODULE base = GetModuleHandleW(NULL);
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  trace->base = (uintptr_t)base;
  trace->end = trace->base + nt->OptionalHeader.SizeOfImage;
  trace->count = walk(&copy, trace->frames, kMaxFrames);
  trace->shown = 0;
  trace->repeats = 0;
  trace->lastLength = 0;
  HANDLE thread = CreateThread(NULL, 1 << 20, symbolize, trace, 0, NULL);
  if (thread) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
  }
  if (trace->shown > 0) {
    return;
  }
  WCHAR exe[MAX_PATH];
  char name[MAX_PATH * 3];
  DWORD length = GetModuleFileNameW(NULL, exe, MAX_PATH);
  const WCHAR *file = exe;
  if (length > 0 && length < MAX_PATH) {
    const WCHAR *slash = wcsrchr(exe, L'\\');
    file = slash ? slash + 1 : exe;
  } else {
    exe[0] = 0;
  }
  utf8_of(file, name, sizeof name);
  if (exe[0] != 0) {
    WCHAR pdb[MAX_PATH];
    wcscpy(pdb, exe);
    WCHAR *dot = wcsrchr(pdb, L'.');
    if (dot && dot > pdb + (file - exe)) {
      wcscpy(dot, L".pdb");
    }
    if (GetFileAttributesW(pdb) == INVALID_FILE_ATTRIBUTES) {
      char hint[MAX_PATH * 6 + 64], pdbName[MAX_PATH * 3];
      utf8_of(pdb + (file - exe), pdbName, sizeof pdbName);
      int n =
          snprintf(hint, sizeof hint, "    (no %s next to %s: only addresses)\n", pdbName, name);
      if (n > 0) {
        put(hint, (size_t)n < sizeof hint ? (size_t)n : sizeof hint - 1);
      }
    }
  }
  int shown = 0;
  for (int i = 0; i < trace->count && shown < kMaxShown; ++i) {
    DWORD64 address = trace->frames[i];
    if (address <= trace->base || address >= trace->end) {
      continue;
    }
    char text[MAX_PATH * 3 + 48];
    int n = snprintf(text, sizeof text, "    at %s+0x%llx\n", name,
                     (unsigned long long)(address - trace->base));
    if (n > 0) {
      put(text, (size_t)n);
    }
    ++shown;
  }
}

#else

void tz_stack_print(const void *context) { (void)context; }

#endif
