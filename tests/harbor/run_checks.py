"""Offline Harbor checks over an already built isolated binary.

Lifecycle: each PTY check owns fresh demo config, starts only --demo and closes its
child in finally. Selection uses a private file at the Clipboard boundary; rich actions use a
private clipboard file and recording opener. No account/profile is read. This
runner waits for each child and stops on its first failure. It never builds,
installs, changes policy, or starts the production application. Native C++ checks
run as finite child processes. Evidence includes the native stdout/stderr/exit and,
before any lane runs, the path, size, mtime and sha256 of each binary used, so a
stale build tree is visible; the caller rebuilds before running.
HARBOR_BUILD_DIR names the existing CMake build tree, HARBOR_CHECK_PYTHON names a
Python with tests/harbor/requirements.txt already available, HARBOR_GO_EXECUTABLE
names the Go used for the input decoder (default go). No install occurs.
A lane whose binary this build cannot have (the producer test outside a Telegram
build) is reported SKIP with its reason instead of failing as a missing file.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
build = Path(os.environ.get("HARBOR_BUILD_DIR", root / "build-harbor")).resolve()
python = os.environ.get("HARBOR_CHECK_PYTHON", sys.executable)
go = os.environ.get("HARBOR_GO_EXECUTABLE", "go")
inventory = json.loads((root / "tests/harbor/check_inventory.json").read_text())
lane = os.environ.get("HARBOR_CHECK_LANE")
if lane and lane not in inventory["lanes"]:
    sys.exit("Unknown HARBOR_CHECK_LANE; use the declared inventory")
native = {"format": "harbor-format-test", "producer": "harbor-richtext-producer-test",
          "cache": "harbor-cache-richtext-test", "media": "harbor-media-test",
          "session": "harbor-session-test"}
lanes = [lane] if lane else list(inventory["lanes"])
env = dict(os.environ, HARBOR_EXECUTABLE=str(build / "bin/harbor"),
           HARBOR_TRACE_PROBE=str(build / "bin/harbor-trace-probe"))


def describe(path):
    if not path.exists():
        return f"BINARY {path} missing"
    stat = path.stat()
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(stat.st_mtime))
    return f"BINARY {path} size={stat.st_size} mtime={stamp} sha256={digest}"


needs = {build / "bin/harbor"} if any(name not in native and name not in ("static", "input_decoding")
                                      for name in lanes) else set()
needs |= {build / "bin" / native[name] for name in lanes if name in native}
if "operational_trace" in lanes: needs.add(build / "bin/harbor-trace-probe")
for path in sorted(needs):
    print(describe(path), flush=True)

for name in lanes:
    if name == "static":
        print("RUN static compile", flush=True)
        for source in inventory["static"]:
            compile((root / source).read_text(), source, "exec")
        print("PASS static", flush=True)
        continue
    cwd = root
    if name in native:
        binary = build / "bin" / native[name]
        if name == "producer" and not binary.exists() and not any(build.rglob("libtgchat*")):
            print("SKIP producer: this build has no Telegram backend (no libtgchat in the build tree), so "
                  "the TDLib producer test is not built; no producer evidence is claimed", flush=True)
            continue
        if not binary.exists():
            sys.exit(f"FAIL {name}: {binary} is missing; build the tree first")
        argv = [str(binary)]
    elif name == "input_decoding":
        if not shutil.which(go):
            sys.exit(f"input_decoding needs Go ({go}); none found, so no decoder evidence")
        argv, cwd = [go, "test", "./..."], root / "src/harbor/input"
    else:
        argv = [python, inventory["lanes"][name][0]]
    print("RUN", name, repr(argv), flush=True)
    if name == "media":
        with tempfile.TemporaryDirectory(prefix="harbor-media-check-") as directory:
            result = subprocess.run(argv + [directory, str(root / "tests/harbor/fixtures")],
                                    cwd=cwd, env=dict(env, HARBOR_TEST_MUTE="1"), timeout=240)
    else:
        result = subprocess.run(argv, cwd=cwd, env=env, timeout=240)
    if result.returncode:
        sys.exit(result.returncode)
    print("PASS", name, flush=True)
