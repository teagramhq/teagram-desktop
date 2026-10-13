#!/usr/bin/env python3
import argparse
import hashlib
import json
import platform
import re
import shlex
import shutil
import struct
import subprocess
import sys
from pathlib import Path


MAX_OUTPUT_BYTES = 1024 * 1024
MAX_ITEMS = 500
MAX_TOOL_OUTPUT = 256 * 1024
SECRET_ASSIGNMENT = re.compile(
    r'(?i)([A-Z0-9_]*(?:API[_-]?(?:HASH|ID|KEY)|TOKEN|PASSWORD|SECRET|CREDENTIAL|PRIVATE[_-]?KEY)[A-Z0-9_]*)'
    r'\s*(=|:)\s*("[^"]*"|\'[^\']*\'|[^;\s,]+)'
)
CMAKE_CACHE_KEYS = {
    "CMAKE_GENERATOR",
    "CMAKE_CXX_COMPILER",
    "CMAKE_CXX_COMPILER_ARG1",
    "CMAKE_CXX_COMPILER_ID",
    "CMAKE_CXX_COMPILER_VERSION",
    "Qt6Core_DIR",
    "Qt6Gui_DIR",
    "Qt6Widgets_DIR",
}


def redact(value):
    if isinstance(value, str):
        return SECRET_ASSIGNMENT.sub(r"\1\2<redacted>", value)
    if isinstance(value, list):
        return [redact(item) for item in value]
    if isinstance(value, dict):
        return {key: redact(item) for key, item in value.items()}
    return value


def mark_incomplete(report, reason):
    report["status"] = "incomplete"
    report.setdefault("capture_errors", []).append(reason[:500])


def bounded_items(values, report, section):
    values = list(values)
    if len(values) > MAX_ITEMS:
        report.setdefault("truncated_sections", []).append(section)
        mark_incomplete(report, f"{section} exceeded the {MAX_ITEMS}-item capture limit")
    return values[:MAX_ITEMS]


def write_report(path, report):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = (json.dumps(report, indent=2, sort_keys=True) + "\n").encode()
    if len(payload) > MAX_OUTPUT_BYTES:
        payload = (json.dumps({
            "schema_version": 1,
            "status": "incomplete",
            "source_sha": report.get("source", {}).get("observed_sha"),
            "image": report.get("image", {}),
            "failure": "metadata exceeded the 1 MiB output limit",
            "attempted_bytes": len(payload),
            "retained_bytes": 0,
        }, indent=2, sort_keys=True) + "\n").encode()
        if len(payload) > MAX_OUTPUT_BYTES:
            raise RuntimeError("bounded metadata failure record exceeded output limit")
    path.write_bytes(payload)
    return json.loads(payload).get("status")


def run_command(args, timeout=15, max_output=MAX_TOOL_OUTPUT):
    if not shutil.which(args[0]):
        return {"status": "absent", "reason": f"{args[0]} is not installed"}
    try:
        result = subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return {"status": "unresolved", "reason": f"{args[0]} exceeded {timeout}s"}
    except OSError as error:
        return {"status": "unresolved", "reason": f"{args[0]} failed: {error}"[:500]}
    output = result.stdout.strip()
    encoded_size = len(output.encode())
    if encoded_size > max_output:
        return {
            "status": "unresolved",
            "reason": f"{args[0]} output exceeded {max_output} bytes",
            "observed_bytes": encoded_size,
        }
    if result.returncode:
        return {
            "status": "unresolved",
            "exit_code": result.returncode,
            "reason": result.stderr.strip()[:500],
            "output": output[:2000],
        }
    return {"status": "observed", "output": output[:max_output]}


def parse_cache(path):
    values = {}
    try:
        for line in Path(path).read_text(errors="replace").splitlines():
            if line.startswith("//") or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            key = key.split(":", 1)[0]
            if key in CMAKE_CACHE_KEYS:
                values[key] = value
    except OSError:
        return {}
    return values


def qt_package_version(config_dir):
    if not config_dir:
        return None
    config_dir = Path(config_dir)
    for filename in (
        "Qt6CoreConfigVersionImpl.cmake",
        "Qt6CoreConfigVersion.cmake",
        "Qt6CoreConfig.cmake",
    ):
        path = config_dir / filename
        try:
            content = path.read_text(errors="replace")
        except OSError:
            continue
        for variable in ("PACKAGE_VERSION", "Qt6Core_VERSION", "Qt6Core_VERSION_STRING"):
            match = re.search(
                rf'(?m)^\s*set\s*\(\s*{variable}\s+"([^"]+)"',
                content,
            )
            if match:
                return match.group(1)
    return None


def configured_compiler_details(build):
    root = Path(build) / "CMakeFiles"
    for path in sorted(root.glob("*/CMakeCXXCompiler.cmake"), reverse=True):
        try:
            content = path.read_text(errors="replace")
        except OSError:
            continue
        details = {}
        for name, variable in (
            ("id", "CMAKE_CXX_COMPILER_ID"),
            ("version", "CMAKE_CXX_COMPILER_VERSION"),
            ("arg1", "CMAKE_CXX_COMPILER_ARG1"),
        ):
            match = re.search(
                rf'(?m)^\s*set\s*\(\s*{variable}\s+"([^"]*)"\s*\)',
                content,
            )
            if match:
                details[name] = match.group(1)
        if details:
            return details
    return {}


def read_os_release(path=Path("/etc/os-release")):
    values = {}
    allowed = {"ID", "NAME", "VERSION_ID", "PRETTY_NAME"}
    try:
        for line in path.read_text(errors="replace").splitlines():
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            if key in allowed:
                values[key] = value.strip().strip('"')[:200]
    except OSError:
        return {"status": "unresolved"}
    return {"status": "observed", "values": values}


def elf_identity(path):
    try:
        with Path(path).open("rb") as stream:
            header = stream.read(20)
    except OSError as error:
        return {"status": "unresolved", "reason": str(error)[:300]}
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return {"status": "absent", "reason": "file is not an ELF object"}
    byte_order = "little" if header[5] == 1 else "big" if header[5] == 2 else None
    if byte_order is None:
        return {"status": "unresolved", "reason": "unknown ELF byte order"}
    machine = struct.unpack_from("<H" if byte_order == "little" else ">H", header, 18)[0]
    machines = {
        3: "x86",
        40: "arm",
        62: "x86_64",
        183: "aarch64",
        243: "riscv",
    }
    return {
        "status": "observed",
        "class": {1: "ELF32", 2: "ELF64"}.get(header[4], "unknown"),
        "byte_order": byte_order,
        "machine_id": machine,
        "architecture": machines.get(machine, "unknown"),
    }


def sha256(path):
    digest = hashlib.sha256()
    try:
        with Path(path).open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest()


def cmake_target(build_dir, report):
    reply_dir = Path(build_dir) / ".cmake/api/v1/reply"
    indexes = sorted(reply_dir.glob("index-*.json"), key=lambda path: path.stat().st_mtime)
    if not indexes:
        mark_incomplete(report, "CMake File API codemodel reply is missing")
        return None
    try:
        index = json.loads(indexes[-1].read_text())
        codemodel_ref = index["reply"]["codemodel-v2"]["jsonFile"]
        codemodel = json.loads((reply_dir / codemodel_ref).read_text())
        configurations = codemodel.get("configurations", [])
        configuration = next(
            (item for item in configurations if item.get("name") == "Debug"),
            configurations[0] if configurations else {},
        )
        target_ref = next(
            item for item in configuration.get("targets", [])
            if item.get("name") == "test_unit"
        )
        target = json.loads((reply_dir / target_ref["jsonFile"]).read_text())
    except (KeyError, IndexError, StopIteration, OSError, json.JSONDecodeError) as error:
        mark_incomplete(report, f"CMake File API could not resolve test_unit: {error}")
        return None

    compile_groups = []
    for index, group in enumerate(target.get("compileGroups", [])):
        compile_groups.append({
            "language": group.get("language"),
            "standard": group.get("languageStandard"),
            "includes": bounded_items(group.get("includes", []), report,
                                      f"compile_groups[{index}].includes"),
            "defines": bounded_items(group.get("defines", []), report,
                                     f"compile_groups[{index}].defines"),
            "compile_fragments": bounded_items(group.get("compileCommandFragments", []), report,
                                                f"compile_groups[{index}].compile_fragments"),
        })
    link = target.get("link", {})
    sources = [item.get("path", "") for item in target.get("sources", [])]
    link_fragments = bounded_items(link.get("commandFragments", []), report, "link.command_fragments")
    return redact({
        "source": "CMake File API codemodel-v2 for the configured test_unit consumer",
        "configuration": configuration.get("name"),
        "target": target.get("name"),
        "artifacts": target.get("artifacts", []),
        "compile_groups": compile_groups,
        "link": {
            "language": link.get("language"),
            "fragments": link_fragments,
            "dependencies": bounded_items(target.get("dependencies", []), report,
                                           "target.dependencies"),
        },
        "resource_sources": sorted({path for path in sources if path.lower().endswith(".qrc")}),
        "plugin_related_link_inputs": sorted({
            token
            for fragment in link_fragments
            for token in re.findall(r"[^\s]+", fragment.get("fragment", ""))
            if re.search(r"(offscreen|minimal|plugin.import|qrc_)", token, re.IGNORECASE)
        }),
    })


def qt_prefixes(cache):
    prefixes = set()
    for key in ("Qt6Core_DIR", "Qt6Gui_DIR", "Qt6Widgets_DIR"):
        value = cache.get(key)
        if value:
            path = Path(value)
            for prefix in path.parents:
                if prefix.name in ("lib", "lib64"):
                    prefixes.add(prefix.parent)
                    break
    return sorted(prefixes)


def plugin_archive(name, prefixes, link_text, report):
    candidates = []
    searched = []
    for prefix in prefixes:
        for directory in (
            prefix / "plugins/platforms",
            prefix / "lib/plugins/platforms",
            prefix / "lib64/plugins/platforms",
            prefix / "lib/qt6/plugins/platforms",
            prefix / "lib64/qt6/plugins/platforms",
        ):
            for filename in (f"libq{name}.a", f"q{name}.a"):
                path = directory / filename
                searched.append(str(path))
                if path.is_file():
                    candidates.append(path)
    if not prefixes:
        return {
            "observed_status": "unresolved",
            "reason": "Qt install prefix was not available from CMakeCache.txt",
            "searched_paths": [],
        }
    archive = next((path for path in candidates if path.suffix == ".a"), None)
    if archive is None:
        shared = []
        for prefix in prefixes:
            for directory in (prefix / "plugins/platforms", prefix / "lib/plugins/platforms"):
                shared.extend(directory.glob(f"libq{name}.so"))
        return {
            "observed_status": "present" if shared else "absent",
            "static_archive_status": "absent",
            "shared_plugin_status": "present" if shared else "absent",
            "shared_plugin_paths": [str(path) for path in shared],
            "searched_paths": searched,
            "linked_to_test_unit": any(str(path) in link_text for path in shared),
        }

    members_result = run_command(["ar", "t", str(archive)])
    members = members_result.get("output", "").splitlines()
    registration_members = [
        member for member in members
        if re.search(name, member, re.IGNORECASE)
    ]
    nm_result = run_command(["nm", "-A", "--defined-only", str(archive)])
    if members_result.get("status") != "observed":
        mark_incomplete(report, f"{name} archive members could not be inspected")
    if nm_result.get("status") != "observed":
        mark_incomplete(report, f"{name} archive registration symbols could not be inspected")
    registration_symbols = [
        line[:500] for line in nm_result.get("output", "").splitlines()
        if re.search(r"qt_static_plugin|static_plugin|qoffscreen|qminimal", line, re.IGNORECASE)
    ][:MAX_ITEMS]
    if len(registration_symbols) == MAX_ITEMS:
        mark_incomplete(report, f"{name} archive registration symbol list reached capture limit")
    symbol_members = []
    for line in registration_symbols:
        match = re.search(r"\.a(?:\[|:)([^:\]]+)(?:\]|:)", line)
        if match:
            symbol_members.append(match.group(1))
    registration_member_names = sorted(set(registration_members + symbol_members))
    registration_objects = []
    if members_result.get("status") == "observed":
        for member in registration_member_names[:MAX_ITEMS]:
            if member.startswith("__.") or member.startswith("/"):
                continue
            try:
                result = subprocess.run(
                    ["ar", "p", str(archive), member],
                    capture_output=True,
                    timeout=10,
                    check=False,
                )
            except (OSError, subprocess.TimeoutExpired):
                mark_incomplete(report, f"{name} registration object {member} could not be extracted")
                continue
            if result.returncode == 0:
                data = result.stdout
                if len(data) >= 20 and data[:4] == b"\x7fELF":
                    byte_order = "little" if data[5] == 1 else "big" if data[5] == 2 else None
                    if byte_order:
                        machine = struct.unpack_from("<H" if byte_order == "little" else ">H", data, 18)[0]
                        registration_objects.append({
                            "member": member,
                            "linkage": "object within static archive",
                            "architecture": elf_identity_from_bytes(data, machine, byte_order),
                        })
                else:
                    mark_incomplete(report, f"{name} registration object {member} has no ELF architecture")
                    registration_objects.append({
                        "member": member,
                        "linkage": "object within static archive",
                        "architecture": {"status": "unresolved", "reason": "member is not an ELF object"},
                    })
            else:
                mark_incomplete(report, f"{name} registration object {member} could not be read from its archive")
    return {
        "observed_status": "present",
        "static_archive_status": "present",
        "linkage": "static archive",
        "path": str(archive),
        "size_bytes": archive.stat().st_size,
        "sha256": sha256(archive),
        "file_type": run_command(["file", "-b", str(archive)]),
        "linked_to_test_unit": str(archive) in link_text or archive.name in link_text,
        "members": bounded_items(members, report, f"{name}.archive_members"),
        "registration_objects": bounded_items(registration_objects, report,
                                              f"{name}.registration_objects"),
        "registration_symbols": bounded_items(registration_symbols, report,
                                              f"{name}.registration_symbols"),
        "registration_status": (
            "unresolved"
            if nm_result.get("status") != "observed" or members_result.get("status") != "observed"
            else "present" if registration_members or registration_symbols else "absent"
        ),
        "inspection_status": {
            "ar": members_result.get("status"),
            "nm": nm_result.get("status"),
        },
    }


def elf_identity_from_bytes(data, machine, byte_order):
    return {
        "class": {1: "ELF32", 2: "ELF64"}.get(data[4], "unknown"),
        "byte_order": byte_order,
        "machine_id": machine,
        "architecture": {
            3: "x86",
            40: "arm",
            62: "x86_64",
            183: "aarch64",
            243: "riscv",
        }.get(machine, "unknown"),
    }


def runtime_libraries(binary):
    result = run_command(["ldd", str(binary)], timeout=15)
    output = result.get("output", "")
    if "not a dynamic executable" in output or "statically linked" in output:
        return {"status": "observed", "linkage": "static", "dependencies": []}
    if result.get("status") != "observed":
        return result
    dependencies = []
    for line in output.splitlines():
        line = line.strip()
        if not line:
            continue
        if "=>" in line:
            name, path = (part.strip() for part in line.split("=>", 1))
            dependencies.append({
                "name": name,
                "path": None if path == "not found" else path.split(" ", 1)[0],
                "status": "missing" if path == "not found" else "resolved",
            })
        else:
            dependencies.append({"name": line.split(" ", 1)[0], "status": "resolved"})
    return {"status": "observed", "linkage": "dynamic", "dependencies": dependencies}


def source_expectations(source):
    dockerfile_path = Path(source) / "Telegram/build/docker/centos_env/Dockerfile"
    tests_path = Path(source) / "Telegram/cmake/tests_unit.cmake"
    dockerfile = dockerfile_path.read_text(errors="replace") if dockerfile_path.is_file() else ""
    tests = tests_path.read_text(errors="replace") if tests_path.is_file() else ""
    qpa = re.search(r'-DQT_QPA_PLATFORMS="([^"]*)"', dockerfile)
    qrc_files = sorted(set(re.findall(r"([A-Za-z0-9_./-]+\.qrc)", tests)))
    return {
        "basis": "read-only source recipe observations; not runtime observations",
        "qt_build_shared_libraries_setting": "OFF" if "-DBUILD_SHARED_LIBS=OFF" in dockerfile else "not found",
        "configured_qpa_plugins": qpa.group(1).split(";") if qpa else [],
        "unit_resource_inputs": qrc_files,
    }


def font_observation():
    if not shutil.which("fc-match"):
        return {"status": "absent", "reason": "fontconfig fc-match is not installed"}
    result = run_command(["fc-match", "-f", "%{family}|%{file}\\n", "sans"])
    if result.get("status") != "observed":
        return result
    family, separator, font_path = result.get("output", "").partition("|")
    if not separator:
        return {"status": "unresolved", "reason": "fc-match did not return family and file"}
    if not font_path:
        return {"status": "absent", "query": "sans", "reason": "fontconfig returned no font file"}
    return {
        "status": "observed",
        "query": "sans",
        "family": family[:200],
        "resolved_file": font_path,
        "file_present": Path(font_path).is_file(),
        "execution": "availability query only; no font was loaded by QApplication",
    }


def collect(args):
    source = Path(args.source).resolve()
    build = Path(args.build).resolve()
    binary = Path(args.test_unit).resolve()
    report = {
        "schema_version": 1,
        "status": "complete",
        "source": {},
        "image": {},
        "toolchain": {},
        "evaluated_exported_target_usage_requirements": {},
        "installed_platform_plugins": {},
        "runtime_inputs": {},
        "source_expectations": source_expectations(source),
        "scope": {
            "selected_cases": [],
            "metadata_collector_launched_test_unit": False,
            "gui_claims": [],
            "limitations": [
                "This inventory does not claim QApplication or GUI bindings executed.",
                "It does not claim font loading or UsernameInput behavior executed.",
            ],
        },
        "capture_errors": [],
    }
    try:
        observed_sha = subprocess.run(
            ["git", "-C", str(source), "rev-parse", "--verify", "HEAD"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        observed_sha = observed_sha.stdout.strip() if observed_sha.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        observed_sha = None
    report["source"] = {
        "expected_sha": args.tested_sha,
        "observed_sha": observed_sha,
        "matches_expected": bool(observed_sha and observed_sha.lower() == args.tested_sha.lower()),
    }
    if not report["source"]["matches_expected"]:
        mark_incomplete(report, "checked out source SHA does not match the recorded tested SHA")

    report["scope"]["selected_cases"] = [
        line.strip()
        for line in Path(args.cases).read_text(errors="replace").splitlines()
        if line.strip()
    ] if Path(args.cases).is_file() else []
    report["image"] = {
        "tag": args.image_tag,
        "id": args.image_id,
        "architecture": platform.machine(),
        "operating_system": read_os_release(),
        "generated_dockerfile_sha256": sha256(
            source / "Telegram/build/docker/centos_env/Dockerfile"
        ),
    }
    if not args.image_id:
        mark_incomplete(report, "dependency image ID was not observed after Buildx load")
    if report["image"]["operating_system"].get("status") != "observed":
        mark_incomplete(report, "dependency image operating system identity is unresolved")

    cache = parse_cache(build / "CMakeCache.txt")
    compiler = cache.get("CMAKE_CXX_COMPILER")
    compiler_details = configured_compiler_details(build)
    compiler_id = cache.get("CMAKE_CXX_COMPILER_ID") or compiler_details.get("id")
    compiler_version_number = (
        cache.get("CMAKE_CXX_COMPILER_VERSION") or compiler_details.get("version")
    )
    compiler_arg1 = (
        cache.get("CMAKE_CXX_COMPILER_ARG1") or compiler_details.get("arg1") or ""
    )
    compiler_arguments = shlex.split(compiler_arg1)
    compiler_command = [compiler, *compiler_arguments] if compiler else []
    compiler_version = run_command([*compiler_command, "--version"]) if compiler else {
        "status": "unresolved",
        "reason": "CMAKE_CXX_COMPILER is missing from CMakeCache.txt",
    }
    cmake_version = run_command(["cmake", "--version"])
    qt_core_dir = cache.get("Qt6Core_DIR")
    qt_version = qt_package_version(qt_core_dir)
    report["toolchain"] = {
        "cmake_generator": cache.get("CMAKE_GENERATOR"),
        "cmake_version": cmake_version,
        "cxx_compiler": compiler,
        "cxx_compiler_arguments": compiler_arguments,
        "cxx_compiler_invocation": compiler_command,
        "cxx_compiler_id": compiler_id,
        "cxx_compiler_version": compiler_version_number,
        "cxx_compiler_version_output": compiler_version,
        "qt6_core_config_dir": qt_core_dir,
        "qt_version": qt_version,
    }
    if (
        not cache.get("CMAKE_GENERATOR")
        or not compiler
        or not compiler_id
        or not compiler_version_number
        or not qt_core_dir
        or not qt_version
    ):
        mark_incomplete(report, "configured compiler, generator, or Qt package version is unresolved")
    if compiler_version.get("status") != "observed" or cmake_version.get("status") != "observed":
        mark_incomplete(report, "configured compiler or CMake version could not be observed")

    usage = cmake_target(build, report)
    if usage is None:
        mark_incomplete(report, "evaluated CMake test_unit usage requirements are unavailable")
    else:
        report["evaluated_exported_target_usage_requirements"] = usage
    if not binary.is_file():
        mark_incomplete(report, "configured test_unit executable is missing")
        binary_info = {"status": "absent", "path": str(binary)}
        link_text = ""
    else:
        binary_info = {
            "status": "observed",
            "path": str(binary),
            "size_bytes": binary.stat().st_size,
            "sha256": sha256(binary),
            "file_type": run_command(["file", "-b", str(binary)]),
            "elf": elf_identity(binary),
        }
        link_text = json.dumps(usage.get("link", {})) if usage else ""
    report["runtime_inputs"]["test_unit_binary"] = binary_info
    report["runtime_inputs"]["dynamic_libraries"] = runtime_libraries(binary) if binary.is_file() else {
        "status": "unresolved",
        "reason": "test_unit executable is unavailable",
    }
    if report["runtime_inputs"]["dynamic_libraries"].get("status") != "observed":
        mark_incomplete(report, "test_unit runtime libraries could not be resolved")

    prefixes = qt_prefixes(cache)
    report["installed_platform_plugins"] = {
        name: plugin_archive(name, prefixes, link_text, report)
        for name in ("offscreen", "minimal")
    }
    if any(item.get("observed_status") == "unresolved"
           for item in report["installed_platform_plugins"].values()):
        mark_incomplete(report, "Qt platform plugin install paths could not be resolved")
    font = font_observation()
    report["runtime_inputs"]["fontconfig_default_match"] = font
    if font.get("status") == "unresolved":
        mark_incomplete(report, "fontconfig runtime input could not be resolved")
    report["runtime_inputs"]["platform_selection"] = {
        "observed_plugins": {
            name: result.get("observed_status")
            for name, result in report["installed_platform_plugins"].items()
        },
        "observed_static_archives": {
            name: result.get("static_archive_status", "unresolved")
            for name, result in report["installed_platform_plugins"].items()
        },
        "source_configured_plugins": report["source_expectations"]["configured_qpa_plugins"],
        "platform_plugin_loaded": False,
        "reason": "The metadata collector inspects installed and linked inputs without starting QApplication.",
    }
    report["runtime_inputs"]["resources"] = {
        "configured_target_qrc_sources": usage.get("resource_sources", []) if usage else [],
        "source_recipe_qrc_inputs": report["source_expectations"]["unit_resource_inputs"],
        "test_unit_rcc_sidecar": {
            "status": "present" if binary.with_name("test_unit.rcc").is_file() else "absent",
            "path": str(binary.with_name("test_unit.rcc")),
        },
        "qrc_link_inputs": [
            path for path in usage.get("plugin_related_link_inputs", [])
            if "qrc_" in path.lower()
        ] if usage else [],
        "resource_execution": "inventory only; no resource-dependent GUI behavior was executed",
    }
    if args.image_id is None or not args.tested_sha:
        mark_incomplete(report, "required image or tested SHA identity was not supplied")
    report["capture_errors"] = list(dict.fromkeys(report["capture_errors"]))
    return report


def record_failure(args):
    path = Path(args.output)
    try:
        report = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        report = {"schema_version": 1, "status": "failed"}
    if report.get("status") == "complete":
        report["status"] = "failed"
    elif report.get("status") not in ("incomplete", "failed"):
        report["status"] = "failed"
    if args.tested_sha:
        report.setdefault("source", {}).setdefault("expected_sha", args.tested_sha)
    if args.image_tag or args.image_id:
        report.setdefault("image", {}).update({
            "tag": args.image_tag,
            "id": args.image_id,
        })
    report.setdefault("capture_errors", []).append(
        f"metadata collector exited with status {args.exit_code}"
    )
    write_report(path, report)
    return 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--source")
    parser.add_argument("--build")
    parser.add_argument("--test-unit")
    parser.add_argument("--cases")
    parser.add_argument("--tested-sha")
    parser.add_argument("--image-tag")
    parser.add_argument("--image-id")
    parser.add_argument("--mark-failure", action="store_true")
    parser.add_argument("--exit-code", type=int, default=1)
    args = parser.parse_args()
    if args.mark_failure:
        return record_failure(args)
    if not all((args.source, args.build, args.test_unit, args.cases, args.tested_sha, args.image_tag)):
        parser.error("source, build, test-unit, cases, tested-sha and image-tag are required")
    try:
        report = collect(args)
        status = write_report(args.output, report)
        return 0 if status == "complete" else 1
    except Exception as error:
        report = {
            "schema_version": 1,
            "status": "incomplete",
            "failure": f"metadata collector failed: {type(error).__name__}: {error}"[:500],
            "source": {"expected_sha": args.tested_sha},
            "image": {"tag": args.image_tag, "id": args.image_id},
        }
        write_report(args.output, report)
        return 1


if __name__ == "__main__":
    sys.exit(main())
