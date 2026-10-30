#include "compiler/library/child_process/child_error_types/child_error_types.h"
#include "compiler/library/builtin_modules/builtin_module_types/builtin_module_types.h"

namespace tz {

namespace {

const llvm::StringLiteral kChildTypes[] = {
    R"(type ExecTextError =
  | { kind: "EXEC_TEXT_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; shell: string }
  | { kind: "EXEC_TEXT_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; cwd: string }
  | { kind: "EXEC_TEXT_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; cwd: string }
  | { kind: "EXEC_TEXT_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string }
  | { kind: "EXEC_TEXT_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string }
  | { kind: "EXEC_TEXT_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string }
  | { kind: "EXEC_TEXT_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string }
  | { kind: "EXEC_TEXT_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_TEXT_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; timeout: int; signal: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_TEXT_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_TEXT_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; command: string; status: int; stdout?: string; stderr?: string }
  | { kind: "EXEC_TEXT_ERROR_SHELL_COMMAND_NOT_FOUND"; code: "ERR_COMMAND_NOT_FOUND"; message: string; command: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_TEXT_ERROR_SHELL_COMMAND_NOT_RUNNABLE"; code: "ERR_COMMAND_NOT_RUNNABLE"; message: string; command: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_TEXT_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; command: string; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: string; stderr?: string }
  | { kind: "EXEC_TEXT_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; command: string; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: string; stderr?: string }
  | { kind: "EXEC_TEXT_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; command: string; signal: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_TEXT_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
  | { kind: "EXEC_TEXT_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
  | { kind: "EXEC_TEXT_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
  | { kind: "EXEC_TEXT_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string }
  | { kind: "EXEC_TEXT_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string }
)",
    R"(type ExecBytesError =
  | { kind: "EXEC_BYTES_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; shell: string }
  | { kind: "EXEC_BYTES_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; cwd: string }
  | { kind: "EXEC_BYTES_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; cwd: string }
  | { kind: "EXEC_BYTES_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string }
  | { kind: "EXEC_BYTES_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string }
  | { kind: "EXEC_BYTES_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string }
  | { kind: "EXEC_BYTES_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string }
  | { kind: "EXEC_BYTES_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_BYTES_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; timeout: int; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_BYTES_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_BYTES_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; command: string; status: int; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_BYTES_ERROR_SHELL_COMMAND_NOT_FOUND"; code: "ERR_COMMAND_NOT_FOUND"; message: string; command: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_BYTES_ERROR_SHELL_COMMAND_NOT_RUNNABLE"; code: "ERR_COMMAND_NOT_RUNNABLE"; message: string; command: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_BYTES_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; command: string; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_BYTES_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; command: string; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_BYTES_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; command: string; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_BYTES_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
  | { kind: "EXEC_BYTES_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
  | { kind: "EXEC_BYTES_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
  | { kind: "EXEC_BYTES_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string }
  | { kind: "EXEC_BYTES_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string }
)",
    R"(type ExecFileTextError =
  | { kind: "EXEC_FILE_TEXT_ERROR_PROGRAM_NOT_FOUND_IN_PATH"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_PROGRAM_FILE_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; cwd: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; file: string; args: string[]; cwd: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_NEEDS_ADMINISTRATOR"; code: "EACCES"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_BATCH_FILE_WITHOUT_SHELL"; code: "EINVAL"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_NOT_A_PROGRAM"; code: "EFTYPE" | "ENOEXEC"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_PATH_TOO_LONG"; code: "ENAMETOOLONG"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; file: string; args: string[]; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; file: string; args: string[]; timeout: int; signal: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; file: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; file: string; args: string[]; status: int; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; file: string; args: string[]; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
  | { kind: "EXEC_FILE_TEXT_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
  | { kind: "EXEC_FILE_TEXT_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_TEXT_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; file: string; args: string[] }
)",
    R"(type ExecFileBytesError =
  | { kind: "EXEC_FILE_BYTES_ERROR_PROGRAM_NOT_FOUND_IN_PATH"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_PROGRAM_FILE_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; cwd: string }
  | { kind: "EXEC_FILE_BYTES_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; file: string; args: string[]; cwd: string }
  | { kind: "EXEC_FILE_BYTES_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_NEEDS_ADMINISTRATOR"; code: "EACCES"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_BATCH_FILE_WITHOUT_SHELL"; code: "EINVAL"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_NOT_A_PROGRAM"; code: "EFTYPE" | "ENOEXEC"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_PATH_TOO_LONG"; code: "ENAMETOOLONG"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; file: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_BYTES_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; file: string; args: string[]; timeout: int; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_BYTES_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; file: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_BYTES_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; file: string; args: string[]; status: int; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_BYTES_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; file: string; args: string[]; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_BYTES_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_BYTES_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_BYTES_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
  | { kind: "EXEC_FILE_BYTES_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
  | { kind: "EXEC_FILE_BYTES_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
  | { kind: "EXEC_FILE_BYTES_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
  | { kind: "EXEC_FILE_BYTES_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_BYTES_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; file: string; args: string[] }
)",
    R"(type SpawnTextError =
  | { kind: "SPAWN_TEXT_ERROR_PROGRAM_NOT_FOUND_IN_PATH"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_PROGRAM_FILE_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; cwd: string }
  | { kind: "SPAWN_TEXT_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; args: string[]; cwd: string }
  | { kind: "SPAWN_TEXT_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_NEEDS_ADMINISTRATOR"; code: "EACCES"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_BATCH_FILE_WITHOUT_SHELL"; code: "EINVAL"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_NOT_A_PROGRAM"; code: "EFTYPE" | "ENOEXEC"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_PATH_TOO_LONG"; code: "ENAMETOOLONG"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; args: string[]; stdout?: string; stderr?: string }
  | { kind: "SPAWN_TEXT_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; args: string[]; timeout: int; signal: string; stdout?: string; stderr?: string }
  | { kind: "SPAWN_TEXT_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: string; stderr?: string }
  | { kind: "SPAWN_TEXT_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
  | { kind: "SPAWN_TEXT_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
  | { kind: "SPAWN_TEXT_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
  | { kind: "SPAWN_TEXT_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
  | { kind: "SPAWN_TEXT_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_TEXT_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string; args: string[] }
)",
    R"(type SpawnBytesError =
  | { kind: "SPAWN_BYTES_ERROR_PROGRAM_NOT_FOUND_IN_PATH"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_PROGRAM_FILE_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; cwd: string }
  | { kind: "SPAWN_BYTES_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; args: string[]; cwd: string }
  | { kind: "SPAWN_BYTES_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_NEEDS_ADMINISTRATOR"; code: "EACCES"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_BATCH_FILE_WITHOUT_SHELL"; code: "EINVAL"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_NOT_A_PROGRAM"; code: "EFTYPE" | "ENOEXEC"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_PATH_TOO_LONG"; code: "ENAMETOOLONG"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
  | { kind: "SPAWN_BYTES_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; args: string[]; timeout: int; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "SPAWN_BYTES_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "SPAWN_BYTES_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
  | { kind: "SPAWN_BYTES_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
  | { kind: "SPAWN_BYTES_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
  | { kind: "SPAWN_BYTES_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
  | { kind: "SPAWN_BYTES_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_BYTES_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string; args: string[] }
)",
    R"(type ExecFileShellTextError =
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; shell: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; cwd: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; file: string; args: string[]; cwd: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; file: string; args: string[]; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; file: string; args: string[]; timeout: int; signal: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; file: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; file: string; args: string[]; status: int; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_SHELL_COMMAND_NOT_FOUND"; code: "ERR_COMMAND_NOT_FOUND"; message: string; file: string; args: string[]; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_SHELL_COMMAND_NOT_RUNNABLE"; code: "ERR_COMMAND_NOT_RUNNABLE"; message: string; file: string; args: string[]; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; file: string; args: string[]; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: string; stdout?: string; stderr?: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; file: string; args: string[] }
)",
    R"(type ExecFileShellBytesError =
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; shell: string }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; cwd: string }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; file: string; args: string[]; cwd: string }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; file: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; file: string; args: string[]; timeout: int; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; file: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; file: string; args: string[]; status: int; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_SHELL_COMMAND_NOT_FOUND"; code: "ERR_COMMAND_NOT_FOUND"; message: string; file: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_SHELL_COMMAND_NOT_RUNNABLE"; code: "ERR_COMMAND_NOT_RUNNABLE"; message: string; file: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; file: string; args: string[]; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; file: string; args: string[] }
  | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; file: string; args: string[] }
)",
    R"(type SpawnShellTextError =
  | { kind: "SPAWN_SHELL_TEXT_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; shell: string }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; cwd: string }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; args: string[]; cwd: string }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; args: string[]; stdout?: string; stderr?: string }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; args: string[]; timeout: int; signal: string; stdout?: string; stderr?: string }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: string; stderr?: string }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_SHELL_TEXT_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string; args: string[] }
)",
    R"(type SpawnShellBytesError =
  | { kind: "SPAWN_SHELL_BYTES_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; shell: string }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; cwd: string }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; args: string[]; cwd: string }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; args: string[]; timeout: int; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: Buffer; stderr?: Buffer }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string; args: string[] }
  | { kind: "SPAWN_SHELL_BYTES_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string; args: string[] }
)",
    R"(type SpawnTextResult = { pid: int; status?: int; signal?: string; output: (string | undefined)[]; stdout?: string; stderr?: string }
)",
    R"(type SpawnBytesResult = { pid: int; status?: int; signal?: string; output: (Buffer | undefined)[]; stdout?: Buffer; stderr?: Buffer }
)",
};

const llvm::StringLiteral kChildTypeNames[] = {
    "ExecTextError",          "ExecBytesError",          "ExecFileTextError",
    "ExecFileBytesError",     "SpawnTextError",          "SpawnBytesError",
    "ExecFileShellTextError", "ExecFileShellBytesError", "SpawnShellTextError",
    "SpawnShellBytesError",   "SpawnTextResult",         "SpawnBytesResult"};

}

ChildErrorTypes::ChildErrorTypes(Checker &checker) : checker_(checker) {}

void ChildErrorTypes::declare() {
  BuiltinModuleTypes &types = checker_.builtinModuleTypes();
  if (types.declared("ExecTextError")) {
    return;
  }
  types.declare(kChildTypes, "<node:child_process errors>");
  for (llvm::StringLiteral name : kChildTypeNames) {
    types.exportFrom(NodeModule::ChildProcess, name);
  }
}

const Type *ChildErrorTypes::errorOf(llvm::StringRef fn, bool text, bool shell) {
  std::string name = fn == "execSync"       ? "Exec"
                     : fn == "execFileSync" ? (shell ? "ExecFileShell" : "ExecFile")
                                            : (shell ? "SpawnShell" : "Spawn");
  name += text ? "TextError" : "BytesError";

  return checker_.builtinModuleTypes().named(name);
}

const Type *ChildErrorTypes::spawnResult(bool text) {
  return checker_.builtinModuleTypes().named(text ? "SpawnTextResult" : "SpawnBytesResult");
}

}
