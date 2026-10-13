#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import tempfile


workflow = Path(os.environ.get(
    "UNIT_FEEDBACK_WORKFLOW", ".github/workflows/unit_feedback.yml"
)).read_text()


def step_script(name):
    step = workflow.split(f"      - name: {name}\n", 1)[1]
    body = step.split("        run: |\n", 1)[1]
    lines = []
    for line in body.splitlines():
        if line and not line.startswith("          "):
            break
        lines.append(line[10:])
    return "\n".join(lines)


def run_feedback(selection, build_status="0"):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        source = root / "source"
        runner = root / "runner"
        tools = root / "bin"
        for path in (source / "Telegram", source / "out/Debug", runner, tools):
            path.mkdir(parents=True, exist_ok=True)
        trace = root / "trace.jsonl"
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
    script = args[-1].replace("/usr/src/tdesktop", os.environ["SOURCE"])
    script = script.replace("/tmp/unit-cases.txt", os.environ["RUNNER_TEMP"] + "/unit-cases.txt")
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
            "SOURCE": str(source),
            "TRACE": str(trace),
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
        return result.returncode, calls, summary.read_text() if summary.exists() else ""


status, calls, summary = run_feedback("PassingCase\nAnotherCase")
assert status == 0, "Two passing selected cases must pass."
assert [call for call in calls if call[0] == "test_unit"] == [
    ["test_unit", "--case", "PassingCase"], ["test_unit", "--case", "AnotherCase"]
], "Focused feedback must run only the requested exact cases."
assert ["configure.sh", "-D", "CMAKE_CONFIGURATION_TYPES=Debug", "-D", "TDESKTOP_API_TEST=ON"] in calls
assert ["cmake", "--build", "../out", "--config", "Debug", "--target", "test_unit"] in calls
assert "- Cases: 2" in summary
assert "a63240c199f4902ec2a867342c2c6627174e5045" in summary

status, calls, summary = run_feedback("")
assert status == 0
assert [call for call in calls if call[0] == "test_unit"] == [["test_unit"]]
assert "- Cases: 2" in summary

for selection in ("FailingCase AnotherCase", "MissingCase", "IgnoresSelector", "FalseSuccess", "NoSummary"):
    status, calls, summary = run_feedback(selection)
    assert status != 0, "Failed, unknown and invalid case results must fail feedback."

for selection in (" ", "PassingCase;touch", "$(touch canary)", "PassingCase " * 33):
    status, calls, summary = run_feedback(selection)
    assert status != 0, "Malformed or oversized selections must fail before setup."
    assert not calls

status, calls, summary = run_feedback("PassingCase", build_status="23")
assert status != 0
assert not any(call[0] == "test_unit" for call in calls)

print("Unit feedback tests passed.")
