#!/usr/bin/env python3
import argparse
import contextlib
import io
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


THREAD_LIST = 3
MODULE_LIST = 4
EXCEPTION = 6
THREAD_ENTRY_SIZE = 48
MODULE_ENTRY_SIZE = 108
EXCEPTION_STREAM_SIZE = 168
ARM64_CONTEXT_SIZE = 272


def read_u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def read_u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def read_utf16_string(data, offset):
    length = read_u32(data, offset)
    return data[offset + 4:offset + 4 + length].decode("utf-16le").rstrip("\0")


def parse_minidump(data):
    if len(data) < 32 or data[:4] != b"MDMP":
        raise ValueError("not a minidump")

    stream_count = read_u32(data, 8)
    directory_rva = read_u32(data, 12)
    streams = {}
    for index in range(stream_count):
        offset = directory_rva + index * 12
        stream_type, stream_size, stream_rva = struct.unpack_from("<III", data, offset)
        streams[stream_type] = (stream_size, stream_rva)

    try:
        exception_size, exception_rva = streams[EXCEPTION]
        thread_size, thread_rva = streams[THREAD_LIST]
        module_size, module_rva = streams[MODULE_LIST]
    except KeyError as error:
        raise ValueError(f"minidump is missing stream {error.args[0]}") from error
    if exception_size < EXCEPTION_STREAM_SIZE:
        raise ValueError("minidump exception stream is truncated")

    thread_id = read_u32(data, exception_rva)
    exception_code = read_u32(data, exception_rva + 8)
    exception_address = read_u64(data, exception_rva + 24)
    context_size, context_rva = struct.unpack_from("<II", data, exception_rva + 160)
    if context_size < ARM64_CONTEXT_SIZE:
        raise ValueError("exception context is not a complete ARM64 context")
    # ARM64_CONTEXT stores X0-X28, FP, LR, SP, then PC after its flags.
    frame_pointer = read_u64(data, context_rva + 8 + 29 * 8)
    program_counter = read_u64(data, context_rva + 8 + 32 * 8)

    thread_count = read_u32(data, thread_rva)
    stack_start = stack_size = stack_rva = None
    for index in range(thread_count):
        offset = thread_rva + 4 + index * THREAD_ENTRY_SIZE
        if read_u32(data, offset) == thread_id:
            stack_start = read_u64(data, offset + 24)
            stack_size, stack_rva = struct.unpack_from("<II", data, offset + 32)
            break
    if stack_start is None:
        raise ValueError(f"exception thread {thread_id} is missing from thread list")

    module_count = read_u32(data, module_rva)
    modules = []
    for index in range(module_count):
        offset = module_rva + 4 + index * MODULE_ENTRY_SIZE
        image_base = read_u64(data, offset)
        image_size = read_u32(data, offset + 8)
        name_rva = read_u32(data, offset + 20)
        modules.append({
            "base": image_base,
            "size": image_size,
            "name": read_utf16_string(data, name_rva),
        })

    def module_for(address):
        return next((
            module for module in modules
            if module["base"] <= address < module["base"] + module["size"]
        ), None)

    fault_module = module_for(program_counter)
    if not fault_module:
        raise ValueError(f"faulting PC {program_counter:#x} is outside all modules")

    frames = [program_counter]
    frame = frame_pointer
    for _ in range(64):
        if (frame < stack_start
                or frame + 16 > stack_start + stack_size
                or frame % 8):
            break
        offset = stack_rva + (frame - stack_start)
        previous_frame = read_u64(data, offset)
        return_address = read_u64(data, offset + 8)
        if not return_address:
            break
        # ARM64 return addresses point just after the call instruction.
        frames.append(return_address - 4)
        if previous_frame <= frame:
            break
        frame = previous_frame

    return {
        "thread_id": thread_id,
        "exception_code": exception_code,
        "exception_address": exception_address,
        "pc": program_counter,
        "module": fault_module,
        "modules": modules,
        "frames": [
            {"address": address, "module": module_for(address)}
            for address in frames
        ],
    }


def symbolicate(report, executable):
    image = Path(executable)
    if not image.is_file():
        raise ValueError(f"Teagram executable is missing: {executable}")

    print(
        "Exception: thread=%d code=%#x address=%#x"
        % (report["thread_id"], report["exception_code"], report["exception_address"])
    )
    module = report["module"]
    print(
        "Faulting module: %s base=%#x"
        % (module["name"], module["base"])
    )
    candidates = [
        candidate for candidate in report["modules"]
        if Path(candidate["name"]).name == image.name
    ]
    exact = [
        candidate for candidate in candidates
        if Path(candidate["name"]) in (image, image.resolve())
    ]
    executable_module = (
        exact[0] if len(exact) == 1
        else candidates[0] if len(candidates) == 1
        else None
    )
    if executable_module is None:
        reason = "ambiguous" if candidates else "not found"
        print("Executable module %s in minidump: %s" % (reason, image.name))
    addresses = [
        frame["address"]
        for frame in report["frames"]
        if executable_module is not None and frame["module"] == executable_module
    ]
    symbols = {}
    symbolication_error = None
    if addresses:
        try:
            result = subprocess.run(
                [
                    "atos", "-o", str(image), "-arch", "arm64",
                    "-l", hex(executable_module["base"]),
                    *[hex(address) for address in addresses],
                ],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
            descriptions = result.stdout.splitlines()
            if len(descriptions) != len(addresses):
                raise RuntimeError(
                    "atos returned %d descriptions for %d addresses"
                    % (len(descriptions), len(addresses)))
            symbols = dict(zip(addresses, descriptions))
        except (OSError, subprocess.CalledProcessError,
                subprocess.TimeoutExpired, RuntimeError) as error:
            symbolication_error = error

    for index, frame in enumerate(report["frames"]):
        owner = frame["module"]
        address = frame["address"]
        if executable_module is not None and owner == executable_module:
            description = symbols.get(address, "").strip()
            if not description:
                description = "%s+%#x" % (
                    owner["name"], address - owner["base"])
                if symbolication_error:
                    description += " (atos unavailable: %s)" % symbolication_error
        elif owner:
            description = "%s+%#x" % (owner["name"], address - owner["base"])
        else:
            description = "<unknown module>"
        label = "faulting frame" if index == 0 else f"frame {index}"
        print("%s: %#x %s" % (label, address, description))


def test_symbolicate_uses_executable(report):
    with tempfile.TemporaryDirectory() as directory:
        executable = Path(directory) / "Teagram"
        executable.write_text("fixture")
        calls = []
        original_run = subprocess.run

        def fake_run(args, **kwargs):
            calls.append((args, kwargs))
            return subprocess.CompletedProcess(
                args, 0,
                stdout="Teagram::test_fault()\nTeagram::test_caller()\n",
                stderr="")

        output = io.StringIO()
        subprocess.run = fake_run
        try:
            with contextlib.redirect_stdout(output):
                symbolicate(report, executable)
        finally:
            subprocess.run = original_run

        assert len(calls) == 1
        args, kwargs = calls[0]
        assert args[:7] == [
            "atos", "-o", str(executable), "-arch", "arm64", "-l",
            hex(report["module"]["base"]),
        ]
        assert args[7:] == [
            hex(frame["address"])
            for frame in report["frames"]
            if frame["module"]
            and frame["module"]["base"] == report["module"]["base"]
        ]
        assert kwargs["timeout"] == 10
        assert "Teagram::test_fault()" in output.getvalue()
        assert "Teagram::test_caller()" in output.getvalue()


def build_fixture(mixed_modules=False):
    stream_count = 3
    data = bytearray(32 + stream_count * 12)
    context = bytearray(912)
    module_base = 0x100000000
    system_base = 0x190000000
    stack_start = 0x700000
    frame_pointer = stack_start + 16
    program_counter = (system_base + 0x3456 if mixed_modules
                       else module_base + 0x1234)
    struct.pack_into("<I", context, 0, 0x400007)
    struct.pack_into("<Q", context, 8 + 29 * 8, frame_pointer)
    struct.pack_into("<Q", context, 8 + 32 * 8, program_counter)
    stack = bytearray(64 if mixed_modules else 32)
    if mixed_modules:
        struct.pack_into("<QQ", stack, 16, stack_start + 32, module_base + 0x2345)
        struct.pack_into("<QQ", stack, 32, stack_start + 48, system_base + 0x4569)
        struct.pack_into("<QQ", stack, 48, 0, module_base + 0x3459)
    else:
        struct.pack_into("<QQ", stack, 16, 0, module_base + 0x2345)

    exception = bytearray(EXCEPTION_STREAM_SIZE)
    struct.pack_into("<I", exception, 0, 42)
    struct.pack_into("<IIQQI", exception, 8, 1, 1, 0, 0xDEAD, 0)

    thread = bytearray(4 + THREAD_ENTRY_SIZE)
    struct.pack_into("<I", thread, 0, 1)
    struct.pack_into("<I", thread, 4, 42)

    modules = (
        [(system_base, "/System/Library/CoreText"),
         (module_base, "/Build/Teagram.app/Contents/MacOS/Teagram")]
        if mixed_modules else [(module_base, "Teagram")]
    )
    module = bytearray(4 + len(modules) * MODULE_ENTRY_SIZE)
    struct.pack_into("<I", module, 0, len(modules))
    module_names = []
    for index, (base, name) in enumerate(modules):
        struct.pack_into("<QI", module, 4 + index * MODULE_ENTRY_SIZE, base, 0x10000)
        name_bytes = (name + "\0").encode("utf-16le")
        module_names.append(struct.pack("<I", len(name_bytes)) + name_bytes)

    streams = [
        (EXCEPTION, exception),
        (THREAD_LIST, thread),
        (MODULE_LIST, module),
        (0, stack),
        (0, context),
    ] + [(0, name) for name in module_names]
    rvas = []
    for _, content in streams:
        rvas.append(len(data))
        data.extend(content)
    struct.pack_into("<II", exception, 160, len(context), rvas[4])
    struct.pack_into("<QII", thread, 4 + 24, stack_start, len(stack), rvas[3])
    for index in range(len(modules)):
        struct.pack_into("<II", module, 4 + index * MODULE_ENTRY_SIZE + 20,
                         rvas[5 + index], 0)
    # Replace the stream placeholders now that their final bytes are known.
    for (_, content), rva in zip(streams, rvas):
        data[rva:rva + len(content)] = content
    struct.pack_into("<IIIIIIQ", data, 0, 0x504D444D, 0xA793, stream_count, 32, 0, 0, 0)
    for index, stream_type in enumerate((EXCEPTION, THREAD_LIST, MODULE_LIST)):
        content = streams[index][1]
        struct.pack_into("<III", data, 32 + index * 12, stream_type, len(content), rvas[index])
    return bytes(data)


def test_symbolicate_system_fault_with_teagram_callers():
    report = parse_minidump(build_fixture(mixed_modules=True))
    assert report["module"]["name"] == "/System/Library/CoreText"
    assert [frame["address"] for frame in report["frames"]] == [
        0x190003456, 0x100002341, 0x190004565, 0x100003455,
    ]
    with tempfile.TemporaryDirectory() as directory:
        executable = Path(directory) / "Teagram"
        executable.write_text("fixture")
        calls = []
        original_run = subprocess.run

        def fake_run(args, **kwargs):
            calls.append((args, kwargs))
            return subprocess.CompletedProcess(
                args, 0,
                stdout="Teagram::first_caller()\nTeagram::second_caller()\n",
                stderr="")

        output = io.StringIO()
        subprocess.run = fake_run
        try:
            with contextlib.redirect_stdout(output):
                symbolicate(report, executable)
        finally:
            subprocess.run = original_run

        assert len(calls) == 1
        args, kwargs = calls[0]
        assert args == [
            "atos", "-o", str(executable), "-arch", "arm64", "-l",
            "0x100000000", "0x100002341", "0x100003455",
        ]
        assert kwargs["timeout"] == 10
        text = output.getvalue()
        assert "faulting frame: 0x190003456 /System/Library/CoreText+0x3456" in text
        assert "frame 1: 0x100002341 Teagram::first_caller()" in text
        assert "frame 2: 0x190004565 /System/Library/CoreText+0x4565" in text
        assert "frame 3: 0x100003455 Teagram::second_caller()" in text


def test_symbolicate_unknown_or_ambiguous_executable():
    for reason in ("not found", "ambiguous"):
        report = parse_minidump(build_fixture(mixed_modules=True))
        if reason == "ambiguous":
            report["modules"].append({
                "base": 0x200000000,
                "size": 0x10000,
                "name": "/Another/Teagram",
            })
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / (
                "Missing" if reason == "not found" else "Teagram")
            executable.write_text("fixture")
            original_run = subprocess.run

            def unexpected_run(*args, **kwargs):
                raise AssertionError("atos must not run without a unique executable module")

            output = io.StringIO()
            subprocess.run = unexpected_run
            try:
                with contextlib.redirect_stdout(output):
                    symbolicate(report, executable)
            finally:
                subprocess.run = original_run
            text = output.getvalue()
            assert "Executable module %s in minidump: %s" % (
                reason, executable.name) in text
            assert "faulting frame: 0x190003456 /System/Library/CoreText+0x3456" in text
            assert "frame 1: 0x100002341 /Build/Teagram.app/Contents/MacOS/Teagram+0x2341" in text


def test_symbolicate_exact_path_with_duplicate_basename():
    report = parse_minidump(build_fixture(mixed_modules=True))
    with tempfile.TemporaryDirectory() as directory:
        executable = Path(directory) / "Teagram"
        executable.write_text("fixture")
        report["modules"][1]["name"] = str(executable)
        report["modules"].append({
            "base": 0x200000000,
            "size": 0x10000,
            "name": "/Another/Teagram",
        })
        calls = []
        original_run = subprocess.run

        def fake_run(args, **kwargs):
            calls.append(args)
            return subprocess.CompletedProcess(
                args, 0, stdout="first()\nsecond()\n", stderr="")

        output = io.StringIO()
        subprocess.run = fake_run
        try:
            with contextlib.redirect_stdout(output):
                symbolicate(report, executable)
        finally:
            subprocess.run = original_run
        assert len(calls) == 1
        assert calls[0][6:] == ["0x100000000", "0x100002341", "0x100003455"]
        assert "frame 1: 0x100002341 first()" in output.getvalue()


def self_test():
    report = parse_minidump(build_fixture())
    assert report["thread_id"] == 42
    assert report["exception_code"] == 1
    assert report["pc"] == 0x100001234
    assert report["module"]["name"] == "Teagram"
    assert [frame["address"] for frame in report["frames"]] == [
        0x100001234,
        0x100002341,
    ]
    test_symbolicate_uses_executable(report)
    test_symbolicate_system_fault_with_teagram_callers()
    test_symbolicate_unknown_or_ambiguous_executable()
    test_symbolicate_exact_path_with_duplicate_basename()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("dump", nargs="?")
    parser.add_argument("executable", nargs="?")
    args = parser.parse_args()
    try:
        if args.self_test:
            self_test()
            print("macOS minidump symbolizer self-test: PASS")
            return 0
        if not args.dump:
            parser.error("dump path is required")
        report = parse_minidump(Path(args.dump).read_bytes())
        if args.json:
            print(json.dumps(report, indent=2))
        elif args.executable:
            symbolicate(report, args.executable)
        else:
            parser.error("Teagram executable path is required unless --json is set")
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
