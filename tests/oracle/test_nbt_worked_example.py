#!/usr/bin/env python3
"""Independent oracle for the NBT writer's §5 worked example.

Invokes the C++ test binary (path supplied via the PATH_TO_BINARY environment
variable) with `--dump <tempfile>`, parses the resulting bytes with the
independently-implemented twoolie/NBT library (pinned NBT==1.5.1), and asserts
that the parsed logical tree matches the contract in
`research/nbt_api_proposal.md` §5:

    {
        "DataVersion": TAG_Int   = 4325,
        "Status":      TAG_String= "minecraft:full",
        "Position":    TAG_List  = [42L, -7L]  (element type TAG_Long),
    }

Uncompressed-NBT path: `nbt.nbt.NBTFile(buffer=<file-like>)`. The `filename=`
and `fileobj=` arguments wrap the input in GzipFile; the `buffer=` argument
reads raw bytes (twoolie/NBT v1.5.1 `nbt/nbt.py:608-631`).

If the oracle disagrees with the C++ writer at any field, ONE OF THEM IS
WRONG; the script exits non-zero with a clear comparison and we investigate
before proceeding.  Never use this oracle to generate fixtures.
"""

import os
import subprocess
import sys
import tempfile

import nbt.nbt
from nbt.nbt import (
    NBTFile,
    TAG_COMPOUND,
    TAG_INT,
    TAG_LIST,
    TAG_LONG,
    TAG_STRING,
)


def fail(message: str) -> "None":
    print("FAIL: " + message)
    sys.exit(1)


def run_dump_and_parse(binary_path: str) -> NBTFile:
    fd_path = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix="nbt_oracle_", suffix=".nbt", delete=False
        ) as fh:
            fd_path = fh.name

        proc = subprocess.run(
            [binary_path, "--dump", fd_path],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            fail(
                "C++ binary exited {rc}; stderr was:\n{err}".format(
                    rc=proc.returncode, err=proc.stderr
                )
            )

        size_on_disk = os.path.getsize(fd_path)
        if size_on_disk != 79:
            fail(
                "dumped file is {n} bytes; expected 79 per §5 fixture".format(
                    n=size_on_disk
                )
            )

        with open(fd_path, "rb") as raw:
            return NBTFile(buffer=raw)
    finally:
        if fd_path is not None and os.path.exists(fd_path):
            os.unlink(fd_path)


def assert_int_field(root, key: str, expected: int) -> None:
    if key not in root:
        fail("root has no {!r} entry".format(key))
    tag = root[key]
    if tag.id != TAG_INT:
        fail(
            "{key} tag id is {got}, expected TAG_INT={want}".format(
                key=key, got=tag.id, want=TAG_INT
            )
        )
    if tag.value != expected:
        fail(
            "{key} = {got!r}, expected {want!r}".format(
                key=key, got=tag.value, want=expected
            )
        )


def assert_string_field(root, key: str, expected: str) -> None:
    if key not in root:
        fail("root has no {!r} entry".format(key))
    tag = root[key]
    if tag.id != TAG_STRING:
        fail(
            "{key} tag id is {got}, expected TAG_STRING={want}".format(
                key=key, got=tag.id, want=TAG_STRING
            )
        )
    if tag.value != expected:
        fail(
            "{key} = {got!r}, expected {want!r}".format(
                key=key, got=tag.value, want=expected
            )
        )


def assert_long_list_field(root, key: str, expected: list) -> None:
    if key not in root:
        fail("root has no {!r} entry".format(key))
    tag = root[key]
    if tag.id != TAG_LIST:
        fail(
            "{key} tag id is {got}, expected TAG_LIST={want}".format(
                key=key, got=tag.id, want=TAG_LIST
            )
        )
    if tag.tagID != TAG_LONG:
        fail(
            "{key} element type id is {got}, expected TAG_LONG={want}".format(
                key=key, got=tag.tagID, want=TAG_LONG
            )
        )
    if len(tag) != len(expected):
        fail(
            "{key} length is {got}, expected {want}".format(
                key=key, got=len(tag), want=len(expected)
            )
        )
    values = [child.value for child in tag]
    if values != expected:
        fail(
            "{key} values = {got!r}, expected {want!r}".format(
                key=key, got=values, want=expected
            )
        )


def main() -> int:
    binary = os.environ.get("PATH_TO_BINARY")
    if not binary:
        fail("PATH_TO_BINARY env var is not set")
    if not os.path.isfile(binary):
        fail("PATH_TO_BINARY={!r} does not exist".format(binary))

    root = run_dump_and_parse(binary)

    if root.id != TAG_COMPOUND:
        fail(
            "root tag id is {got}, expected TAG_COMPOUND={want}".format(
                got=root.id, want=TAG_COMPOUND
            )
        )

    assert_int_field(root, "DataVersion", 4325)
    assert_string_field(root, "Status", "minecraft:full")
    assert_long_list_field(root, "Position", [42, -7])

    print("PASS: Python oracle agrees with C++ writer on §5 fixture")
    print(
        "  parsed via twoolie/NBT v{ver} (uncompressed-buffer path)".format(
            ver=nbt._get_version()
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
