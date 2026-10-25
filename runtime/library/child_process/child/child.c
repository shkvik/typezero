// node:child_process (docs/process.md): spawnSync, execFileSync and
// execSync, as Node runs them.
//
// Node prepares the call in JavaScript (lib/child_process.js: the shell, the
// environment, the checks of the options), and libuv starts the program
// (deps/uv/src/win/process.c) while src/spawn_sync.cc collects what it
// prints. The same steps are here, in the same order, so that the program
// gets the same command line, the same environment and the same handles as
// from Node, and the errors are Node's: the same codes and numbers, and the
// same texts for wrong options.
//
// On Windows the parent's ends of the pipes are named pipes opened for
// overlapped I/O, as libuv makes them, and one loop waits for the process,
// each read of stdout and stderr, the write of the input and the timeout at
// once: both outputs are read while the program runs, so neither fills up
// and blocks it. Only the handles the child is meant to get are inherited
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST), so that another thread starting a
// program at the same time cannot pass its pipes to this one. On other
// systems posix_spawn starts the program and poll() does the same.
//
// What the call gives is made by functions of the program (TzMaker, see
// CodeGen::spawnMaker): the result of spawnSync, an ExecSyncError, or a
// SystemError when the program could not start.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
// TODO(linux): tz builds programs for Windows only so far, so this half has
// run only under a harness of requests made by hand (gcc, glibc). It follows
// libuv's src/unix/process.c, with posix_spawn.
#define _GNU_SOURCE // posix_spawn_file_actions_addchdir_np, pipe2
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

#include "tzrt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// What the program gives and gets (must match src/codegen/codegen_child.cpp)
//===----------------------------------------------------------------------===//

// The call, as Sema checked it (see ChildArg in src/ast/ast.h). Strings,
// the array, the map and the bytes are borrowed.
typedef struct {
  TzStr file; // the program; the command of execSync
  TzArray *args;
  TzStr cwd, input_text, kill_name, shell, argv0;
  TzMap *env;           // NULL: the environment of this process
  TzBytes *input_bytes; // a view, when the input is bytes
  const TzType *string_type;
  double timeout, max_buffer;
  int32_t mode;  // TZ_CHILD_SPAWN...
  int32_t has;   // which options the call gave: HAS_CWD...
  int32_t input; // INPUT_NONE, INPUT_TEXT or INPUT_BYTES
  int32_t kill_number;
  int32_t shell_kind; // SHELL_NONE, SHELL_SYSTEM or SHELL_PATH
  int32_t windows_hide, verbatim, detached;
  int32_t uid, gid;
  int32_t stdio[3]; // as ChildArg: 0 a pipe, 1 ignored, 2 + the fd inherited
  int32_t encoding; // 0 for Buffers, 1 + the number of a Buffer encoding
} TzSpawnRequest;

enum { TZ_CHILD_SPAWN = 0, TZ_CHILD_EXEC_FILE = 1, TZ_CHILD_EXEC = 2 };
enum {
  HAS_CWD = 1,
  HAS_TIMEOUT = 2,
  HAS_MAX_BUFFER = 4,
  HAS_KILL_NAME = 8,
  HAS_KILL_NUMBER = 16,
  HAS_ARGV0 = 32,
  HAS_UID = 64,
  HAS_GID = 128,
  HAS_STDIO = 256,
};
enum { INPUT_NONE, INPUT_TEXT, INPUT_BYTES };
enum { SHELL_NONE, SHELL_SYSTEM, SHELL_PATH };
enum { STDIO_PIPE = 0, STDIO_IGNORE = 1, STDIO_INHERIT = 2 };

// The parts of what the call gives, which its maker takes over: the result
// of spawnSync, or an ExecSyncError, by the fields each has.
typedef struct {
  TzStr message, name; // of an ExecSyncError
  void *error;         // of the result of spawnSync: a SystemError, or NULL
  // An ExecSyncError of an error of the system (ETIMEDOUT...), with has_code.
  TzStr code, syscall, path;
  TzArray *spawnargs;
  TzStr signal;      // with has_signal
  TzStr text[3];     // what each stream printed, with an encoding
  TzBytes *bytes[3]; // or without one
  int32_t errno_value, has_code;
  int32_t status, has_status, has_signal;
  int32_t pid;
  int32_t text_mode; // whether the output is in `text`
  int32_t piped[3];  // whether the stream was a pipe the child wrote to
} TzSpawnParts;

//===----------------------------------------------------------------------===//
// Errors
//===----------------------------------------------------------------------===//

// The errors of libuv that starting a program can give, with the numbers
// Node gives for them: libuv's on Windows, -errno elsewhere.
typedef struct {
  const char *code;
  int32_t number;
} UvError;

#ifdef _WIN32
#define UV_NUMBER(windows, posix) (windows)
#else
#define UV_NUMBER(windows, posix) (posix)
#endif

enum {
  X_NONE = -1,
  X_UNKNOWN,
  X_ENOENT,
  X_EACCES,
  X_EPERM,
  X_EINVAL,
  X_E2BIG,
  X_ENOMEM,
  X_ETIMEDOUT,
  X_ENOBUFS,
  X_EPIPE,
  X_EOF,
  X_ENOTSUP,
  X_ENOSYS,
  X_ESRCH,
  X_EBADF,
  X_EAGAIN,
  X_EMFILE,
  X_ENAMETOOLONG,
  X_ENOTDIR,
  X_EIO,
  X_EFTYPE,
  X_EBUSY,
  X_ECANCELED,
  X_ECHARSET,
  X_EEXIST,
  X_EFAULT,
  X_EISDIR,
  X_ELOOP,
  X_ENOSPC,
  X_ENOTEMPTY,
  X_EROFS,
  X_EXDEV,
  X_ETXTBSY,
};

static const UvError kErrors[] = {
    {"UNKNOWN", -4094},
    {"ENOENT", UV_NUMBER(-4058, -ENOENT)},
    {"EACCES", UV_NUMBER(-4092, -EACCES)},
    {"EPERM", UV_NUMBER(-4048, -EPERM)},
    {"EINVAL", UV_NUMBER(-4071, -EINVAL)},
    {"E2BIG", UV_NUMBER(-4093, -E2BIG)},
    {"ENOMEM", UV_NUMBER(-4057, -ENOMEM)},
    {"ETIMEDOUT", UV_NUMBER(-4039, -ETIMEDOUT)},
    {"ENOBUFS", UV_NUMBER(-4060, -ENOBUFS)},
    {"EPIPE", UV_NUMBER(-4047, -EPIPE)},
    {"EOF", -4095},
    {"ENOTSUP", UV_NUMBER(-4049, -ENOTSUP)},
    {"ENOSYS", UV_NUMBER(-4054, -ENOSYS)},
    {"ESRCH", UV_NUMBER(-4040, -ESRCH)},
    {"EBADF", UV_NUMBER(-4083, -EBADF)},
    {"EAGAIN", UV_NUMBER(-4088, -EAGAIN)},
    {"EMFILE", UV_NUMBER(-4066, -EMFILE)},
    {"ENAMETOOLONG", UV_NUMBER(-4064, -ENAMETOOLONG)},
    {"ENOTDIR", UV_NUMBER(-4052, -ENOTDIR)},
    {"EIO", UV_NUMBER(-4070, -EIO)},
    {"EFTYPE", -4028},
    {"EBUSY", UV_NUMBER(-4082, -EBUSY)},
    {"ECANCELED", UV_NUMBER(-4081, -ECANCELED)},
    {"ECHARSET", -4080},
    {"EEXIST", UV_NUMBER(-4075, -EEXIST)},
    {"EFAULT", UV_NUMBER(-4074, -EFAULT)},
    {"EISDIR", UV_NUMBER(-4068, -EISDIR)},
    {"ELOOP", UV_NUMBER(-4067, -ELOOP)},
    {"ENOSPC", UV_NUMBER(-4055, -ENOSPC)},
    {"ENOTEMPTY", UV_NUMBER(-4051, -ENOTEMPTY)},
    {"EROFS", UV_NUMBER(-4043, -EROFS)},
    {"EXDEV", UV_NUMBER(-4037, -EXDEV)},
    {"ETXTBSY", UV_NUMBER(-4038, -ETXTBSY)},
};

#ifdef _WIN32
// An error of Windows as libuv translates it (uv_translate_sys_error).
static int win32_error(DWORD error) {
  switch (error) {
  case ERROR_ELEVATION_REQUIRED:
  case ERROR_CANT_ACCESS_FILE:
    return X_EACCES;
  case ERROR_NO_DATA:
    return X_EAGAIN;
  case ERROR_INVALID_FLAGS:
  case ERROR_INVALID_HANDLE:
    return X_EBADF;
  case ERROR_LOCK_VIOLATION:
  case ERROR_PIPE_BUSY:
  case ERROR_SHARING_VIOLATION:
    return X_EBUSY;
  case ERROR_OPERATION_ABORTED:
    return X_ECANCELED;
  case ERROR_NO_UNICODE_TRANSLATION:
    return X_ECHARSET;
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
    return X_EEXIST;
  case ERROR_NOACCESS:
    return X_EFAULT;
  case ERROR_INSUFFICIENT_BUFFER:
  case ERROR_INVALID_DATA:
  case ERROR_INVALID_PARAMETER:
  case ERROR_SYMLINK_NOT_SUPPORTED:
    return X_EINVAL;
  case ERROR_BEGINNING_OF_MEDIA:
  case ERROR_BUS_RESET:
  case ERROR_CRC:
  case ERROR_DEVICE_DOOR_OPEN:
  case ERROR_DEVICE_REQUIRES_CLEANING:
  case ERROR_DISK_CORRUPT:
  case ERROR_EOM_OVERFLOW:
  case ERROR_FILEMARK_DETECTED:
  case ERROR_GEN_FAILURE:
  case ERROR_INVALID_BLOCK_LENGTH:
  case ERROR_IO_DEVICE:
  case ERROR_NO_DATA_DETECTED:
  case ERROR_NO_SIGNAL_SENT:
  case ERROR_OPEN_FAILED:
  case ERROR_SETMARK_DETECTED:
  case ERROR_SIGNAL_REFUSED:
    return X_EIO;
  case ERROR_CANT_RESOLVE_FILENAME:
    return X_ELOOP;
  case ERROR_TOO_MANY_OPEN_FILES:
    return X_EMFILE;
  case ERROR_BUFFER_OVERFLOW:
  case ERROR_FILENAME_EXCED_RANGE:
    return X_ENAMETOOLONG;
  case ERROR_BAD_PATHNAME:
  case ERROR_DIRECTORY:
  case ERROR_ENVVAR_NOT_FOUND:
  case ERROR_FILE_NOT_FOUND:
  case ERROR_INVALID_NAME:
  case ERROR_INVALID_DRIVE:
  case ERROR_INVALID_REPARSE_DATA:
  case ERROR_MOD_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
    return X_ENOENT;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
    return X_ENOMEM;
  case ERROR_CANNOT_MAKE:
  case ERROR_DISK_FULL:
  case ERROR_EA_TABLE_FULL:
  case ERROR_END_OF_MEDIA:
  case ERROR_HANDLE_DISK_FULL:
    return X_ENOSPC;
  case ERROR_DIR_NOT_EMPTY:
    return X_ENOTEMPTY;
  case ERROR_NOT_SUPPORTED:
    return X_ENOTSUP;
  case ERROR_BROKEN_PIPE:
    return X_EOF;
  case ERROR_ACCESS_DENIED:
  case ERROR_PRIVILEGE_NOT_HELD:
    return X_EPERM;
  case ERROR_BAD_PIPE:
  case ERROR_PIPE_NOT_CONNECTED:
    return X_EPIPE;
  case ERROR_WRITE_PROTECT:
    return X_EROFS;
  case ERROR_SEM_TIMEOUT:
    return X_ETIMEDOUT;
  case ERROR_NOT_SAME_DEVICE:
    return X_EXDEV;
  case ERROR_INVALID_FUNCTION:
    return X_EISDIR;
  case ERROR_META_EXPANSION_TOO_LONG:
    return X_E2BIG;
  case ERROR_BAD_EXE_FORMAT:
    return X_EFTYPE;
  default:
    return X_UNKNOWN;
  }
}
#else
static int errno_error(int e) {
  switch (e) {
  case ENOENT:
    return X_ENOENT;
  case EACCES:
    return X_EACCES;
  case EPERM:
    return X_EPERM;
  case EINVAL:
    return X_EINVAL;
  case E2BIG:
    return X_E2BIG;
  case ENOMEM:
    return X_ENOMEM;
  case ETIMEDOUT:
    return X_ETIMEDOUT;
  case ENOBUFS:
    return X_ENOBUFS;
  case EPIPE:
    return X_EPIPE;
  case ENOTSUP:
    return X_ENOTSUP;
  case ENOSYS:
    return X_ENOSYS;
  case ESRCH:
    return X_ESRCH;
  case EBADF:
    return X_EBADF;
  case EAGAIN:
    return X_EAGAIN;
  case EMFILE:
    return X_EMFILE;
  case ENAMETOOLONG:
    return X_ENAMETOOLONG;
  case ENOTDIR:
    return X_ENOTDIR;
  case EIO:
    return X_EIO;
  case EISDIR:
    return X_EISDIR;
  case ELOOP:
    return X_ELOOP;
  case ETXTBSY:
    return X_ETXTBSY;
  default:
    return X_UNKNOWN;
  }
}
#endif

//===----------------------------------------------------------------------===//
// Signals
//===----------------------------------------------------------------------===//

// os.constants.signals of Node on this system, in its order: the first name
// of a number is the one Node reports.
typedef struct {
  const char *name;
  int number;
} Signal;

static const Signal kSignals[] = {
#ifdef _WIN32
    {"SIGHUP", 1},   {"SIGINT", 2},    {"SIGQUIT", 3},   {"SIGILL", 4},
    {"SIGABRT", 22}, {"SIGFPE", 8},    {"SIGKILL", 9},   {"SIGSEGV", 11},
    {"SIGTERM", 15}, {"SIGBREAK", 21}, {"SIGWINCH", 28},
#else
    {"SIGHUP", SIGHUP},       {"SIGINT", SIGINT},   {"SIGQUIT", SIGQUIT},   {"SIGILL", SIGILL},
    {"SIGTRAP", SIGTRAP},     {"SIGABRT", SIGABRT}, {"SIGIOT", SIGIOT},     {"SIGBUS", SIGBUS},
    {"SIGFPE", SIGFPE},       {"SIGKILL", SIGKILL}, {"SIGUSR1", SIGUSR1},   {"SIGSEGV", SIGSEGV},
    {"SIGUSR2", SIGUSR2},     {"SIGPIPE", SIGPIPE}, {"SIGALRM", SIGALRM},   {"SIGTERM", SIGTERM},
    {"SIGCHLD", SIGCHLD},
#ifdef SIGSTKFLT
    {"SIGSTKFLT", SIGSTKFLT},
#endif
    {"SIGCONT", SIGCONT},     {"SIGSTOP", SIGSTOP}, {"SIGTSTP", SIGTSTP},   {"SIGTTIN", SIGTTIN},
    {"SIGTTOU", SIGTTOU},     {"SIGURG", SIGURG},   {"SIGXCPU", SIGXCPU},   {"SIGXFSZ", SIGXFSZ},
    {"SIGVTALRM", SIGVTALRM}, {"SIGPROF", SIGPROF}, {"SIGWINCH", SIGWINCH}, {"SIGIO", SIGIO},
#ifdef SIGPOLL
    {"SIGPOLL", SIGPOLL},
#endif
#ifdef SIGPWR
    {"SIGPWR", SIGPWR},
#endif
    {"SIGSYS", SIGSYS},
#endif
};

#define SIGNAL_TERM 15
#define SIGNAL_KILL 9

// The name of a signal, as Node's signo_string gives it; NULL for none.
static const char *signal_name(int number) {
  for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i)
    if (kSignals[i].number == number)
      return kSignals[i].name;
  return NULL;
}

//===----------------------------------------------------------------------===//
// Text
//===----------------------------------------------------------------------===//

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

static void buf_take_units(TzBuf *to, const TzBuf *from) {
  tz_buf_units(to, from->data, from->length);
}

static int has_zero(const TzBuf *b) {
  for (size_t i = 0; i < b->length; ++i)
    if (b->data[i] == 0)
      return 1;
  return 0;
}

// " Received 'a\x00b'": a string as Node's inspect shows it in the messages
// of its errors.
static void buf_received(TzBuf *m, const TzBuf *value) {
  int single = 0, dbl = 0;
  for (size_t i = 0; i < value->length; ++i) {
    single |= value->data[i] == '\'';
    dbl |= value->data[i] == '"';
  }
  uint16_t quote = single && !dbl ? '"' : '\'';
  tz_buf_ascii(m, ". Received ");
  tz_buf_push(m, quote);
  for (size_t i = 0; i < value->length; ++i) {
    uint16_t c = value->data[i];
    char hex[8];
    if (c == quote || c == '\\') {
      tz_buf_push(m, '\\');
      tz_buf_push(m, c);
    } else if (c == '\n') {
      tz_buf_ascii(m, "\\n");
    } else if (c == '\t') {
      tz_buf_ascii(m, "\\t");
    } else if (c == '\r') {
      tz_buf_ascii(m, "\\r");
    } else if (c < 0x20 || c == 0x7F) {
      static const char digits[] = "0123456789ABCDEF";
      hex[0] = '\\';
      hex[1] = 'x';
      hex[2] = digits[c >> 4];
      hex[3] = digits[c & 15];
      hex[4] = 0;
      tz_buf_ascii(m, hex);
    } else {
      tz_buf_push(m, c);
    }
  }
  tz_buf_push(m, quote);
}

// ERR_INVALID_ARG_VALUE of a string with a zero in it: "The argument
// 'args[0]' must be a string without null bytes. Received 'a\x00b'"; a name
// with a dot is a property.
static void throw_null_bytes(const char *name, const TzBuf *key, const char *reason,
                             const TzBuf *value) {
  TzBuf m;
  tz_buf_init(&m);
  tz_buf_ascii(&m, strchr(name, '.') ? "The property '" : "The argument '");
  tz_buf_ascii(&m, name);
  if (key) {
    tz_buf_ascii(&m, "['");
    buf_take_units(&m, key);
    tz_buf_ascii(&m, "']");
  }
  tz_buf_ascii(&m, "' ");
  tz_buf_ascii(&m, reason);
  buf_received(&m, value);
  TzStr message = tz_buf_take(&m);
  tz_throw_str(TZ_TYPE_ERROR, message.obj, message.meta);
  tz_str_release_value(message);
}

// A number as Node's messages show it.
static void throw_out_of_range(const char *name, const char *must, double value) {
  char number[32];
  number[tz_format_f64(value, number)] = 0;
  tz_throw(TZ_RANGE_ERROR, "The value of \"%s\" is out of range. It must be %s. Received %s", name,
           must, number);
}

//===----------------------------------------------------------------------===//
// Growing lists
//===----------------------------------------------------------------------===//

// The bytes a stream gave.
typedef struct {
  uint8_t *data;
  size_t length, capacity;
} Out;

static void out_append(Out *o, const uint8_t *data, size_t n) {
  if (o->length + n > o->capacity) {
    size_t capacity = o->capacity * 2 + 4096;
    if (capacity < o->length + n)
      capacity = o->length + n;
    uint8_t *grown = (uint8_t *)tz_alloc(capacity);
    if (o->length)
      memcpy(grown, o->data, o->length);
    tz_free(o->data);
    o->data = grown;
    o->capacity = capacity;
  }
  memcpy(o->data + o->length, data, n);
  o->length += n;
}

// Strings of UTF-16, for the arguments and the environment.
typedef struct {
  TzBuf *items;
  size_t count, capacity;
} List;

static TzBuf *list_add(List *l) {
  if (l->count == l->capacity) {
    size_t capacity = l->capacity * 2 + 8;
    TzBuf *grown = (TzBuf *)tz_alloc(capacity * sizeof(TzBuf));
    if (l->count)
      memcpy(grown, l->items, l->count * sizeof(TzBuf));
    tz_free(l->items);
    l->items = grown;
    l->capacity = capacity;
  }
  TzBuf *b = &l->items[l->count++];
  tz_buf_init(b);
  return b;
}

static void list_free(List *l) {
  for (size_t i = 0; i < l->count; ++i)
    tz_buf_free(&l->items[i]);
  tz_free(l->items);
  l->items = NULL;
  l->count = l->capacity = 0;
}

static int units_equal(const TzBuf *b, const char *text) {
  size_t n = strlen(text);
  if (b->length != n)
    return 0;
  for (size_t i = 0; i < n; ++i)
    if (b->data[i] != (unsigned char)text[i])
      return 0;
  return 1;
}

//===----------------------------------------------------------------------===//
// The call, prepared as Node prepares it (normalizeSpawnArguments)
//===----------------------------------------------------------------------===//

typedef struct {
  const TzSpawnRequest *req;
  TzBuf file;   // the program started: the shell, with one
  List args;    // its arguments, argv[0] first
  int verbatim; // windowsVerbatimArguments: as they are, unquoted
  TzBuf cwd;    // empty: this process's
  int has_cwd;
  List env; // "NAME=value"
  int kill_signal;
  int64_t timeout; // in milliseconds, 0 for none
  double max_buffer;
  TzBytes *input; // the bytes stdin gets, or NULL
} Call;

static void call_free(Call *c) {
  tz_buf_free(&c->file);
  list_free(&c->args);
  tz_buf_free(&c->cwd);
  list_free(&c->env);
  if (c->input)
    tz_instance_release((TzInstance *)c->input);
}

#ifdef _WIN32
// The value of a variable of this process, or 0 without it.
static int get_env(TzBuf *out, const wchar_t *name) {
  DWORD n = GetEnvironmentVariableW(name, NULL, 0);
  if (n == 0)
    return 0;
  wchar_t *value = (wchar_t *)tz_alloc(n * sizeof(wchar_t));
  DWORD got = GetEnvironmentVariableW(name, value, n);
  if (got < n)
    tz_buf_units(out, (const uint16_t *)value, got);
  tz_free(value);
  return got < n;
}

// Whether a file names cmd.exe: /^(?:.*\\)?cmd(?:\.exe)?$/i.
static int is_cmd(const TzBuf *file) {
  size_t n = file->length;
  const uint16_t *s = file->data;
  for (int exe = 1; exe >= 0; --exe) {
    const char *tail = exe ? "cmd.exe" : "cmd";
    size_t k = strlen(tail);
    if (n < k)
      continue;
    int same = 1;
    for (size_t i = 0; i < k && same; ++i) {
      uint16_t c = s[n - k + i];
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
      same = c == (unsigned char)tail[i];
    }
    if (same && (n == k || s[n - k - 1] == '\\'))
      return 1;
  }
  return 0;
}
#endif

// The environment the program gets: the option `env`, or a copy of this
// process's (without the hidden variables of drives, as process.env has
// them). On Windows, as Node does, sorted, keeping the first of names that
// differ only in case.
static int collect_env(Call *c) {
  const TzSpawnRequest *req = c->req;
  List pairs = {0};
  List names = {0};
  if (req->env) {
    const TzMapLayout *layout = req->env->layout;
    uint32_t at = 0;
    for (char *e; (e = (char *)tz_map_next(req->env, &at));) {
      TzBuf *name = list_add(&names);
      tz_buf_str(name, *(const TzStr *)(e + layout->key_offset));
      TzBuf value;
      tz_buf_init(&value);
      tz_buf_str(&value, *(const TzStr *)(e + layout->value_offset));
      if (has_zero(name) || has_zero(&value)) {
        throw_null_bytes("options.env", name, "must be a string without null bytes",
                         has_zero(name) ? name : &value);
        tz_buf_free(&value);
        list_free(&pairs);
        list_free(&names);
        return 0;
      }
      TzBuf *pair = list_add(&pairs);
      buf_take_units(pair, name);
      tz_buf_push(pair, '=');
      buf_take_units(pair, &value);
      tz_buf_free(&value);
    }
  } else {
#ifdef _WIN32
    wchar_t *block = GetEnvironmentStringsW();
    for (const wchar_t *p = block; p && *p; p += wcslen(p) + 1) {
      const wchar_t *equals = wcschr(p + 1, L'=');
      if (*p == L'=' || !equals)
        continue;
      tz_buf_units(list_add(&names), (const uint16_t *)p, (size_t)(equals - p));
      tz_buf_units(list_add(&pairs), (const uint16_t *)p, wcslen(p));
    }
    if (block)
      FreeEnvironmentStringsW(block);
#else
    extern char **environ;
    for (char **p = environ; p && *p; ++p) {
      const char *equals = strchr(*p, '=');
      if (!equals)
        continue;
      // Kept as bytes, one unit each: they go back to the system as they
      // came (see utf8_of).
      TzBuf *name = list_add(&names);
      for (const char *q = *p; q < equals; ++q)
        tz_buf_push(name, (uint8_t)*q);
      TzBuf *pair = list_add(&pairs);
      for (const char *q = *p; *q; ++q)
        tz_buf_push(pair, (uint8_t)*q);
    }
#endif
  }
#ifdef _WIN32
  // Sorted by name as JavaScript sorts strings, then the duplicates dropped.
  size_t n = pairs.count;
  size_t *order = (size_t *)tz_alloc((n + 1) * sizeof(size_t));
  for (size_t i = 0; i < n; ++i) {
    size_t j = i;
    while (j > 0) {
      const TzBuf *a = &names.items[order[j - 1]], *b = &names.items[i];
      size_t k = 0;
      while (k < a->length && k < b->length && a->data[k] == b->data[k])
        ++k;
      int greater = k < a->length && (k == b->length || a->data[k] > b->data[k]);
      if (!greater)
        break;
      order[j] = order[j - 1];
      --j;
    }
    order[j] = i;
  }
  for (size_t i = 0; i < n; ++i) {
    const TzBuf *name = &names.items[order[i]];
    int seen = 0;
    for (size_t k = 0; k < c->env.count && !seen; ++k) {
      const TzBuf *kept = &c->env.items[k];
      size_t eq = 0;
      while (eq < kept->length && kept->data[eq] != '=')
        ++eq;
      seen = CompareStringOrdinal((const wchar_t *)kept->data, (int)eq, (const wchar_t *)name->data,
                                  (int)name->length, TRUE) == CSTR_EQUAL;
    }
    if (!seen)
      buf_take_units(list_add(&c->env), &pairs.items[order[i]]);
  }
  tz_free(order);
#else
  for (size_t i = 0; i < pairs.count; ++i)
    buf_take_units(list_add(&c->env), &pairs.items[i]);
#endif
  list_free(&pairs);
  list_free(&names);
  return 1;
}

// The checks and the steps of normalizeSpawnArguments and spawnSync, in
// their order; false after throwing a TypeError or a RangeError.
static int prepare(Call *c, const TzSpawnRequest *req) {
  memset(c, 0, sizeof *c);
  c->req = req;
  tz_buf_init(&c->file);
  tz_buf_init(&c->cwd);
  tz_buf_str(&c->file, req->file);
  int exec = req->mode == TZ_CHILD_EXEC;
  if (has_zero(&c->file)) {
    throw_null_bytes(exec ? "command" : "file", NULL, "must be a string without null bytes",
                     &c->file);
    return 0;
  }
  TzBuf argv0;
  tz_buf_init(&argv0);
  if (req->has & HAS_ARGV0) {
    tz_buf_str(&argv0, req->argv0);
    if (has_zero(&argv0)) {
      throw_null_bytes("options.argv0", NULL, "must be a string without null bytes", &argv0);
      tz_buf_free(&argv0);
      return 0;
    }
  }
  if (c->file.length == 0) {
    TzBuf m;
    tz_buf_init(&m);
    tz_buf_ascii(&m, "The argument 'file' cannot be empty");
    buf_received(&m, &c->file);
    TzStr message = tz_buf_take(&m);
    tz_throw_str(TZ_TYPE_ERROR, message.obj, message.meta);
    tz_str_release_value(message);
    tz_buf_free(&argv0);
    return 0;
  }
  // The arguments: those of the call, after the program.
  List given = {0};
  uint32_t count = req->args ? req->args->length : 0;
  for (uint32_t i = 0; i < count; ++i) {
    TzBuf *arg = list_add(&given);
    tz_buf_str(arg, ((const TzStr *)req->args->data)[i]);
    if (has_zero(arg)) {
      char name[32];
      snprintf(name, sizeof name, "args[%u]", i);
      throw_null_bytes(name, NULL, "must be a string without null bytes", arg);
      list_free(&given);
      tz_buf_free(&argv0);
      return 0;
    }
  }
  if (req->has & HAS_CWD) {
    c->has_cwd = 1;
    tz_buf_str(&c->cwd, req->cwd);
    if (has_zero(&c->cwd)) {
      throw_null_bytes("options.cwd", NULL,
                       "must be a string, Uint8Array, or URL without null bytes", &c->cwd);
      list_free(&given);
      tz_buf_free(&argv0);
      return 0;
    }
  }
  c->verbatim = req->verbatim;
  int shell = exec ? (req->shell_kind == SHELL_PATH ? SHELL_PATH : SHELL_SYSTEM) : req->shell_kind;
  TzBuf path;
  tz_buf_init(&path);
  if (shell == SHELL_PATH) {
    tz_buf_str(&path, req->shell);
    // An empty one is none, as `if (options.shell)` reads it in Node.
    if (path.length == 0)
      shell = SHELL_NONE;
  }
  if (shell != SHELL_NONE) {
    if (has_zero(&path)) {
      throw_null_bytes("options.shell", NULL, "must be a string without null bytes", &path);
      tz_buf_free(&path);
      list_free(&given);
      tz_buf_free(&argv0);
      return 0;
    }
    // The command: the program and its arguments joined, as they are.
    TzBuf command;
    tz_buf_init(&command);
    buf_take_units(&command, &c->file);
    for (size_t i = 0; i < given.count; ++i) {
      tz_buf_push(&command, ' ');
      buf_take_units(&command, &given.items[i]);
    }
    list_free(&given);
    c->file.length = 0;
#ifdef _WIN32
    if (shell == SHELL_PATH)
      buf_take_units(&c->file, &path);
    else if (!get_env(&c->file, L"comspec"))
      tz_buf_ascii(&c->file, "cmd.exe");
    if (is_cmd(&c->file)) {
      // '/d /s /c' with the command in quotes, unquoted inside.
      tz_buf_ascii(list_add(&given), "/d");
      tz_buf_ascii(list_add(&given), "/s");
      tz_buf_ascii(list_add(&given), "/c");
      TzBuf *quoted = list_add(&given);
      tz_buf_push(quoted, '"');
      buf_take_units(quoted, &command);
      tz_buf_push(quoted, '"');
      c->verbatim = 1;
    } else {
      tz_buf_ascii(list_add(&given), "-c");
      buf_take_units(list_add(&given), &command);
    }
#else
    if (shell == SHELL_PATH)
      buf_take_units(&c->file, &path);
    else
      tz_buf_ascii(&c->file, "/bin/sh");
    tz_buf_ascii(list_add(&given), "-c");
    buf_take_units(list_add(&given), &command);
#endif
    tz_buf_free(&command);
  }
  tz_buf_free(&path);
  buf_take_units(list_add(&c->args), (req->has & HAS_ARGV0) ? &argv0 : &c->file);
  for (size_t i = 0; i < given.count; ++i)
    buf_take_units(list_add(&c->args), &given.items[i]);
  list_free(&given);
  tz_buf_free(&argv0);

  if (!collect_env(c))
    return 0;

  // spawnSync: the timeout, maxBuffer and the signal.
  if (req->has & HAS_TIMEOUT) {
    double t = req->timeout;
    if (!(t >= 0 && isfinite(t) && t == floor(t))) {
      throw_out_of_range("timeout", "an unsigned integer", t);
      return 0;
    }
    c->timeout = t > 1e15 ? (int64_t)1e15 : (int64_t)t;
  }
  c->max_buffer = 1024 * 1024;
  if (req->has & HAS_MAX_BUFFER) {
    if (!(req->max_buffer >= 0)) {
      throw_out_of_range("options.maxBuffer", "a positive number", req->max_buffer);
      return 0;
    }
    c->max_buffer = req->max_buffer;
  }
  c->kill_signal = SIGNAL_TERM;
  if (req->has & HAS_KILL_NAME) {
    TzBuf name;
    tz_buf_init(&name);
    tz_buf_str(&name, req->kill_name);
    int found = -1;
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0] && found < 0; ++i)
      if (units_equal(&name, kSignals[i].name))
        found = kSignals[i].number;
    if (found < 0) {
      TzBuf m;
      tz_buf_init(&m);
      tz_buf_ascii(&m, "Unknown signal: ");
      buf_take_units(&m, &name);
      TzStr message = tz_buf_take(&m);
      tz_throw_str(TZ_TYPE_ERROR, message.obj, message.meta);
      tz_str_release_value(message);
      tz_buf_free(&name);
      return 0;
    }
    tz_buf_free(&name);
    c->kill_signal = found;
  } else if (req->has & HAS_KILL_NUMBER) {
    if (!signal_name(req->kill_number)) {
      tz_throw(TZ_TYPE_ERROR, "Unknown signal: %d", req->kill_number);
      return 0;
    }
    c->kill_signal = req->kill_number;
  }
  // The input, as bytes: text in the encoding of the call (utf8 without
  // one), as Buffer.from(input, encoding) makes it.
  if (req->input == INPUT_TEXT && req->stdio[0] == STDIO_PIPE) {
    int32_t encoding = req->encoding > 0 ? req->encoding - 1 : 0;
    c->input = tz_buffer_from_string(req->input_text.obj, req->input_text.meta, encoding);
  } else if (req->input == INPUT_BYTES && req->stdio[0] == STDIO_PIPE && req->input_bytes) {
    tz_rc_inc(&req->input_bytes->rc);
    c->input = req->input_bytes;
  }
  return 1;
}

//===----------------------------------------------------------------------===//
// Running the program
//===----------------------------------------------------------------------===//

// What happened: the error (X_NONE for none), whether the program started,
// how it ended, and what it printed.
typedef struct {
  int error;      // the first error, as spawn_sync.cc keeps it
  int pipe_error; // an error of a pipe, reported only without the other
  int started;
  int32_t pid;
  int64_t status; // the exit code
  int signal;     // the signal that ended it, 0 for none
  Out out[3];
} Outcome;

static void set_error(Outcome *o, int error) {
  if (o->error == X_NONE)
    o->error = error;
}

static void set_pipe_error(Outcome *o, int error) {
  if (o->pipe_error == X_NONE)
    o->pipe_error = error;
}

#ifdef _WIN32

// Whether the program is a batch file, which Node refuses to start without
// a shell (CVE-2024-27980): its last extension, after trailing dots and
// spaces, is .bat or .cmd.
static int is_batch_file(const TzBuf *file) {
  size_t end = file->length;
  while (end > 0 && (file->data[end - 1] == '.' || file->data[end - 1] == ' ' ||
                     (file->data[end - 1] >= '\t' && file->data[end - 1] <= '\r')))
    --end;
  size_t start = end;
  while (start > 0) {
    uint16_t c = file->data[start - 1];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      break;
    --start;
  }
  if (start == end || start == 0 || file->data[start - 1] != '.' || end - start != 3)
    return 0;
  char ext[4];
  for (int i = 0; i < 3; ++i) {
    uint16_t c = file->data[start + (size_t)i];
    ext[i] = (char)(c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c);
  }
  ext[3] = 0;
  return strcmp(ext, "bat") == 0 || strcmp(ext, "cmd") == 0;
}

// libuv's search_join_test: dir and name joined (with the cwd for a
// relative dir) and the extension added; the result if that is a file,
// else NULL. To free with tz_free.
static wchar_t *search_path_join_test(const wchar_t *dir, size_t dir_len, const wchar_t *name,
                                      size_t name_len, const wchar_t *ext, size_t ext_len,
                                      const wchar_t *cwd, size_t cwd_len) {
  if (dir_len > 2 && ((dir[0] == L'\\' || dir[0] == L'/') && (dir[1] == L'\\' || dir[1] == L'/'))) {
    cwd_len = 0; // a UNC path
  } else if (dir_len >= 1 && (dir[0] == L'/' || dir[0] == L'\\')) {
    cwd_len = 2; // a full path without a drive: the drive of the cwd
  } else if (dir_len >= 2 && dir[1] == L':' &&
             (dir_len < 3 || (dir[2] != L'/' && dir[2] != L'\\'))) {
    // A relative path with a drive (D:../file): the cwd, if on that drive.
    if (cwd_len < 2 || _wcsnicmp(cwd, dir, 2) != 0) {
      cwd_len = 0;
    } else {
      dir += 2;
      dir_len -= 2;
    }
  } else if (dir_len > 2 && dir[1] == L':') {
    cwd_len = 0; // absolute, with a drive
  }
  wchar_t *result = (wchar_t *)tz_alloc(sizeof(wchar_t) *
                                        (cwd_len + 1 + dir_len + 1 + name_len + 1 + ext_len + 1));
  wchar_t *pos = result;
  wcsncpy(pos, cwd, cwd_len);
  pos += cwd_len;
  if (cwd_len && wcsrchr(L"\\/:", pos[-1]) == NULL)
    *pos++ = L'\\';
  wcsncpy(pos, dir, dir_len);
  pos += dir_len;
  if (dir_len && wcsrchr(L"\\/:", pos[-1]) == NULL)
    *pos++ = L'\\';
  wcsncpy(pos, name, name_len);
  pos += name_len;
  if (ext_len) {
    if (name_len && pos[-1] != L'.')
      *pos++ = L'.';
    wcsncpy(pos, ext, ext_len);
    pos += ext_len;
  }
  *pos = 0;
  DWORD attrs = GetFileAttributesW(result);
  if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
    return result;
  tz_free(result);
  return NULL;
}

// libuv's path_search_walk_ext: the name as it is if it has an extension,
// then with .com, then with .exe.
static wchar_t *path_search_walk_ext(const wchar_t *dir, size_t dir_len, const wchar_t *name,
                                     size_t name_len, const wchar_t *cwd, size_t cwd_len,
                                     int name_has_ext) {
  wchar_t *result;
  if (name_has_ext &&
      (result = search_path_join_test(dir, dir_len, name, name_len, L"", 0, cwd, cwd_len)))
    return result;
  if ((result = search_path_join_test(dir, dir_len, name, name_len, L"com", 3, cwd, cwd_len)))
    return result;
  return search_path_join_test(dir, dir_len, name, name_len, L"exe", 3, cwd, cwd_len);
}

// libuv's search_path: the program as CreateProcess gets it, found as
// cmd.exe finds it, but trying only .com and .exe, not PATHEXT. A name
// without a directory is looked for in the cwd (of the child), then in each
// directory of PATH, which may be quoted. NULL if there is none.
static wchar_t *search_path(const wchar_t *file, const wchar_t *cwd, const wchar_t *path) {
  size_t file_len = wcslen(file), cwd_len = wcslen(cwd);
  if (file_len == 0 || (file_len == 1 && file[0] == L'.'))
    return NULL;
  const wchar_t *name = file + file_len;
  while (name > file && name[-1] != L'\\' && name[-1] != L'/' && name[-1] != L':')
    --name;
  int file_has_dir = name != file;
  const wchar_t *dot = wcschr(name, L'.');
  int name_has_ext = dot != NULL && dot[1] != 0;
  if (file_has_dir)
    return path_search_walk_ext(file, (size_t)(name - file), name, file_len - (size_t)(name - file),
                                cwd, cwd_len, name_has_ext);
  wchar_t *result = NULL;
  if (NeedCurrentDirectoryForExePathW(L""))
    result = path_search_walk_ext(L"", 0, file, file_len, cwd, cwd_len, name_has_ext);
  const wchar_t *dir_end = path;
  while (result == NULL) {
    if (dir_end == NULL || *dir_end == 0)
      break;
    if (dir_end != path || *path == L';')
      ++dir_end;
    const wchar_t *dir_start = dir_end;
    if (*dir_start == L'"' || *dir_start == L'\'') {
      dir_end = wcschr(dir_start + 1, *dir_start);
      if (dir_end == NULL)
        dir_end = wcschr(dir_start, 0);
    }
    dir_end = wcschr(dir_end, L';');
    if (dir_end == NULL)
      dir_end = wcschr(dir_start, 0);
    if (dir_end == dir_start)
      continue;
    const wchar_t *dir = dir_start;
    size_t dir_len = (size_t)(dir_end - dir_start);
    if (dir[0] == L'"' || dir[0] == L'\'') {
      ++dir;
      --dir_len;
    }
    if (dir_len > 0 && (dir[dir_len - 1] == L'"' || dir[dir_len - 1] == L'\''))
      --dir_len;
    result = path_search_walk_ext(dir, dir_len, file, file_len, cwd, cwd_len, name_has_ext);
  }
  return result;
}

// libuv's quote_cmd_arg: an argument as the C library of the child splits
// the command line back into it (CommandLineToArgvW's rules).
static void quote_cmd_arg(TzBuf *out, const TzBuf *arg) {
  size_t len = arg->length;
  const uint16_t *s = arg->data;
  if (len == 0) {
    tz_buf_ascii(out, "\"\"");
    return;
  }
  int needs = 0, special = 0;
  for (size_t i = 0; i < len; ++i) {
    needs |= s[i] == ' ' || s[i] == '\t' || s[i] == '"';
    special |= s[i] == '"' || s[i] == '\\';
  }
  if (!needs) {
    tz_buf_units(out, s, len);
    return;
  }
  if (!special) {
    tz_buf_push(out, '"');
    tz_buf_units(out, s, len);
    tz_buf_push(out, '"');
    return;
  }
  // Backward: the backslashes before a quote (or the closing quote) are
  // doubled, and each quote escaped.
  TzBuf rev;
  tz_buf_init(&rev);
  int quote_hit = 1;
  for (size_t i = len; i > 0; --i) {
    tz_buf_push(&rev, s[i - 1]);
    if (quote_hit && s[i - 1] == '\\') {
      tz_buf_push(&rev, '\\');
    } else if (s[i - 1] == '"') {
      quote_hit = 1;
      tz_buf_push(&rev, '\\');
    } else {
      quote_hit = 0;
    }
  }
  tz_buf_push(out, '"');
  for (size_t i = rev.length; i > 0; --i)
    tz_buf_push(out, rev.data[i - 1]);
  tz_buf_push(out, '"');
  tz_buf_free(&rev);
}

// The environment block of CreateProcessW, as libuv's make_program_env makes
// it: sorted by name without case, with the variables Windows programs need
// (SYSTEMROOT...) added from this process when the environment lacks them.
static wchar_t *make_env_block(Call *c) {
  static const wchar_t *const required[] = {
      L"HOMEDRIVE", L"HOMEPATH",   L"LOGONSERVER", L"PATH",        L"SYSTEMDRIVE", L"SYSTEMROOT",
      L"TEMP",      L"USERDOMAIN", L"USERNAME",    L"USERPROFILE", L"WINDIR"};
  for (size_t r = 0; r < sizeof required / sizeof required[0]; ++r) {
    int present = 0;
    int len = (int)wcslen(required[r]);
    for (size_t i = 0; i < c->env.count && !present; ++i) {
      const TzBuf *pair = &c->env.items[i];
      size_t eq = 0;
      while (eq < pair->length && pair->data[eq] != '=')
        ++eq;
      present = CompareStringOrdinal((const wchar_t *)pair->data, (int)eq, required[r], len,
                                     TRUE) == CSTR_EQUAL;
    }
    TzBuf value;
    tz_buf_init(&value);
    if (!present && get_env(&value, required[r])) {
      TzBuf *pair = list_add(&c->env);
      tz_buf_units(pair, (const uint16_t *)required[r], (size_t)len);
      tz_buf_push(pair, '=');
      buf_take_units(pair, &value);
    }
    tz_buf_free(&value);
  }
  // Sorted by name, case ignored (insertion: the lists are short).
  size_t n = c->env.count;
  for (size_t i = 1; i < n; ++i) {
    TzBuf moving = c->env.items[i];
    size_t moving_eq = 0;
    while (moving_eq < moving.length && moving.data[moving_eq] != '=')
      ++moving_eq;
    size_t j = i;
    while (j > 0) {
      const TzBuf *before = &c->env.items[j - 1];
      size_t eq = 0;
      while (eq < before->length && before->data[eq] != '=')
        ++eq;
      if (CompareStringOrdinal((const wchar_t *)before->data, (int)eq, (const wchar_t *)moving.data,
                               (int)moving_eq, TRUE) != CSTR_GREATER_THAN)
        break;
      c->env.items[j] = c->env.items[j - 1];
      --j;
    }
    c->env.items[j] = moving;
  }
  size_t total = 2;
  for (size_t i = 0; i < n; ++i)
    total += c->env.items[i].length + 1;
  wchar_t *block = (wchar_t *)tz_alloc(total * sizeof(wchar_t));
  wchar_t *p = block;
  for (size_t i = 0; i < n; ++i) {
    memcpy(p, c->env.items[i].data, c->env.items[i].length * sizeof(wchar_t));
    p += c->env.items[i].length;
    *p++ = 0;
  }
  *p++ = 0;
  if (n == 0)
    *p = 0;
  return block;
}

// PATH of the block (find_path), NULL without one.
static const wchar_t *find_path(const wchar_t *env) {
  for (; env && *env; env += wcslen(env) + 1)
    if ((env[0] == L'P' || env[0] == L'p') && (env[1] == L'A' || env[1] == L'a') &&
        (env[2] == L'T' || env[2] == L't') && (env[3] == L'H' || env[3] == L'h') && env[4] == L'=')
      return env + 5;
  return NULL;
}

// The job that the programs started are in, as libuv's: they end when this
// process does.
HANDLE tz_child_job; // not static: see the rules in tzrt.h
INIT_ONCE tz_child_job_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_job(PINIT_ONCE once, PVOID param, PVOID *context) {
  (void)once;
  (void)param;
  (void)context;
  SECURITY_ATTRIBUTES attr;
  memset(&attr, 0, sizeof attr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
  memset(&info, 0, sizeof info);
  info.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK |
      JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  HANDLE job = CreateJobObjectW(&attr, NULL);
  if (job && !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info)) {
    CloseHandle(job);
    job = NULL;
  }
  // As libuv does: this process too, which ties the job to its session.
  if (job)
    AssignProcessToJobObject(job, GetCurrentProcess());
  tz_child_job = job;
  return TRUE;
}

// The parent's end of a pipe and what is in flight on it.
typedef struct {
  HANDLE handle; // NULL when closed
  OVERLAPPED ov;
  int pending;
  uint8_t *chunk; // the read in flight
} Pipe;

volatile LONG tz_child_pipes; // a number for the names of pipes

// A connected pair of pipe ends, as libuv's uv__create_stdio_pipe_pair
// makes them: the parent's, a named pipe for overlapped I/O; the child's,
// inheritable and synchronous. `readable` is the child's stdin.
static DWORD make_pipe(HANDLE *parent, HANDLE *child, int readable) {
  DWORD server = FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE | WRITE_DAC;
  DWORD client = 0;
  if (readable) {
    server |= PIPE_ACCESS_OUTBOUND | PIPE_ACCESS_INBOUND;
    client |= GENERIC_READ | FILE_WRITE_ATTRIBUTES;
  } else {
    server |= PIPE_ACCESS_INBOUND;
    client |= GENERIC_WRITE | FILE_READ_ATTRIBUTES;
  }
  wchar_t name[64];
  HANDLE h = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 32 && h == INVALID_HANDLE_VALUE; ++attempt) {
    _snwprintf(name, 64, L"\\\\.\\pipe\\tz-%lu-%ld", GetCurrentProcessId(),
               InterlockedIncrement(&tz_child_pipes));
    name[63] = 0;
    h = CreateNamedPipeW(name, server, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536,
                         65536, 0, NULL);
    if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_PIPE_BUSY &&
        GetLastError() != ERROR_ACCESS_DENIED)
      return GetLastError();
  }
  if (h == INVALID_HANDLE_VALUE)
    return GetLastError();
  SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
  HANDLE c = CreateFileW(name, client, 0, &sa, OPEN_EXISTING, 0, NULL);
  if (c == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError();
    CloseHandle(h);
    return error;
  }
  // The child connected when it opened its end.
  OVERLAPPED ov;
  memset(&ov, 0, sizeof ov);
  ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!ConnectNamedPipe(h, &ov) && GetLastError() == ERROR_IO_PENDING) {
    DWORD n;
    GetOverlappedResult(h, &ov, &n, TRUE);
  }
  CloseHandle(ov.hEvent);
  *parent = h;
  *child = c;
  return 0;
}

static void pipe_close(Pipe *p) {
  if (!p->handle)
    return;
  if (p->pending) {
    DWORD n;
    CancelIoEx(p->handle, &p->ov);
    GetOverlappedResult(p->handle, &p->ov, &n, TRUE);
    p->pending = 0;
  }
  CloseHandle(p->handle);
  p->handle = NULL;
}

// Starts the next read of an output pipe; false at its end.
static int pipe_read(Pipe *p, Outcome *o) {
  if (!p->chunk)
    p->chunk = (uint8_t *)tz_alloc(65536);
  ResetEvent(p->ov.hEvent);
  if (ReadFile(p->handle, p->chunk, 65536, NULL, &p->ov) || GetLastError() == ERROR_IO_PENDING) {
    p->pending = 1;
    return 1;
  }
  DWORD error = GetLastError();
  if (error != ERROR_BROKEN_PIPE)
    set_pipe_error(o, win32_error(error));
  pipe_close(p);
  return 0;
}

static void run(Call *c, Outcome *o) {
  const TzSpawnRequest *req = c->req;
  if (is_batch_file(&c->file)) {
    o->error = X_EINVAL;
    return;
  }
  if (req->has & (HAS_UID | HAS_GID)) {
    o->error = X_ENOTSUP;
    return;
  }
  // The command line, the environment, the cwd and PATH.
  TzBuf line;
  tz_buf_init(&line);
  for (size_t i = 0; i < c->args.count; ++i) {
    if (i > 0)
      tz_buf_push(&line, ' ');
    if (c->verbatim)
      buf_take_units(&line, &c->args.items[i]);
    else
      quote_cmd_arg(&line, &c->args.items[i]);
  }
  tz_buf_push(&line, 0);
  wchar_t *env = make_env_block(c);
  TzBuf cwd;
  tz_buf_init(&cwd);
  if (c->has_cwd) {
    buf_take_units(&cwd, &c->cwd);
  } else {
    DWORD n = GetCurrentDirectoryW(0, NULL);
    wchar_t *here = (wchar_t *)tz_alloc((n + 1) * sizeof(wchar_t));
    DWORD got = GetCurrentDirectoryW(n + 1, here);
    tz_buf_units(&cwd, (const uint16_t *)here, got <= n ? got : 0);
    tz_free(here);
  }
  tz_buf_push(&cwd, 0);
  if (cwd.length - 1 >= MAX_PATH) {
    DWORD n = GetShortPathNameW((wchar_t *)cwd.data, (wchar_t *)cwd.data, (DWORD)cwd.length);
    if (n > 0 && n < cwd.length)
      cwd.data[n] = 0;
  }
  TzBuf parent_path;
  tz_buf_init(&parent_path);
  const wchar_t *path = find_path(env);
  if (!path && get_env(&parent_path, L"PATH")) {
    tz_buf_push(&parent_path, 0);
    path = (const wchar_t *)parent_path.data;
  }
  TzBuf file;
  tz_buf_init(&file);
  buf_take_units(&file, &c->file);
  tz_buf_push(&file, 0);

  // The handles of the child, as libuv's uv__stdio_create makes them.
  HANDLE child[3] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
  unsigned char crt_flags[3] = {0, 0, 0};
  Pipe pipes[3];
  memset(pipes, 0, sizeof pipes);
  DWORD failure = 0;
  int inherits = 0;
  for (int i = 0; i < 3 && !failure; ++i) {
    int32_t kind = req->stdio[i];
    if (kind == STDIO_IGNORE) {
      SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
      DWORD access = i == 0 ? FILE_GENERIC_READ : FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES;
      child[i] = CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                             0, NULL);
      if (child[i] == INVALID_HANDLE_VALUE)
        failure = GetLastError();
      crt_flags[i] = 0x01 | 0x40; // FOPEN | FDEV
    } else if (kind == STDIO_PIPE) {
      failure = make_pipe(&pipes[i].handle, &child[i], i == 0);
      if (!failure)
        pipes[i].ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
      crt_flags[i] = 0x01 | 0x08; // FOPEN | FPIPE
    } else {
      inherits = 1;
      int fd = kind - STDIO_INHERIT;
      HANDLE h = (HANDLE)_get_osfhandle(fd);
      if (h == INVALID_HANDLE_VALUE || h == NULL || h == (HANDLE)-2 ||
          !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &child[i], 0, TRUE,
                           DUPLICATE_SAME_ACCESS)) {
        child[i] = INVALID_HANDLE_VALUE;
        // A missing stdin, stdout or stderr is no error: the child goes
        // without it.
        if (fd > 2)
          failure = ERROR_INVALID_HANDLE;
        continue;
      }
      switch (GetFileType(child[i])) {
      case FILE_TYPE_DISK:
        crt_flags[i] = 0x01;
        break;
      case FILE_TYPE_PIPE:
        crt_flags[i] = 0x01 | 0x08;
        break;
      default:
        crt_flags[i] = 0x01 | 0x40;
        break;
      }
    }
  }
  // The C library of the child reads its fds from lpReserved2: a count, the
  // flags of each, then the handles, unaligned.
  unsigned char reserved[sizeof(int) + 3 + 3 * sizeof(HANDLE)];
  int three = 3;
  memcpy(reserved, &three, sizeof three);
  memcpy(reserved + sizeof(int), crt_flags, 3);
  memcpy(reserved + sizeof(int) + 3, child, sizeof child);

  wchar_t *application = NULL;
  if (!failure) {
    application = search_path((const wchar_t *)file.data, (const wchar_t *)cwd.data, path);
    if (!application)
      failure = ERROR_FILE_NOT_FOUND;
  }

  // Only these handles go to the child.
  HANDLE inherited[3];
  DWORD inherited_count = 0;
  for (int i = 0; i < 3; ++i) {
    if (child[i] == INVALID_HANDLE_VALUE)
      continue;
    int dup = 0;
    for (DWORD k = 0; k < inherited_count; ++k)
      dup |= inherited[k] == child[i];
    if (!dup)
      inherited[inherited_count++] = child[i];
  }
  LPPROC_THREAD_ATTRIBUTE_LIST attributes = NULL;
  if (!failure && inherited_count > 0) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)tz_alloc(size);
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   inherited_count * sizeof(HANDLE), NULL, NULL)) {
      failure = GetLastError();
    }
  }

  PROCESS_INFORMATION info;
  memset(&info, 0, sizeof info);
  if (!failure) {
    STARTUPINFOEXW startup;
    memset(&startup, 0, sizeof startup);
    startup.StartupInfo.cb = attributes ? sizeof startup : sizeof startup.StartupInfo;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.cbReserved2 = (WORD)sizeof reserved;
    startup.StartupInfo.lpReserved2 = reserved;
    startup.StartupInfo.hStdInput = child[0];
    startup.StartupInfo.hStdOutput = child[1];
    startup.StartupInfo.hStdError = child[2];
    startup.lpAttributeList = attributes;
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (attributes)
      flags |= EXTENDED_STARTUPINFO_PRESENT;
    // windowsHide: no console window unless a stream is inherited.
    if (req->windows_hide && !inherits)
      flags |= CREATE_NO_WINDOW;
    startup.StartupInfo.wShowWindow = req->windows_hide ? SW_HIDE : SW_SHOWDEFAULT;
    if (req->detached)
      flags |= DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED;
    // The output of this program first, so that it comes before the
    // child's where they share a stream.
    if (inherits)
      tz_out_flush(1);
    if (!CreateProcessW(application, (wchar_t *)line.data, NULL, NULL, inherited_count > 0, flags,
                        env, (const wchar_t *)cwd.data, &startup.StartupInfo, &info)) {
      failure = GetLastError();
    } else {
      if (!req->detached) {
        InitOnceExecuteOnce(&tz_child_job_once, init_job, NULL, NULL);
        if (tz_child_job)
          AssignProcessToJobObject(tz_child_job, info.hProcess);
      } else if (ResumeThread(info.hThread) == (DWORD)-1) {
        failure = GetLastError();
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
      }
    }
  }
  if (attributes) {
    DeleteProcThreadAttributeList(attributes);
    tz_free(attributes);
  }
  tz_free(application);
  tz_free(env);
  tz_buf_free(&line);
  tz_buf_free(&cwd);
  tz_buf_free(&parent_path);
  tz_buf_free(&file);
  for (int i = 0; i < 3; ++i)
    if (child[i] != INVALID_HANDLE_VALUE)
      CloseHandle(child[i]);
  if (failure) {
    o->error = win32_error(failure);
    for (int i = 0; i < 3; ++i) {
      pipe_close(&pipes[i]);
      if (pipes[i].ov.hEvent)
        CloseHandle(pipes[i].ov.hEvent);
    }
    return;
  }
  CloseHandle(info.hThread);
  o->started = 1;
  o->pid = (int32_t)info.dwProcessId;

  // The loop of spawn_sync.cc: until the program ended and its outputs
  // closed, or the time ran out.
  ULONGLONG deadline = c->timeout > 0 ? GetTickCount64() + (ULONGLONG)c->timeout : 0;
  int exited = 0, killed = 0;
  double buffered = 0;
  for (int i = 1; i < 3; ++i)
    if (pipes[i].handle)
      pipe_read(&pipes[i], o);
  if (pipes[0].handle) {
    if (c->input && c->input->length > 0) {
      ResetEvent(pipes[0].ov.hEvent);
      DWORD length = (DWORD)tz_bytes_byte_length(c->input);
      if (WriteFile(pipes[0].handle, c->input->data, length, NULL, &pipes[0].ov) ||
          GetLastError() == ERROR_IO_PENDING) {
        pipes[0].pending = 1;
      } else {
        DWORD error = GetLastError();
        set_pipe_error(
            o, error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA ? X_EPIPE : win32_error(error));
        pipe_close(&pipes[0]);
      }
    } else {
      pipe_close(&pipes[0]); // the end of the input
    }
  }
  // Kills the program (SyncProcessRunner::Kill) and closes the pipes.
  int kill_signal = c->kill_signal;
  for (;;) {
    HANDLE waits[4];
    int which[4];
    DWORD n = 0;
    if (!exited) {
      waits[n] = info.hProcess;
      which[n++] = -1;
    }
    for (int i = 0; i < 3; ++i)
      if (pipes[i].handle && pipes[i].pending) {
        waits[n] = pipes[i].ov.hEvent;
        which[n++] = i;
      }
    if (n == 0)
      break;
    DWORD wait = INFINITE;
    if (deadline) {
      ULONGLONG now = GetTickCount64();
      wait = now >= deadline ? 0 : (DWORD)(deadline - now);
    }
    DWORD r = WaitForMultipleObjects(n, waits, FALSE, wait);
    int stop = 0;
    if (r == WAIT_TIMEOUT) {
      set_error(o, X_ETIMEDOUT);
      deadline = 0;
      stop = 1;
    } else if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) {
      int i = which[r - WAIT_OBJECT_0];
      if (i < 0) {
        exited = 1;
        continue;
      }
      Pipe *p = &pipes[i];
      DWORD got = 0;
      BOOL ok = GetOverlappedResult(p->handle, &p->ov, &got, FALSE);
      DWORD error = ok ? 0 : GetLastError();
      p->pending = 0;
      if (i == 0) {
        // The input is written: its end tells the child so.
        if (!ok)
          set_pipe_error(o, error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA
                                ? X_EPIPE
                                : win32_error(error));
        pipe_close(p);
        continue;
      }
      if (!ok) {
        if (error != ERROR_BROKEN_PIPE)
          set_pipe_error(o, win32_error(error));
        pipe_close(p);
        continue;
      }
      out_append(&o->out[i], p->chunk, got);
      buffered += got;
      if (c->max_buffer > 0 && buffered > c->max_buffer) {
        set_error(o, X_ENOBUFS);
        stop = 1;
      } else {
        pipe_read(p, o);
      }
    } else {
      set_error(o, win32_error(GetLastError()));
      stop = 1;
    }
    if (stop && !killed) {
      killed = 1;
      if (!exited) {
        // Windows has no signals: TERM, KILL, INT and QUIT end the program
        // with 1, as libuv does; others are ENOSYS, and then SIGKILL.
        int supported = kill_signal == SIGNAL_TERM || kill_signal == SIGNAL_KILL ||
                        kill_signal == 2 || kill_signal == 3;
        if (!supported) {
          set_error(o, X_ENOSYS);
          kill_signal = SIGNAL_KILL;
        }
        if (TerminateProcess(info.hProcess, 1)) {
          o->signal = kill_signal;
        } else {
          DWORD status;
          DWORD error = GetLastError();
          if (!(error == ERROR_ACCESS_DENIED && GetExitCodeProcess(info.hProcess, &status) &&
                status != STILL_ACTIVE))
            set_error(o, win32_error(error));
        }
      }
      for (int i = 0; i < 3; ++i)
        pipe_close(&pipes[i]);
    }
  }
  DWORD status = 0;
  GetExitCodeProcess(info.hProcess, &status);
  o->status = status;
  CloseHandle(info.hProcess);
  for (int i = 0; i < 3; ++i) {
    pipe_close(&pipes[i]);
    if (pipes[i].ov.hEvent)
      CloseHandle(pipes[i].ov.hEvent);
    tz_free(pipes[i].chunk);
  }
}

#else // POSIX

// The string as UTF-8 with a terminating zero, to free with tz_free. Units
// above 0xFF that are not pairs become U+FFFD; a string from the
// environment, kept as bytes (see collect_env), goes back as it came.
static char *utf8_of(const TzBuf *b) {
  char *out = (char *)tz_alloc(b->length * 3 + 1);
  size_t n = 0;
  for (size_t i = 0; i < b->length; ++i) {
    uint32_t c = b->data[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < b->length && b->data[i + 1] >= 0xDC00 &&
        b->data[i + 1] <= 0xDFFF)
      c = 0x10000 + ((c - 0xD800) << 10) + (b->data[++i] - 0xDC00);
    else if (c >= 0xD800 && c <= 0xDFFF)
      c = 0xFFFD;
    if (c < 0x80) {
      out[n++] = (char)c;
    } else if (c < 0x800) {
      out[n++] = (char)(0xC0 | (c >> 6));
      out[n++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out[n++] = (char)(0xE0 | (c >> 12));
      out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[n++] = (char)(0x80 | (c & 0x3F));
    } else {
      out[n++] = (char)(0xF0 | (c >> 18));
      out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
      out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[n++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[n] = 0;
  return out;
}

static int64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// The program found as execvp finds it, in the PATH of the child's
// environment (libuv's uv__spawn_find_path_in_env); NULL for a path, which
// is used as it is, or a name that is in none of its directories.
static char *find_program(const char *file, char **envp) {
  if (strchr(file, '/'))
    return NULL;
  const char *path = NULL;
  for (char **e = envp; *e && !path; ++e)
    if (strncmp(*e, "PATH=", 5) == 0)
      path = *e + 5;
  if (!path)
    path = "/usr/local/bin:/bin:/usr/bin";
  size_t file_len = strlen(file);
  for (const char *p = path;;) {
    const char *end = strchr(p, ':');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char *candidate = (char *)tz_alloc(len + file_len + 3);
    if (len == 0) {
      strcpy(candidate, file);
    } else {
      memcpy(candidate, p, len);
      candidate[len] = '/';
      memcpy(candidate + len + 1, file, file_len + 1);
    }
    if (access(candidate, X_OK) == 0)
      return candidate;
    tz_free(candidate);
    if (!end)
      break;
    p = end + 1;
  }
  return NULL;
}

static void run(Call *c, Outcome *o) {
  const TzSpawnRequest *req = c->req;
  char **argv = (char **)tz_alloc((c->args.count + 1) * sizeof(char *));
  for (size_t i = 0; i < c->args.count; ++i)
    argv[i] = utf8_of(&c->args.items[i]);
  argv[c->args.count] = NULL;
  char **envp = (char **)tz_alloc((c->env.count + 1) * sizeof(char *));
  for (size_t i = 0; i < c->env.count; ++i)
    envp[i] = utf8_of(&c->env.items[i]);
  envp[c->env.count] = NULL;
  char *file = utf8_of(&c->file);
  char *cwd = c->has_cwd ? utf8_of(&c->cwd) : NULL;
  char *program = find_program(file, envp);
  int failure = 0, inherits = 0;
  // A name not found in PATH is not looked for elsewhere, as execvp does.
  if (!program && !strchr(file, '/'))
    failure = ENOENT;

  int parent[3] = {-1, -1, -1}, child[3] = {-1, -1, -1};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  for (int i = 0; i < 3 && !failure; ++i) {
    int32_t kind = req->stdio[i];
    if (kind == STDIO_PIPE) {
      int fds[2];
      if (pipe2(fds, O_CLOEXEC) != 0) {
        failure = errno;
        break;
      }
      parent[i] = i == 0 ? fds[1] : fds[0];
      child[i] = i == 0 ? fds[0] : fds[1];
      posix_spawn_file_actions_adddup2(&actions, child[i], i);
    } else if (kind == STDIO_IGNORE) {
      posix_spawn_file_actions_addopen(&actions, i, "/dev/null", i == 0 ? O_RDONLY : O_RDWR, 0);
    } else {
      inherits = 1;
      int fd = kind - STDIO_INHERIT;
      if (fd != i)
        posix_spawn_file_actions_adddup2(&actions, fd, i);
    }
  }
  if (cwd && !failure)
    failure = posix_spawn_file_actions_addchdir_np(&actions, cwd);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
  sigset_t none, all;
  sigemptyset(&none);
  sigfillset(&all);
  posix_spawnattr_setsigmask(&attr, &none);
  posix_spawnattr_setsigdefault(&attr, &all);
  if (req->detached)
    flags |= POSIX_SPAWN_SETSID;
  posix_spawnattr_setflags(&attr, flags);
  if (req->has & (HAS_UID | HAS_GID))
    failure = ENOTSUP; // TODO(linux): setuid and setgid in the child, as libuv does
  pid_t pid = 0;
  if (!failure) {
    if (inherits)
      tz_out_flush(1);
    failure = posix_spawn(&pid, program ? program : file, &actions, &attr, argv, envp);
  }
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  for (int i = 0; i < 3; ++i)
    if (child[i] >= 0)
      close(child[i]);
  for (size_t i = 0; i < c->args.count; ++i)
    tz_free(argv[i]);
  tz_free(argv);
  for (size_t i = 0; i < c->env.count; ++i)
    tz_free(envp[i]);
  tz_free(envp);
  tz_free(file);
  tz_free(cwd);
  tz_free(program);
  if (failure) {
    o->error = errno_error(failure);
    for (int i = 0; i < 3; ++i)
      if (parent[i] >= 0)
        close(parent[i]);
    return;
  }
  o->started = 1;
  o->pid = (int32_t)pid;
  for (int i = 0; i < 3; ++i)
    if (parent[i] >= 0)
      fcntl(parent[i], F_SETFL, fcntl(parent[i], F_GETFL) | O_NONBLOCK);
  size_t written = 0;
  size_t input_length = c->input ? (size_t)tz_bytes_byte_length(c->input) : 0;
  if (parent[0] >= 0 && input_length == 0) {
    close(parent[0]);
    parent[0] = -1;
  }
  // A child that stops reading its input must not end this process, as
  // Node, which ignores SIGPIPE, is not ended.
  signal(SIGPIPE, SIG_IGN);
  int64_t deadline = c->timeout > 0 ? now_ms() + c->timeout : 0;
  int exited = 0, killed = 0, wstatus = 0;
  double buffered = 0;
  uint8_t *chunk = (uint8_t *)tz_alloc(65536);
  for (;;) {
    struct pollfd fds[3];
    int which[3];
    nfds_t n = 0;
    for (int i = 0; i < 3; ++i)
      if (parent[i] >= 0) {
        fds[n].fd = parent[i];
        fds[n].events = i == 0 ? POLLOUT : POLLIN;
        fds[n].revents = 0;
        which[n++] = i;
      }
    if (n == 0 && exited)
      break;
    int wait = -1;
    if (deadline) {
      int64_t left = deadline - now_ms();
      wait = left > 0 ? (int)left : 0;
    }
    // Without pipes, the wait for the end is a poll of nothing in steps.
    if (n == 0 && (wait < 0 || wait > 10))
      wait = 10;
    int stop = 0;
    int r = poll(fds, n, wait);
    if (!exited) {
      pid_t done = waitpid(pid, &wstatus, WNOHANG);
      if (done == pid)
        exited = 1;
    }
    if (r < 0 && errno != EINTR) {
      set_error(o, errno_error(errno));
      stop = 1;
    } else if (r == 0 && deadline && now_ms() >= deadline) {
      set_error(o, X_ETIMEDOUT);
      deadline = 0;
      stop = 1;
    }
    for (nfds_t k = 0; k < n && r > 0 && !stop; ++k) {
      int i = which[k];
      if (!fds[k].revents)
        continue;
      if (i == 0) {
        ssize_t w = write(parent[0], c->input->data + written, input_length - written);
        if (w > 0)
          written += (size_t)w;
        if ((w < 0 && errno != EAGAIN) || written == input_length) {
          if (w < 0)
            set_pipe_error(o, errno_error(errno));
          close(parent[0]);
          parent[0] = -1;
        }
        continue;
      }
      ssize_t got = read(parent[i], chunk, 65536);
      if (got > 0) {
        out_append(&o->out[i], chunk, (size_t)got);
        buffered += (double)got;
        if (c->max_buffer > 0 && buffered > c->max_buffer) {
          set_error(o, X_ENOBUFS);
          stop = 1;
        }
      } else if (got == 0 || errno != EAGAIN) {
        if (got < 0)
          set_pipe_error(o, errno_error(errno));
        close(parent[i]);
        parent[i] = -1;
      }
    }
    if (stop && !killed) {
      killed = 1;
      if (!exited && kill(pid, c->kill_signal) != 0 && errno != ESRCH) {
        set_error(o, errno_error(errno));
        kill(pid, SIGKILL);
      }
      for (int i = 0; i < 3; ++i)
        if (parent[i] >= 0) {
          close(parent[i]);
          parent[i] = -1;
        }
    }
  }
  tz_free(chunk);
  if (WIFSIGNALED(wstatus)) {
    o->signal = WTERMSIG(wstatus);
    o->status = 0;
  } else {
    o->status = WEXITSTATUS(wstatus);
  }
}

#endif

//===----------------------------------------------------------------------===//
// What the call gives
//===----------------------------------------------------------------------===//

static TzStr str_of_buf(const TzBuf *b) { return tz_str_from_utf16(b->data, b->length); }

// "spawnSync <file>", the call of the system in Node's errors.
static TzStr syscall_of(const Call *c) {
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_ascii(&b, "spawnSync ");
  buf_take_units(&b, &c->file);
  return tz_buf_take(&b);
}

// "spawnSync <file> ENOENT": the message of ErrnoException.
static TzStr errno_message(const Call *c, int error) {
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_ascii(&b, "spawnSync ");
  buf_take_units(&b, &c->file);
  tz_buf_push(&b, ' ');
  tz_buf_ascii(&b, kErrors[error].code);
  return tz_buf_take(&b);
}

static void *system_error(const Call *c, int error, TzMaker make) {
  TzSystemErrorParts parts;
  memset(&parts, 0, sizeof parts);
  parts.message = errno_message(c, error);
  parts.name = ascii("Error");
  parts.errno_value = kErrors[error].number;
  parts.code = ascii(kErrors[error].code);
  parts.syscall = syscall_of(c);
  parts.path.value = str_of_buf(&c->file);
  parts.path.has = 1;
  return make(&parts);
}

// The output of each stream that was a pipe the child wrote to: decoded
// with the encoding, or a Buffer. A program that did not start gave nothing.
static void fill_output(const Call *c, const Outcome *o, TzSpawnParts *parts) {
  int32_t encoding = c->req->encoding;
  parts->text_mode = encoding > 0;
  for (int i = 1; i < 3; ++i) {
    if (c->req->stdio[i] != STDIO_PIPE)
      continue;
    parts->piped[i] = 1;
    const Out *out = &o->out[i];
    if (encoding > 0)
      parts->text[i] = tz_buffer_decode(out->data, (int64_t)out->length, encoding - 1);
    else
      parts->bytes[i] = tz_buffer_copy_of(out->data, (int64_t)out->length);
  }
}

static void release_output(TzSpawnParts *parts, int from) {
  for (int i = from; i < 3; ++i) {
    tz_str_release_value(parts->text[i]);
    parts->text[i].obj = NULL;
    parts->text[i].meta = 0;
    if (parts->bytes[i])
      tz_instance_release((TzInstance *)parts->bytes[i]);
    parts->bytes[i] = NULL;
  }
}

// What the call does when the program has run (or could not start).
static void *finish(Call *c, Outcome *o, TzSpawnParts *out, TzMaker make_system_error,
                    TzMaker make) {
  const TzSpawnRequest *req = c->req;
  int error = o->error != X_NONE ? o->error : o->pipe_error;
  TzSpawnParts parts;
  memset(&parts, 0, sizeof parts);
  fill_output(c, o, &parts);
  if (o->started) {
    parts.pid = o->pid;
    if (o->signal) {
      const char *name = signal_name(o->signal);
      parts.has_signal = 1;
      parts.signal = ascii(name ? name : "");
    } else {
      parts.has_status = 1;
      parts.status = (int32_t)o->status;
    }
  }
  if (req->mode == TZ_CHILD_SPAWN) {
    if (error != X_NONE)
      parts.error = system_error(c, error, make_system_error);
    return make(&parts);
  }

  // execSync and execFileSync: stderr goes on to this program's, unless the
  // call said where it goes.
  if (!(req->has & HAS_STDIO) && o->started && parts.piped[2]) {
    if (parts.text_mode)
      tz_stderr_write(parts.text[2].obj, parts.text[2].meta);
    else
      tz_std_write_bytes(2, o->out[2].data, o->out[2].length);
  }
  if (error != X_NONE && !o->started) {
    release_output(&parts, 0);
    tz_str_release_value(parts.signal);
    tz_thrown = system_error(c, error, make_system_error);
    return NULL;
  }
  if (error == X_NONE && parts.has_status && parts.status == 0) {
    // What it printed: the maker of the call reads stdout.
    out->text_mode = parts.text_mode;
    out->piped[1] = parts.piped[1];
    out->text[1] = parts.text[1];
    out->bytes[1] = parts.bytes[1];
    parts.text[1].obj = NULL;
    parts.text[1].meta = 0;
    parts.bytes[1] = NULL;
    release_output(&parts, 0);
    return NULL;
  }
  TzBuf m;
  tz_buf_init(&m);
  if (error != X_NONE) {
    // An error of the system while it ran: Node's error is that one, with
    // the result of spawnSync in it.
    TzStr message = errno_message(c, error);
    tz_buf_str(&m, message);
    tz_str_release_value(message);
    parts.has_code = 1;
    parts.errno_value = kErrors[error].number;
    parts.code = ascii(kErrors[error].code);
    parts.syscall = syscall_of(c);
    parts.path = str_of_buf(&c->file);
    parts.spawnargs = tz_arr_new(req->string_type, 0, (int64_t)c->args.count);
    for (size_t i = 1; i < c->args.count; ++i) {
      TzStr arg = str_of_buf(&c->args.items[i]);
      tz_arr_push_value(parts.spawnargs, &arg);
    }
  } else {
    // "Command failed: git status", then what it wrote to stderr.
    tz_buf_ascii(&m, "Command failed: ");
    if (req->mode == TZ_CHILD_EXEC) {
      tz_buf_str(&m, req->file);
    } else {
      tz_buf_str(&m, (req->has & HAS_ARGV0) ? req->argv0 : req->file);
      uint32_t count = req->args ? req->args->length : 0;
      for (uint32_t i = 0; i < count; ++i) {
        tz_buf_push(&m, ' ');
        tz_buf_str(&m, ((const TzStr *)req->args->data)[i]);
      }
    }
    if (parts.piped[2] && o->out[2].length > 0) {
      tz_buf_push(&m, '\n');
      if (parts.text_mode) {
        tz_buf_str(&m, parts.text[2]);
      } else {
        TzStr text = tz_buffer_decode(o->out[2].data, (int64_t)o->out[2].length, 0);
        tz_buf_str(&m, text);
        tz_str_release_value(text);
      }
    }
  }
  parts.message = tz_buf_take(&m);
  parts.name = ascii("Error");
  tz_thrown = make(&parts);
  return NULL;
}

// spawnSync, execFileSync and execSync. spawnSync gives the object `make`
// makes; the others leave stdout in `out` for the program, or throw an
// ExecSyncError that `make` makes, or a SystemError.
void *tz_child_run(const TzSpawnRequest *req, TzSpawnParts *out, TzMaker make_system_error,
                   TzMaker make) {
  memset(out, 0, sizeof *out);
  Call c;
  if (!prepare(&c, req)) {
    call_free(&c);
    return NULL;
  }
  Outcome o;
  memset(&o, 0, sizeof o);
  o.error = o.pipe_error = X_NONE;
  run(&c, &o);
  void *result = finish(&c, &o, out, make_system_error, make);
  for (int i = 0; i < 3; ++i)
    tz_free(o.out[i].data);
  call_free(&c);
  return result;
}
