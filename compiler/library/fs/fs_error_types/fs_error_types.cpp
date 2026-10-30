#include "compiler/library/fs/fs_error_types/fs_error_types.h"
#include "compiler/library/builtin_modules/builtin_module_types/builtin_module_types.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

namespace {

const llvm::StringLiteral kFsErrorTypes[] = {
    R"(type ReadFileError =
  | { kind: "READ_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "READ_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "READ_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
  | { kind: "READ_FILE_ERROR_BUSY"; code: "EBUSY" | "EAGAIN" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
  | { kind: "READ_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "READ_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type WriteFileError =
  | { kind: "WRITE_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "WRITE_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "WRITE_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "WRITE_FILE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
  | { kind: "WRITE_FILE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string }
  | { kind: "WRITE_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "WRITE_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type WriteNewFileError =
  | { kind: "WRITE_NEW_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "WRITE_NEW_FILE_ERROR_EXISTS"; code: "EEXIST"; message: string; syscall: string; path: string }
  | { kind: "WRITE_NEW_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "WRITE_NEW_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "WRITE_NEW_FILE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
  | { kind: "WRITE_NEW_FILE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string }
  | { kind: "WRITE_NEW_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "WRITE_NEW_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type AppendFileError =
  | { kind: "APPEND_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "APPEND_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "APPEND_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "APPEND_FILE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
  | { kind: "APPEND_FILE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string }
  | { kind: "APPEND_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "APPEND_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type AppendNewFileError =
  | { kind: "APPEND_NEW_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "APPEND_NEW_FILE_ERROR_EXISTS"; code: "EEXIST"; message: string; syscall: string; path: string }
  | { kind: "APPEND_NEW_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "APPEND_NEW_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "APPEND_NEW_FILE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
  | { kind: "APPEND_NEW_FILE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string }
  | { kind: "APPEND_NEW_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "APPEND_NEW_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type TruncateError =
  | { kind: "TRUNCATE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "TRUNCATE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "TRUNCATE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "TRUNCATE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
  | { kind: "TRUNCATE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string }
  | { kind: "TRUNCATE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "TRUNCATE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type AccessError =
  | { kind: "ACCESS_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "ACCESS_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "ACCESS_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "ACCESS_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "ACCESS_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type ChangeModeError =
  | { kind: "CHANGE_MODE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "CHANGE_MODE_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "CHANGE_MODE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "CHANGE_MODE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "CHANGE_MODE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type StatusError =
  | { kind: "STATUS_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "STATUS_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "STATUS_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
  | { kind: "STATUS_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
  | { kind: "STATUS_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "STATUS_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type StatusIfExistsError =
  | { kind: "STATUS_IF_EXISTS_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "STATUS_IF_EXISTS_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
  | { kind: "STATUS_IF_EXISTS_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
  | { kind: "STATUS_IF_EXISTS_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "STATUS_IF_EXISTS_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type LinkStatusError =
  | { kind: "LINK_STATUS_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "LINK_STATUS_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "LINK_STATUS_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
  | { kind: "LINK_STATUS_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
  | { kind: "LINK_STATUS_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "LINK_STATUS_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type LinkStatusIfExistsError =
  | { kind: "LINK_STATUS_IF_EXISTS_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "LINK_STATUS_IF_EXISTS_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
  | { kind: "LINK_STATUS_IF_EXISTS_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
  | { kind: "LINK_STATUS_IF_EXISTS_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "LINK_STATUS_IF_EXISTS_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type ReadDirectoryError =
  | { kind: "READ_DIRECTORY_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "READ_DIRECTORY_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "READ_DIRECTORY_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
  | { kind: "READ_DIRECTORY_ERROR_BUSY"; code: "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
  | { kind: "READ_DIRECTORY_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL" | "ECHARSET"; message: string; syscall: string; path: string }
  | { kind: "READ_DIRECTORY_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type RealPathError =
  | { kind: "REAL_PATH_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "REAL_PATH_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "REAL_PATH_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
  | { kind: "REAL_PATH_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
  | { kind: "REAL_PATH_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL" | "ECHARSET"; message: string; syscall: string; path: string }
  | { kind: "REAL_PATH_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type MakeDirectoryError =
  | { kind: "MAKE_DIRECTORY_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "MAKE_DIRECTORY_ERROR_EXISTS"; code: "EEXIST"; message: string; syscall: string; path: string }
  | { kind: "MAKE_DIRECTORY_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "MAKE_DIRECTORY_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "MAKE_DIRECTORY_ERROR_FULL"; code: "ENOSPC" | "EMLINK"; message: string; syscall: string; path: string }
  | { kind: "MAKE_DIRECTORY_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "MAKE_DIRECTORY_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type MakeTemporaryDirectoryError =
  | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_FULL"; code: "ENOSPC" | "EMLINK"; message: string; syscall: string; path: string }
  | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type RemoveDirectoryError =
  | { kind: "REMOVE_DIRECTORY_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_DIRECTORY_ERROR_NOT_EMPTY"; code: "ENOTEMPTY"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_DIRECTORY_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_DIRECTORY_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_DIRECTORY_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_DIRECTORY_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_DIRECTORY_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type UnlinkError =
  | { kind: "UNLINK_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "UNLINK_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "UNLINK_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "UNLINK_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
  | { kind: "UNLINK_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
  | { kind: "UNLINK_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type RemoveError =
  | { kind: "REMOVE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_ERROR_NOT_EMPTY"; code: "ENOTEMPTY"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_ERROR_BUSY"; code: "EBUSY" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL" | "ECHARSET"; message: string; syscall: string; path: string }
  | { kind: "REMOVE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type ForceRemoveError =
  | { kind: "FORCE_REMOVE_ERROR_NOT_EMPTY"; code: "ENOTEMPTY"; message: string; syscall: string; path: string }
  | { kind: "FORCE_REMOVE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
  | { kind: "FORCE_REMOVE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
  | { kind: "FORCE_REMOVE_ERROR_BUSY"; code: "EBUSY" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
  | { kind: "FORCE_REMOVE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL" | "ECHARSET"; message: string; syscall: string; path: string }
  | { kind: "FORCE_REMOVE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
)",
    R"(type RenameError =
  | { kind: "RENAME_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string; dest: string }
  | { kind: "RENAME_ERROR_EXISTS"; code: "EEXIST"; message: string; syscall: string; path: string; dest: string }
  | { kind: "RENAME_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string; dest: string }
  | { kind: "RENAME_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string; dest: string }
  | { kind: "RENAME_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string; dest: string }
  | { kind: "RENAME_ERROR_FULL"; code: "ENOSPC" | "EMLINK"; message: string; syscall: string; path: string; dest: string }
  | { kind: "RENAME_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string; dest: string }
  | { kind: "RENAME_ERROR_UNSUPPORTED"; code: "EXDEV"; message: string; syscall: string; path: string; dest: string }
  | { kind: "RENAME_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string; dest: string }
)",
    R"(type ReplaceError =
  | { kind: "REPLACE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string; dest: string }
  | { kind: "REPLACE_ERROR_NOT_EMPTY"; code: "ENOTEMPTY" | "EEXIST"; message: string; syscall: string; path: string; dest: string }
  | { kind: "REPLACE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string; dest: string }
  | { kind: "REPLACE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string; dest: string }
  | { kind: "REPLACE_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string; dest: string }
  | { kind: "REPLACE_ERROR_FULL"; code: "ENOSPC" | "EMLINK"; message: string; syscall: string; path: string; dest: string }
  | { kind: "REPLACE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string; dest: string }
  | { kind: "REPLACE_ERROR_UNSUPPORTED"; code: "EXDEV"; message: string; syscall: string; path: string; dest: string }
  | { kind: "REPLACE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string; dest: string }
)",
    R"(type CopyFileError =
  | { kind: "COPY_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string; dest: string }
  | { kind: "COPY_FILE_ERROR_EXISTS"; code: "EEXIST"; message: string; syscall: string; path: string; dest: string }
  | { kind: "COPY_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string; dest: string }
  | { kind: "COPY_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string; dest: string }
  | { kind: "COPY_FILE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string; dest: string }
  | { kind: "COPY_FILE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string; dest: string }
  | { kind: "COPY_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string; dest: string }
  | { kind: "COPY_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string; dest: string }
)",
    R"(interface ReadStreamErrorNotFound extends StreamError { kind: "READ_STREAM_ERROR_NOT_FOUND"; code: "ENOENT"; syscall: string; path: string }
interface ReadStreamErrorWrongType extends StreamError { kind: "READ_STREAM_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; syscall: string; path: string }
interface ReadStreamErrorPermission extends StreamError { kind: "READ_STREAM_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; syscall: string; path: string }
interface ReadStreamErrorBusy extends StreamError { kind: "READ_STREAM_ERROR_BUSY"; code: "EBUSY" | "EAGAIN" | "EMFILE" | "ENFILE"; syscall: string; path: string }
interface ReadStreamErrorPath extends StreamError { kind: "READ_STREAM_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; syscall: string; path: string }
interface ReadStreamErrorSystem extends StreamError { kind: "READ_STREAM_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "UNKNOWN"; syscall: string; path: string }
interface ReadStreamErrorInvalidRange extends StreamError { kind: "READ_STREAM_ERROR_INVALID_RANGE"; code: "ERR_OUT_OF_RANGE"; path: string }
type ReadStreamError = ReadStreamErrorNotFound | ReadStreamErrorWrongType | ReadStreamErrorPermission | ReadStreamErrorBusy | ReadStreamErrorPath | ReadStreamErrorSystem | ReadStreamErrorInvalidRange
)",
    R"(interface WriteStreamErrorNotFound extends StreamError { kind: "WRITE_STREAM_ERROR_NOT_FOUND"; code: "ENOENT"; syscall: string; path: string }
interface WriteStreamErrorWrongType extends StreamError { kind: "WRITE_STREAM_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; syscall: string; path: string }
interface WriteStreamErrorPermission extends StreamError { kind: "WRITE_STREAM_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; syscall: string; path: string }
interface WriteStreamErrorBusy extends StreamError { kind: "WRITE_STREAM_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; syscall: string; path: string }
interface WriteStreamErrorFull extends StreamError { kind: "WRITE_STREAM_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; syscall: string; path: string }
interface WriteStreamErrorPath extends StreamError { kind: "WRITE_STREAM_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; syscall: string; path: string }
interface WriteStreamErrorSystem extends StreamError { kind: "WRITE_STREAM_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; syscall: string; path: string }
interface WriteStreamErrorInvalidRange extends StreamError { kind: "WRITE_STREAM_ERROR_INVALID_RANGE"; code: "ERR_OUT_OF_RANGE"; path: string }
type WriteStreamError = WriteStreamErrorNotFound | WriteStreamErrorWrongType | WriteStreamErrorPermission | WriteStreamErrorBusy | WriteStreamErrorFull | WriteStreamErrorPath | WriteStreamErrorSystem | WriteStreamErrorInvalidRange
)",
    R"(interface WriteNewStreamErrorNotFound extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_NOT_FOUND"; code: "ENOENT"; syscall: string; path: string }
interface WriteNewStreamErrorExists extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_EXISTS"; code: "EEXIST"; syscall: string; path: string }
interface WriteNewStreamErrorWrongType extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; syscall: string; path: string }
interface WriteNewStreamErrorPermission extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; syscall: string; path: string }
interface WriteNewStreamErrorBusy extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; syscall: string; path: string }
interface WriteNewStreamErrorFull extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; syscall: string; path: string }
interface WriteNewStreamErrorPath extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; syscall: string; path: string }
interface WriteNewStreamErrorSystem extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; syscall: string; path: string }
interface WriteNewStreamErrorInvalidRange extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_INVALID_RANGE"; code: "ERR_OUT_OF_RANGE"; path: string }
type WriteNewStreamError = WriteNewStreamErrorNotFound | WriteNewStreamErrorExists | WriteNewStreamErrorWrongType | WriteNewStreamErrorPermission | WriteNewStreamErrorBusy | WriteNewStreamErrorFull | WriteNewStreamErrorPath | WriteNewStreamErrorSystem | WriteNewStreamErrorInvalidRange
)",
};

struct ErrorVerb {
  llvm::StringLiteral type;
  llvm::StringLiteral verb;
};

const ErrorVerb kVerbs[] = {
    {"ReadFileError", "read"},
    {"WriteFileError", "write"},
    {"WriteNewFileError", "write"},
    {"AppendFileError", "append to"},
    {"AppendNewFileError", "append to"},
    {"TruncateError", "truncate"},
    {"AccessError", "check"},
    {"ChangeModeError", "change the mode of"},
    {"StatusError", "get the info of"},
    {"StatusIfExistsError", "get the info of"},
    {"LinkStatusError", "get the info of"},
    {"LinkStatusIfExistsError", "get the info of"},
    {"ReadDirectoryError", "list"},
    {"RealPathError", "resolve"},
    {"MakeDirectoryError", "create the folder"},
    {"MakeTemporaryDirectoryError", "create a temporary folder at"},
    {"RemoveDirectoryError", "remove the folder"},
    {"UnlinkError", "remove"},
    {"RemoveError", "remove"},
    {"ForceRemoveError", "remove"},
    {"RenameError", "move"},
    {"ReplaceError", "move"},
    {"CopyFileError", "copy"},
    {"ReadStreamError", "read"},
    {"WriteStreamError", "write"},
    {"WriteNewStreamError", "write"},
};

std::string textOf(const Expr *expr) {
  auto *lit = expr ? dyn_cast<StringLitExpr>(expr) : nullptr;
  if (!lit) {
    return "";
  }
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(lit->value.data()),
                                  lit->value.size()),
      out);

  return out;
}

bool literalTrue(const Expr *expr) {
  auto *lit = expr ? dyn_cast<BoolLitExpr>(expr) : nullptr;

  return lit && lit->value;
}

}

FsErrorTypes::FsErrorTypes(Checker &checker) : checker_(checker) {}

void FsErrorTypes::declare() {
  BuiltinModuleTypes &types = checker_.builtinModuleTypes();
  if (types.declared("ReadFileError")) {
    return;
  }
  types.declare(kFsErrorTypes, "<node:fs errors>");
  for (const ErrorVerb &entry : kVerbs) {
    if (entry.type == "ReplaceError") {
      types.exportFrom(NodeModule::TzFs, entry.type);
      continue;
    }
    types.exportFrom(NodeModule::Fs, entry.type);
    types.exportFrom(NodeModule::FsPromises, entry.type);
  }
  for (llvm::StringRef side : {"Read", "Write", "WriteNew"}) {
    for (llvm::StringRef reason : {"NotFound", "Exists", "WrongType", "Permission", "Busy", "Full",
                                   "Path", "System", "InvalidRange"}) {
      std::string name = (side + "StreamError" + reason).str();
      if (types.declared(name)) {
        types.exportFrom(NodeModule::Fs, name);
      }
    }
  }
}

const Type *FsErrorTypes::named(llvm::StringRef name) {
  return checker_.builtinModuleTypes().named(name);
}

const Type *FsErrorTypes::ofCall(const CallExpr *call) {
  auto arg = [&](size_t i) -> const Expr * {
    return i < call->args.size() ? call->args[i] : nullptr;
  };
  switch (call->builtin) {
  case Builtin::FsReadFile:
    return named("ReadFileError");
  case Builtin::FsWriteFile: {
    std::string flag = textOf(arg(2));
    bool append = flag.find('a') != std::string::npos;
    bool fresh = flag.find('x') != std::string::npos;
    if (append) {
      return named(fresh ? "AppendNewFileError" : "AppendFileError");
    }
    return named(fresh ? "WriteNewFileError" : "WriteFileError");
  }
  case Builtin::FsAccess:
    return named("AccessError");
  case Builtin::FsChmod:
    return named("ChangeModeError");
  case Builtin::FsTruncate:
    return named("TruncateError");
  case Builtin::FsStat: {
    bool follow = literalTrue(arg(1));
    bool throwIfMissing = literalTrue(arg(2));
    if (follow) {
      return named(throwIfMissing ? "StatusError" : "StatusIfExistsError");
    }
    return named(throwIfMissing ? "LinkStatusError" : "LinkStatusIfExistsError");
  }
  case Builtin::FsReaddir:
    return named("ReadDirectoryError");
  case Builtin::FsMkdir:
    return named("MakeDirectoryError");
  case Builtin::FsMkdtemp:
    return named("MakeTemporaryDirectoryError");
  case Builtin::FsRmdir:
    return named("RemoveDirectoryError");
  case Builtin::FsUnlink:
    return named("UnlinkError");
  case Builtin::FsRm:
    return named(literalTrue(arg(2)) ? "ForceRemoveError" : "RemoveError");
  case Builtin::FsRename:
    return named("RenameError");
  case Builtin::FsReplace:
    return named("ReplaceError");
  case Builtin::FsCopyFile:
    return named("CopyFileError");
  case Builtin::FsRealpath:
    return named("RealPathError");
  default:
    return Type::error();
  }
}

llvm::StringRef FsErrorTypes::verbOf(llvm::StringRef name) {
  for (const ErrorVerb &entry : kVerbs) {
    if (entry.type == name) {
      return entry.verb;
    }
  }

  return "";
}

}
