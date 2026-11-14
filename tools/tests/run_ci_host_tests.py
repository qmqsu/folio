#!/usr/bin/env python3
"""Run an explicit allowlist of synthetic host suites and annotate every failure.

No arbitrary commands, shell strings, device operations or signing jobs are
accepted. Original combined stdout/stderr streams remain in the job log; failed
suites additionally expose their final 80 lines as escaped GitHub annotations.
"""
import argparse
from collections import deque
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SUITES = {
    "release": "run_release_tests.sh",
    "update": "run_update_tests.sh",
    "settings": "run_settings_tests.sh",
    "online_update": "run_online_update_tests.sh",
    "online_update_guard": "run_online_update_guard_tests.sh",
    "power": "run_power_tests.sh",
}


def annotation_data(text):
    """GitHub workflow-command data escaping; percent must be escaped first."""
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def run_suite(name):
    # This lookup also rejects unknown values in programmatic callers.
    runner = ROOT / "tools/tests" / SUITES[name]
    tail = deque(maxlen=80)
    print(f"::group::Host suite: {name}", flush=True)
    try:
        with subprocess.Popen(["sh", str(runner)], cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                              errors="replace", bufsize=1) as process:
            assert process.stdout is not None
            for line in process.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
                tail.append(line)
            result = process.wait()
    except OSError as error:
        line = f"Unable to execute allowlisted suite {name}: {error}\n"
        sys.stdout.write(line)
        tail.append(line)
        result = 1
    # Keep the workflow command on its own line even when a compiler's last
    # diagnostic did not end in a newline. Do not alter the original output.
    if tail and not tail[-1].endswith("\n"):
        print()
    print("::endgroup::", flush=True)
    if result:
        diagnostic = f"Host suite {name} exited {result}. Final {len(tail)} output lines:\n" + "".join(tail)
        print(f"::error title=Host suite {name} failed::{annotation_data(diagnostic)}", flush=True)
    else:
        print(f"Host suite {name}: PASS", flush=True)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suites", nargs="+", choices=tuple(SUITES), help="Only the named host suites run; all selected suites finish before returning failure")
    args = parser.parse_args(argv)
    failed = []
    for name in args.suites:
        if run_suite(name):
            failed.append(name)
    if failed:
        print("Failed host suites: " + ", ".join(failed), flush=True)
        return 1
    print("All selected host suites passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
