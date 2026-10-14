#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import tempfile


workflow = Path(os.environ.get(
    "UNIT_FEEDBACK_WORKFLOW", ".github/workflows/unit_feedback.yml"
)).read_text()
metadata_source = Path(__file__).with_name("unit_feedback_metadata.py").read_text()
metadata_namespace = {"__name__": "unit_feedback_metadata"}
exec(compile(metadata_source, "unit_feedback_metadata.py", "exec"), metadata_namespace)


def step_script(name):
    step = workflow.split(f"      - name: {name}\n", 1)[1]
    body = step.split("        run: |\n", 1)[1]
    lines = []
    for line in body.splitlines():
        if line and not line.startswith("          "):
            break
        lines.append(line[10:])
    return "\n".join(lines)


def run_feedback(selection, build_status="0", metadata_status="complete"):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        source = root / "source"
        runner = root / "runner"
        tools = root / "bin"
        inspector = runner / "unit_feedback_metadata.py"
        for path in (source / "Telegram", source / "out/Debug", runner, tools):
            path.mkdir(parents=True, exist_ok=True)
        trace = root / "trace.jsonl"
        inspector.write_text('''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
output = Path(args[args.index("--output") + 1])
if "--mark-failure" in args:
    try:
        report = json.loads(output.read_text())
    except (OSError, json.JSONDecodeError):
        report = {"schema_version": 1, "status": "failed"}
    if report.get("status") == "complete":
        report["status"] = "failed"
    report.setdefault("capture_errors", []).append("metadata collector exited")
    output.write_text(json.dumps(report))
    sys.exit(1)
status = os.environ["METADATA_STATUS"]
if status == "missing":
    sys.exit(1)
if status == "oversized":
    output.write_text("x" * (1024 * 1024 + 1))
    sys.exit(0)
output.write_text(json.dumps({"schema_version": 1, "status": status}))
sys.exit(0 if status == "complete" else 1)
''')
        mock = '''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["TRACE"], "a") as stream:
    stream.write(json.dumps([name, *args]) + "\\n")
if name == "docker":
    mount = os.environ["RUNNER_TEMP"] + "/unit-cases.txt:/tmp/unit-cases.txt:ro"
    assert mount in args, "The validated selector file must be mounted read-only."
    metadata_mount = os.environ["RUNNER_TEMP"] + "/unit-environment.json:/tmp/unit-environment.json"
    assert metadata_mount in args, "The environment metadata file must be retained."
    inspector_mount = os.environ["INSPECTOR"] + ":/tmp/unit_feedback_metadata.py:ro"
    assert inspector_mount in args, "The metadata inspector must be mounted read-only."
    script = args[-1].replace("/usr/src/tdesktop", os.environ["SOURCE"])
    script = script.replace("/tmp/unit-cases.txt", os.environ["RUNNER_TEMP"] + "/unit-cases.txt")
    script = script.replace("/tmp/unit_feedback_metadata.py", os.environ["INSPECTOR"])
    script = script.replace("/tmp/unit-environment.json", os.environ["RUNNER_TEMP"] + "/unit-environment.json")
    sys.exit(subprocess.run(["bash", "-c", script]).returncode)
if name == "cmake":
    sys.exit(int(os.environ["BUILD_STATUS"]))
if name == "test_unit":
    if not args:
        print("run  PassingCase\\nok   PassingCase\\nrun  AnotherCase\\nok   AnotherCase")
        print("2 case(s), 0 failed, 0 failed check(s).")
    elif args == ["--case", "FailingCase"]:
        print("run  FailingCase\\nFAIL FailingCase\\n1 case(s), 1 failed, 1 failed check(s).")
        sys.exit(1)
    elif args in (["--case", "PassingCase"], ["--case", "AnotherCase"]):
        print("run  " + args[1] + "\\nok   " + args[1])
        print("1 case(s), 0 failed, 0 failed check(s).")
    elif args == ["--case", "IgnoresSelector"]:
        print("2 case(s), 0 failed, 0 failed check(s).")
    elif args == ["--case", "FalseSuccess"]:
        print("1 case(s), 0 failed, 1 failed check(s).")
    elif args == ["--case", "NoSummary"]:
        sys.exit(0)
    elif args == ["--case", "ExitFailure"]:
        print("1 case(s), 0 failed, 0 failed check(s).")
        sys.exit(7)
    else:
        print("No test case named " + args[-1] + " is registered.")
        print("0 case(s), 0 failed, 0 failed check(s).")
        sys.exit(1)
'''
        for path in (source / "Telegram/configure.sh", source / "out/Debug/test_unit",
                     tools / "cmake", tools / "docker"):
            path.write_text(mock)
            path.chmod(0o755)
        environment = {
            **os.environ,
            "PATH": str(tools) + os.pathsep + os.environ["PATH"],
            "RUNNER_TEMP": str(runner),
            "GITHUB_STEP_SUMMARY": str(root / "summary.md"),
            "UNIT_CASES": selection,
            "TESTED_SHA": "a63240c199f4902ec2a867342c2c6627174e5045",
            "CONFIG": "Debug",
            "IMAGE_TAG": "tdesktop:centos_env",
            "IMAGE_ID": "sha256:mock-image-id",
            "SOURCE": str(source),
            "TRACE": str(trace),
            "INSPECTOR": str(inspector),
            "METADATA_STATUS": metadata_status,
            "BUILD_STATUS": build_status,
        }
        result = subprocess.run(
            ["bash", "-e", "-o", "pipefail", "-c", step_script("Validate case selection.")],
            cwd=source, env=environment, capture_output=True, text=True,
        )
        if result.returncode == 0:
            result = subprocess.run(
                ["bash", "-e", "-o", "pipefail", "-c", step_script("Unit tests.")],
                cwd=source, env=environment, capture_output=True, text=True,
            )
        calls = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
        summary = Path(environment["GITHUB_STEP_SUMMARY"])
        metadata = runner / "unit-environment.json"
        try:
            metadata_report = json.loads(metadata.read_text())
            retained_status = metadata_report.get("status", "invalid")
        except (OSError, json.JSONDecodeError):
            retained_status = "invalid"
        return (
            result.returncode,
            calls,
            summary.read_text() if summary.exists() else "",
            retained_status,
            metadata.stat().st_size if metadata.exists() else 0,
        )


assert "timeout --signal=KILL 300s" in workflow
assert "1048576" in workflow
assert "unit-environment.json" in workflow
redact = metadata_namespace["redact"]
assert "private-value" not in redact("TDESKTOP_API_HASH=private-value")
assert "123456" not in redact("TDESKTOP_API_ID=123456")
with tempfile.TemporaryDirectory() as directory:
    cache_path = Path(directory) / "CMakeCache.txt"
    cache_path.write_text(
        "CMAKE_GENERATOR:INTERNAL=Ninja\n"
        "Qt6Core_DIR:PATH=/usr/local/lib/cmake/Qt6Core\n"
        "TDESKTOP_API_HASH:STRING=private-value\n"
    )
    cache = metadata_namespace["parse_cache"](cache_path)
    assert "private-value" not in json.dumps(cache)
    assert cache == {
        "CMAKE_GENERATOR": "Ninja",
        "Qt6Core_DIR": "/usr/local/lib/cmake/Qt6Core",
    }
    qt_core_dir = Path(directory) / "Qt6Core"
    qt_core_dir.mkdir()
    (qt_core_dir / "Qt6CoreConfigVersion.cmake").write_text(
        'include("${CMAKE_CURRENT_LIST_DIR}/Qt6CoreConfigVersionImpl.cmake")\n'
    )
    (qt_core_dir / "Qt6CoreConfigVersionImpl.cmake").write_text(
        'set(PACKAGE_VERSION "6.11.1")\n'
    )
    assert metadata_namespace["qt_package_version"](qt_core_dir) == "6.11.1"

    build_path = Path(directory) / "build"
    compiler_file = build_path / "CMakeFiles/3.26.5/CMakeCXXCompiler.cmake"
    compiler_file.parent.mkdir(parents=True)
    compiler_file.write_text(
        'set(CMAKE_CXX_COMPILER_ID "GNU")\n'
        'set(CMAKE_CXX_COMPILER_VERSION "15.1.1")\n'
        'set(CMAKE_CXX_COMPILER_ARG1 "g++")\n'
    )
    assert metadata_namespace["configured_compiler_details"](build_path) == {
        "id": "GNU",
        "version": "15.1.1",
        "arg1": "g++",
    }

    metadata_path = Path(directory) / "metadata.json"
    status = metadata_namespace["write_report"](
        metadata_path,
        {"status": "complete", "large_value": "x" * (1024 * 1024)},
    )
    report = json.loads(metadata_path.read_text())
    assert status == "incomplete", "Oversized metadata must be marked incomplete."
    assert report["status"] == "incomplete"
    assert "1 MiB output limit" in report["failure"]
    assert metadata_path.stat().st_size <= 1024 * 1024

    secret_report = {
        "status": "complete",
        "toolchain": {
            "cxx_compiler_arguments": [
                "-DAPI_KEY=test-only-argument-value",
                "--token",
                "test-only-wrapper-value",
            ],
            "cxx_compiler_version_output": {
                "status": "observed",
                "output": "wrapper --auth-token test-only-version-value",
            },
        },
    }
    status = metadata_namespace["write_report"](metadata_path, secret_report)
    report_text = metadata_path.read_text()
    report = json.loads(report_text)
    assert status == "complete"
    assert report["status"] == "complete"
    for secret in (
        "test-only-argument-value",
        "test-only-wrapper-value",
        "test-only-version-value",
    ):
        assert secret not in report_text, "Compiler metadata must not expose credentials."
    assert report["toolchain"]["cxx_compiler_arguments"] == [
        "-DAPI_KEY=<redacted>", "--token", "<redacted>"
    ]
    assert report["toolchain"]["cxx_compiler_version_output"]["output"] == (
        "wrapper --auth-token <redacted>"
    )

    build_path = Path(directory) / "plugin-build"
    build_path.mkdir()
    cache = {
        "CMAKE_GENERATOR": "Ninja Multi-Config",
        "CMAKE_CONFIGURATION_TYPES": "Debug",
        "CMAKE_CXX_COMPILER": "/usr/bin/ccache",
        "CMAKE_CXX_COMPILER_ARG1": "g++",
        "Qt6Core_DIR": "/usr/local/lib/cmake/Qt6Core",
        "Qt6Gui_DIR": "/usr/local/lib/cmake/Qt6Gui",
    }
    elf_header = bytearray(20)
    elf_header[:4] = b"\x7fELF"
    elf_header[4] = 2
    elf_header[5] = 1
    elf_header[18:20] = (62).to_bytes(2, "little")

    def write_target(build, name, imported_target, plugin_archive):
        reply = build / ".cmake/api/v1/reply"
        reply.mkdir(parents=True, exist_ok=True)
        target_ref = f"{name}-Debug.json"
        target = {
            "name": name,
            "compileGroups": [{
                "language": "CXX",
                "includes": [{"path": "/usr/local/include/qt6", "isSystem": True}],
                "defines": [{"define": "QT_STATICPLUGIN"}],
                "compileCommandFragments": [{"fragment": "-fPIC"}],
            }],
            "link": {
                "language": "CXX",
                "commandFragments": [{"fragment": plugin_archive, "role": "libraries"}],
            },
            "dependencies": [{"id": imported_target, "target": imported_target}],
            "sources": [{"path": "main.cpp"}],
        }
        (reply / target_ref).write_text(json.dumps(target))
        codemodel_path = reply / "codemodel-v2.json"
        targets = []
        if codemodel_path.is_file():
            targets = json.loads(codemodel_path.read_text())["configurations"][0]["targets"]
        targets.append({"name": name, "jsonFile": target_ref})
        codemodel_path.write_text(json.dumps({
            "configurations": [{
                "name": "Debug",
                "targets": targets,
            }],
        }))
        (reply / "index-test.json").write_text(json.dumps({
            "reply": {"codemodel-v2": {"jsonFile": "codemodel-v2.json"}},
        }))

    original_run_command = metadata_namespace["run_command"]

    def fake_cmake(args, timeout=15, max_output=None):
        assert args[0] == "cmake"
        source_arg = Path(args[args.index("-S") + 1])
        build_arg = Path(args[args.index("-B") + 1])
        cmake_text = (source_arg / "CMakeLists.txt").read_text()
        assert "target_link_libraries(\"${consumer}\" PRIVATE \"${plugin_target}\")" in cmake_text
        assert "TARGET_GENEX_EVAL" in cmake_text
        assert "INTERFACE_LINK_LIBRARIES" in cmake_text
        assert "add_plugin_probe(offscreen Qt6::QOffscreenIntegrationPlugin Qt6::QOffscreenIntegrationPlugin_init)" in cmake_text
        assert "add_plugin_probe(minimal Qt6::QMinimalIntegrationPlugin Qt6::QMinimalIntegrationPlugin_init)" in cmake_text
        assert "test_unit" not in cmake_text, "Plugin usage must use isolated consumers."
        write_target(
            build_arg,
            "unit_feedback_plugin_offscreen",
            "Qt6::QOffscreenIntegrationPlugin",
            "/usr/local/lib/plugins/platforms/libqoffscreen.a",
        )
        write_target(
            build_arg,
            "unit_feedback_plugin_minimal",
            "Qt6::QMinimalIntegrationPlugin",
            "/usr/local/lib/plugins/platforms/libqminimal.a",
        )
        for plugin_name in ("offscreen", "minimal"):
            init_object = build_arg / f"{plugin_name}-init.o"
            init_object.write_bytes(elf_header)
            (build_arg / f"{plugin_name}-init-target.txt").write_text("OBJECT_LIBRARY")
            (build_arg / f"{plugin_name}-init-objects-Debug.txt").write_text(str(init_object))
            (build_arg / f"{plugin_name}-interface-Debug.txt").write_text(
                "INTERFACE_INCLUDE_DIRECTORIES=/usr/local/include/qt6|/usr/local/include\n"
                "INTERFACE_LINK_LIBRARIES=Qt6::Core|Qt6::GuiPrivate\n"
            )
        return {"status": "observed", "output": ""}

    metadata_namespace["run_command"] = fake_cmake
    try:
        plugin_targets = metadata_namespace["cmake_plugin_targets"](
            build_path, cache, {"status": "complete", "capture_errors": []}
        )
    finally:
        metadata_namespace["run_command"] = original_run_command

    for plugin_name, imported_target in (
        ("offscreen", "Qt6::QOffscreenIntegrationPlugin"),
        ("minimal", "Qt6::QMinimalIntegrationPlugin"),
    ):
        target_usage = plugin_targets[plugin_name]["usage"]
        assert target_usage["status"] == "observed"
        assert target_usage["target"] == imported_target
        assert target_usage["configuration"] == "Debug"
        assert target_usage["compile_groups"][0]["defines"] == [
            {"define": "QT_STATICPLUGIN"}
        ]
        assert target_usage["link"]["dependencies"][0]["target"] == imported_target
        assert target_usage["evaluated_interface"]["INTERFACE_LINK_LIBRARIES"] == [
            "Qt6::Core", "Qt6::GuiPrivate"
        ]
        import_objects = plugin_targets[plugin_name]["registration_objects"]
        assert import_objects["status"] == "present"
        assert import_objects["objects"][0]["linkage"] == "imported object-library object"
        assert import_objects["objects"][0]["architecture"]["architecture"] == "x86_64"

    plugin_prefix = Path(directory) / "qt-prefix"
    plugin_path = plugin_prefix / "lib/plugins/platforms/libqoffscreen.a"
    plugin_path.parent.mkdir(parents=True)
    plugin_path.write_bytes(b"archive")
    missing_build = Path(directory) / "missing-init-build"
    missing_build.mkdir()
    (missing_build / "offscreen-init-target.txt").write_text("OBJECT_LIBRARY")
    (missing_build / "offscreen-init-objects-Debug.txt").write_text(
        str(missing_build / "QOffscreenIntegrationPlugin_init.cpp.o")
    )
    missing_report = {
        "status": "complete",
        "capture_errors": [],
        "source_expectations": {"qt_build_shared_libraries_setting": "OFF"},
    }
    missing_import_objects = metadata_namespace["inspect_static_import_objects"](
        "offscreen",
        "Qt6::QOffscreenIntegrationPlugin",
        missing_build,
        "Debug",
        missing_report,
    )
    assert missing_import_objects["status"] == "absent"
    assert missing_report["status"] == "incomplete"
    original_run_command = metadata_namespace["run_command"]

    def fake_archive_tool(args, timeout=15, max_output=None):
        if args[0] in ("ar", "nm"):
            return {"status": "observed", "output": ""}
        if args[0] == "file":
            return {"status": "observed", "output": "current ar archive"}
        return original_run_command(args, timeout=timeout, max_output=max_output or 256 * 1024)

    metadata_namespace["run_command"] = fake_archive_tool
    try:
        plugin_report = metadata_namespace["plugin_archive"](
            "offscreen",
            [plugin_prefix],
            "",
            {"registration_objects": missing_import_objects},
            missing_report,
        )
    finally:
        metadata_namespace["run_command"] = original_run_command
    assert plugin_report["static_archive_status"] == "present"
    assert plugin_report["registration_status"] == "absent", (
        "A plugin archive cannot imply that its static import object is installed."
    )
    assert plugin_report["registration_objects"]["static_import_objects"][0]["status"] == "absent"

status, calls, summary, metadata_status, metadata_size = run_feedback("PassingCase\nAnotherCase")
assert status == 0, "Two passing selected cases must pass."
assert metadata_status == "complete"
assert metadata_size <= 1024 * 1024
assert [call for call in calls if call[0] == "test_unit"] == [
    ["test_unit", "--case", "PassingCase"], ["test_unit", "--case", "AnotherCase"]
], "Focused feedback must run only the requested exact cases."
assert ["configure.sh", "-D", "CMAKE_CONFIGURATION_TYPES=Debug", "-D", "TDESKTOP_API_TEST=ON"] in calls
assert ["cmake", "--build", "../out", "--config", "Debug", "--target", "test_unit"] in calls
assert "- Cases: 2" in summary
assert "a63240c199f4902ec2a867342c2c6627174e5045" in summary

status, calls, summary, metadata_status, metadata_size = run_feedback("")
assert status == 0
assert metadata_status == "complete"
assert [call for call in calls if call[0] == "test_unit"] == [["test_unit"]]
assert "- Cases: 2" in summary

for first_case in ("FailingCase", "ExitFailure", "MissingCase", "NoSummary"):
    status, calls, summary, metadata_status, metadata_size = run_feedback(f"{first_case} AnotherCase")
    assert status != 0, "A failed or incomplete case must fail the whole batch."
    assert metadata_status == "complete"
    assert [call for call in calls if call[0] == "test_unit"] == [
        ["test_unit", "--case", first_case], ["test_unit", "--case", "AnotherCase"]
    ], "A failed case must not prevent later requested cases from running."
    if first_case == "FailingCase":
        assert "- Cases: 2" in summary
        assert "- Failed cases: 1" in summary
        assert "- Failed checks: 1" in summary

for selection in ("FailingCase AnotherCase", "MissingCase", "IgnoresSelector", "FalseSuccess", "NoSummary"):
    status, calls, summary, metadata_status, metadata_size = run_feedback(selection)
    assert status != 0, "Failed, unknown and invalid case results must fail feedback."

for selection in (" ", "PassingCase;touch", "$(touch canary)", "PassingCase " * 33):
    status, calls, summary, metadata_status, metadata_size = run_feedback(selection)
    assert status != 0, "Malformed or oversized selections must fail before setup."
    assert not calls

status, calls, summary, metadata_status, metadata_size = run_feedback("PassingCase", build_status="23")
assert status != 0
assert not any(call[0] == "test_unit" for call in calls)
assert metadata_status == "failed"

status, calls, summary, metadata_status, metadata_size = run_feedback(
    "PassingCase AnotherCase", metadata_status="incomplete"
)
assert status != 0, "Incomplete environment metadata must fail feedback."
assert metadata_status == "incomplete"
assert [call for call in calls if call[0] == "test_unit"] == [
    ["test_unit", "--case", "PassingCase"], ["test_unit", "--case", "AnotherCase"]
], "Metadata failure must not skip selected unit cases."

status, calls, summary, metadata_status, metadata_size = run_feedback(
    "PassingCase", metadata_status="missing"
)
assert status != 0, "Missing environment metadata must fail feedback."
assert metadata_status == "failed"
assert [call for call in calls if call[0] == "test_unit"] == [["test_unit", "--case", "PassingCase"]]

status, calls, summary, metadata_status, metadata_size = run_feedback(
    "PassingCase", metadata_status="oversized"
)
assert status != 0, "Oversized environment metadata must fail feedback."
assert metadata_status == "incomplete"
assert metadata_size < 1024 * 1024, "Truncation evidence must stay under the output limit."
assert [call for call in calls if call[0] == "test_unit"] == [["test_unit", "--case", "PassingCase"]]

print("Unit feedback tests passed.")
