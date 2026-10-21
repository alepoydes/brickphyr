# SPDX-License-Identifier: Apache-2.0
"""Decode storage causes reported by the standard SMP file upload response."""

from dataclasses import dataclass

from smp.error import MGMT_ERR
from smp.file_management import FileSystemManagementErrorV1
from smpclient.exceptions import SMPUploadError

_OPERATIONS = frozenset({"open", "stat", "seek", "truncate", "unlink", "write", "close"})
_CAUSES = frozenset({"ENOSPC", "EIO"})


@dataclass(frozen=True)
class UploadStorageFailure:
    operation: str
    cause: str

    def __str__(self) -> str:
        if self.cause == "ENOSPC":
            return f"storage full during file {self.operation}"
        return f"storage I/O error during file {self.operation}"


def upload_storage_failure(error: SMPUploadError) -> UploadStorageFailure | None:
    """Return a known upload failure, or None for unrelated SMP upload errors."""
    if not error.args or not isinstance(error.args[0], FileSystemManagementErrorV1):
        return None

    reply = error.args[0]
    if reply.rc != MGMT_ERR.EUNKNOWN or reply.rsn is None:
        return None

    operation, separator, cause = reply.rsn.partition(":")
    if separator != ":" or not operation.startswith("fs_"):
        return None
    operation = operation.removeprefix("fs_")
    if operation not in _OPERATIONS or cause not in _CAUSES:
        return None
    return UploadStorageFailure(operation, cause)
