# SPDX-License-Identifier: Apache-2.0
"""Public smpclient upload error decoding for filesystem storage failures."""

import cbor2
import pytest
from smp.error import MGMT_ERR
from smp.file_management import FileSystemManagementErrorV1
from smp.header import OP, CommandId, Flag, GroupId, Header, Version
from smpclient.exceptions import SMPUploadError

from brickphyrdev.storage_errors import upload_storage_failure


def upload_error(code, reason):
    payload = cbor2.dumps({"rc": int(code), "rsn": reason})
    header = Header(
        op=OP.WRITE_RSP,
        version=Version.V2,
        flags=Flag(0),
        length=len(payload),
        group_id=GroupId.FILE_MANAGEMENT,
        sequence=0,
        command_id=CommandId.FileManagement.FILE_DOWNLOAD_UPLOAD,
    )
    return SMPUploadError(FileSystemManagementErrorV1.loads(bytes(header) + payload))


@pytest.mark.parametrize(
    ("reason", "operation", "cause", "message"),
    [
        ("fs_open:ENOSPC", "open", "ENOSPC", "storage full during file open"),
        ("fs_stat:EIO", "stat", "EIO", "storage I/O error during file stat"),
        ("fs_write:EIO", "write", "EIO", "storage I/O error during file write"),
        ("fs_close:ENOSPC", "close", "ENOSPC", "storage full during file close"),
    ],
)
def test_known_upload_storage_failure(reason, operation, cause, message):
    failure = upload_storage_failure(upload_error(MGMT_ERR.EUNKNOWN, reason))

    assert failure is not None
    assert (failure.operation, failure.cause, str(failure)) == (operation, cause, message)


@pytest.mark.parametrize(
    ("code", "reason"),
    [
        (MGMT_ERR.EUNKNOWN, "fs_write:ERANGE"),
        (MGMT_ERR.EUNKNOWN, "unknown:ENOSPC"),
        (MGMT_ERR.EINVAL, "fs_write:ENOSPC"),
        (MGMT_ERR.EUNKNOWN, None),
    ],
)
def test_unrelated_upload_error_is_not_guessed(code, reason):
    assert upload_storage_failure(upload_error(code, reason)) is None


def test_non_response_upload_error_is_not_guessed():
    assert upload_storage_failure(SMPUploadError("timeout")) is None
