"""Exercise the production ZIP reader with real miniz and generated archives.

python run_zip_entry_host_tests.py --miniz-dir /path/to/miniz --cxx clang++ --cc clang
Windows: add --zig /path/to/zig.exe instead of --cxx/--cc.
--archive additionally checks a supplied XZ-compressed module ZIP, without running
its scripts. The subprocess boundary is mocked; this is not a device install test.
"""

import argparse
import binascii
import bz2
import lzma
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile
import zlib


def make_zip(data, method, *, name="module.prop", flags=0, bad_crc=False):
    name = name.encode("utf-8")
    compressed = (zlib.compress(data)[2:-4] if method == 8 else
                  bz2.compress(data) if method == 12 else
                  lzma.compress(data) if method == 95 else data)
    crc = binascii.crc32(data) ^ int(bad_crc)
    local = struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, flags, method, 0, 0,
                        crc, len(compressed), len(data), len(name), 0)
    local += name + compressed
    central = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, flags,
                          method, 0, 0, crc, len(compressed), len(data),
                          len(name), 0, 0, 0, 0, 0, 0) + name
    end = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 1, 1,
                      len(central), len(local), 0)
    return local + central + end


def read_xz_prop(archive):
    with zipfile.ZipFile(archive) as zipped:
        info = zipped.getinfo("module.prop")
        if info.compress_type != 95 or info.flag_bits & 1:
            raise ValueError("--archive expects an unencrypted XZ module.prop")
        with archive.open("rb") as stream:
            stream.seek(info.header_offset)
            header = stream.read(30)
            name_size, extra_size = struct.unpack_from("<HH", header, 26)
            stream.seek(name_size + extra_size, 1)
            data = lzma.decompress(stream.read(info.compress_size))
        if len(data) != info.file_size or binascii.crc32(data) != info.CRC:
            raise ValueError("Supplied module.prop failed size/CRC validation")
        return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--zig", type=Path)
    parser.add_argument("--miniz-dir", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    cc = [str(args.zig), "cc"] if args.zig else [args.cc]
    cxx = [str(args.zig), "c++"] if args.zig else [args.cxx]
    tests = Path(__file__).resolve().parent
    miniz = args.miniz_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="zip-entry-tests-") as temporary:
        build = Path(temporary)
        source = (tests.parent / "src/utils.cpp").read_text(encoding="utf-8")
        start = source.rfind("namespace {", 0, source.index("struct ZipFd"))
        end = source.index("\nbool append_file(", start)
        (build / "zip_entry_under_test.inc").write_text(source[start:end], encoding="utf-8")
        (build / "miniz_export.h").write_text("#define MINIZ_EXPORT\n", encoding="utf-8")

        expected = b"id=fixture_lsposed\nname=ZIP compatibility fixture\nversionCode=7912\n"
        (build / "expected.prop").write_bytes(expected)
        fixtures = {
            "stored": make_zip(expected, 0),
            "deflate": make_zip(expected, 8),
            "xz": make_zip(expected, 95),
            "bzip2": make_zip(expected, 12),
            "-module": make_zip(expected, 95),
            "xz spaces ; $()": make_zip(expected, 95),
            "unknown": make_zip(expected, 99),
            "missing": make_zip(expected, 8, name="README.md"),
            "nested": make_zip(expected, 8, name="folder/module.prop"),
            "invalid": b"not a ZIP archive",
            "crc_bad": make_zip(expected, 8, bad_crc=True),
            "encrypted": make_zip(expected, 8, flags=1),
            "encrypted_xz": make_zip(expected, 95, flags=1),
            "truncated": make_zip(expected, 8)[:-12],
        }
        for name, content in fixtures.items():
            (build / (name + ".zip")).write_bytes(content)

        definitions = ["-DMINIZ_NO_STDIO", "-DMINIZ_NO_TIME",
                       "-DMINIZ_NO_ARCHIVE_WRITING_APIS"]
        includes = ["-I" + str(build), "-I" + str(miniz)]
        objects = []
        for name in ("miniz", "miniz_tdef", "miniz_tinfl", "miniz_zip"):
            obj = build / (name + ".o")
            subprocess.run([*cc, "-std=c99", *definitions, *includes, "-c",
                            str(miniz / (name + ".c")), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = build / ("zip_entry_test.exe" if os.name == "nt" else "zip_entry_test")
        subprocess.run([*cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        *definitions, *includes, str(tests / "zip_entry_test.cpp"),
                        *objects, "-o", str(binary)], check=True)
        extra = []
        if args.archive:
            expected_archive = build / "archive.prop"
            expected_archive.write_bytes(read_xz_prop(args.archive.resolve()))
            extra = [str(args.archive.resolve()), str(expected_archive)]
        subprocess.run([str(binary), str(build), *extra], check=True, timeout=60)


if __name__ == "__main__":
    main()
