#!/usr/bin/env python3
"""Independent oracle for the complete-tag-coverage NBT fixture.

Verifies round-trip of all 13 NBT tag types in one tree.  Invokes the C++
test binary at $PATH_TO_BINARY with --dump <tempfile>, parses with twoolie/NBT
(NBT==1.5.1, uncompressed-buffer path), and asserts every field's name, value,
and tag id.  See research/nbt_api_proposal.md §9 for the design choices that
make this oracle meaningful (in particular: float gets a 1e-6 tolerance per
§9.4, double is exact-equal, NaN/infinity are excluded from the fixture).

Fields exercised (one per spec tag id 1..12; TAG_End=0 appears as the terminator
after the root and nested compounds, completing all 13 tag types on the wire):

    ByteVal      TAG_Byte         -128
    ShortVal     TAG_Short        32767
    IntVal       TAG_Int          2147483647
    LongVal      TAG_Long         -9223372036854775807
    FloatVal     TAG_Float        3.14f          (binary32 round-trip)
    DoubleVal    TAG_Double       2.718281828459045
    ByteArr      TAG_Byte_Array   [0x01, 0x02, 0x03]
    StrVal       TAG_String       "hello"
    ListVal      TAG_List<Int>    [1, 2, 3]
    CompoundVal  TAG_Compound     { "Nested": TAG_Int = 42 }
    IntArr       TAG_Int_Array    [-1, 0, 1]
    LongArr      TAG_Long_Array   [100, 200, 300]

If the oracle disagrees with the C++ writer on ANY field, exit non-zero with a
clear comparison.  Never use this oracle to generate fixtures.
"""

import os
import subprocess
import sys
import tempfile

import nbt.nbt
from nbt.nbt import (
    NBTFile,
    TAG_BYTE,
    TAG_BYTE_ARRAY,
    TAG_COMPOUND,
    TAG_DOUBLE,
    TAG_FLOAT,
    TAG_INT,
    TAG_INT_ARRAY,
    TAG_LIST,
    TAG_LONG,
    TAG_LONG_ARRAY,
    TAG_SHORT,
    TAG_STRING,
)

FLOAT_TOL = 1e-6  # binary32 → Python float round-trip tolerance, per §9.4


def fail(message: str) -> None:
    print("FAIL: " + message)
    sys.exit(1)


def run_dump_and_parse(binary_path: str) -> NBTFile:
    fd_path = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix="nbt_oracle_cov_", suffix=".nbt", delete=False
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

        with open(fd_path, "rb") as raw:
            return NBTFile(buffer=raw)
    finally:
        if fd_path is not None and os.path.exists(fd_path):
            os.unlink(fd_path)


def check_id(tag, expected_id: int, label: str) -> None:
    if tag.id != expected_id:
        fail(
            "{label}: tag id is {got}, expected {want}".format(
                label=label, got=tag.id, want=expected_id
            )
        )


def require_key(container, key: str, container_label: str):
    if key not in container:
        fail("{label}: missing key {!r}".format(key, label=container_label))
    return container[key]


def check_scalar_value(tag, expected, label: str) -> None:
    if tag.value != expected:
        fail(
            "{label}: value {got!r}, expected {want!r}".format(
                label=label, got=tag.value, want=expected
            )
        )


def check_float_value(tag, expected: float, label: str) -> None:
    diff = abs(tag.value - expected)
    if diff > FLOAT_TOL:
        fail(
            "{label}: float value {got!r} differs from expected {want!r} "
            "by {diff:.3e} (tolerance {tol:.0e})".format(
                label=label, got=tag.value, want=expected,
                diff=diff, tol=FLOAT_TOL
            )
        )


def check_sequence_values(tag, expected_list: list, label: str) -> None:
    actual_list = list(tag.value) if hasattr(tag, "value") and not isinstance(
        tag.value, (int, float, str)
    ) else [child.value for child in tag]
    if actual_list != expected_list:
        fail(
            "{label}: sequence values {got!r}, expected {want!r}".format(
                label=label, got=actual_list, want=expected_list
            )
        )


def check_int_list(tag, expected_list: list, label: str) -> None:
    if tag.tagID != TAG_INT:
        fail(
            "{label}: list element type id is {got}, expected TAG_INT={want}"
            .format(label=label, got=tag.tagID, want=TAG_INT)
        )
    values = [child.value for child in tag]
    if values != expected_list:
        fail(
            "{label}: list values {got!r}, expected {want!r}".format(
                label=label, got=values, want=expected_list
            )
        )


def main() -> int:
    binary = os.environ.get("PATH_TO_BINARY")
    if not binary:
        fail("PATH_TO_BINARY env var is not set")
    if not os.path.isfile(binary):
        fail("PATH_TO_BINARY={!r} does not exist".format(binary))

    root = run_dump_and_parse(binary)
    check_id(root, TAG_COMPOUND, "root")

    # --- TAG_Byte (id 1) ---
    t = require_key(root, "ByteVal", "root")
    check_id(t, TAG_BYTE, "ByteVal")
    check_scalar_value(t, -128, "ByteVal")

    # --- TAG_Short (id 2) ---
    t = require_key(root, "ShortVal", "root")
    check_id(t, TAG_SHORT, "ShortVal")
    check_scalar_value(t, 32767, "ShortVal")

    # --- TAG_Int (id 3) ---
    t = require_key(root, "IntVal", "root")
    check_id(t, TAG_INT, "IntVal")
    check_scalar_value(t, 2147483647, "IntVal")

    # --- TAG_Long (id 4) ---
    t = require_key(root, "LongVal", "root")
    check_id(t, TAG_LONG, "LongVal")
    check_scalar_value(t, -9223372036854775807, "LongVal")

    # --- TAG_Float (id 5) ---
    t = require_key(root, "FloatVal", "root")
    check_id(t, TAG_FLOAT, "FloatVal")
    check_float_value(t, 3.14, "FloatVal")

    # --- TAG_Double (id 6) ---
    t = require_key(root, "DoubleVal", "root")
    check_id(t, TAG_DOUBLE, "DoubleVal")
    check_scalar_value(t, 2.718281828459045, "DoubleVal")

    # --- TAG_Byte_Array (id 7) ---
    t = require_key(root, "ByteArr", "root")
    check_id(t, TAG_BYTE_ARRAY, "ByteArr")
    if list(t.value) != [0x01, 0x02, 0x03]:
        fail(
            "ByteArr: bytes {got!r}, expected [1, 2, 3]".format(
                got=list(t.value)
            )
        )

    # --- TAG_String (id 8) ---
    t = require_key(root, "StrVal", "root")
    check_id(t, TAG_STRING, "StrVal")
    check_scalar_value(t, "hello", "StrVal")

    # --- TAG_List<TAG_Int> (id 9) ---
    t = require_key(root, "ListVal", "root")
    check_id(t, TAG_LIST, "ListVal")
    check_int_list(t, [1, 2, 3], "ListVal")

    # --- TAG_Compound (id 10) — nested ---
    t = require_key(root, "CompoundVal", "root")
    check_id(t, TAG_COMPOUND, "CompoundVal")
    nested = require_key(t, "Nested", "CompoundVal")
    check_id(nested, TAG_INT, "CompoundVal.Nested")
    check_scalar_value(nested, 42, "CompoundVal.Nested")
    if len(t) != 1:
        fail(
            "CompoundVal: contains {n} entries, expected exactly 1".format(
                n=len(t)
            )
        )

    # --- TAG_Int_Array (id 11) ---
    t = require_key(root, "IntArr", "root")
    check_id(t, TAG_INT_ARRAY, "IntArr")
    if list(t.value) != [-1, 0, 1]:
        fail(
            "IntArr: ints {got!r}, expected [-1, 0, 1]".format(
                got=list(t.value)
            )
        )

    # --- TAG_Long_Array (id 12) ---
    t = require_key(root, "LongArr", "root")
    check_id(t, TAG_LONG_ARRAY, "LongArr")
    if list(t.value) != [100, 200, 300]:
        fail(
            "LongArr: longs {got!r}, expected [100, 200, 300]".format(
                got=list(t.value)
            )
        )

    print("PASS: Python oracle agrees with C++ writer on all 13 tag types")
    print(
        "  parsed via twoolie/NBT v{ver} (uncompressed-buffer path)".format(
            ver=nbt._get_version()
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
