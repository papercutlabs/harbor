"""Check the native read-only session preflight over owned disposable sockets and a demo UI.

Run with the Python used by native_smoke.py. HARBOR_EXECUTABLE selects the new binary.
No inherited HOME/XDG path selects a resource; all probes explicitly use this fixture's home
or configuration directory. Cleanup closes only sockets/processes created here. The existing
native PTY fixture always starts --demo and owns its child. No Telegram account is accessed.
"""
import hashlib
import os
from pathlib import Path
import socket
import stat
import struct
import subprocess
import tempfile
import threading
import time

from native_smoke import Terminal

ROOT = Path(__file__).resolve().parents[2]
EXE = str(Path(os.environ.get("HARBOR_EXECUTABLE", ROOT / "build-harbor/bin/harbor")).resolve())


def snapshot(root):
    result = {}
    for path in sorted(root.rglob("*")):
        st = path.lstat()
        result[str(path.relative_to(root))] = (stat.S_IFMT(st.st_mode), st.st_mode & 0o777,
            st.st_ino, hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None)
    return result


def frame(kind, payload):
    return struct.pack("<I", len(payload) + 1) + kind + payload


def welcome(version):
    encoded = version.encode()
    return frame(b"W", struct.pack("<I", len(encoded)) + encoded + struct.pack("<II", 7, 0))


def main():
    # /tmp keeps Unix paths below macOS's sockaddr_un limit regardless of TMPDIR.
    with tempfile.TemporaryDirectory(prefix="harbor-check-", dir="/tmp") as directory:
        root = Path(directory)
        env = dict(os.environ, HOME=str(root / "home"), XDG_CONFIG_HOME=str(root / "xdg"),
                   XDG_STATE_HOME=str(root / "logs"))
        other = root / "unrelated"
        other.mkdir()
        (other / "sentinel").write_text("unrelated profile and draft stay unchanged")
        # Conflicting inherited selectors must not override an explicit -d or demo choice.
        (root / "xdg").mkdir()
        (root / "xdg/harbor").mkdir()
        (root / "xdg/harbor/session.sock").write_text("not a socket")
        identity = subprocess.run([EXE, "--version"], capture_output=True, text=True, check=True,
            env=dict(env, XDG_STATE_HOME=str(root / "version-logs")))
        version = identity.stdout.split()[1]
        accepted = {"0.1.23": ("0.1.22", "0.1.23"),
                    "0.1.24": ("0.1.22", "0.1.23", "0.1.24"),
                    "0.1.25": ("0.1.24", "0.1.25"),
                    "0.1.26": ("0.1.25", "0.1.26"),
                    "0.1.27": ("0.1.26", "0.1.27"),
                    "0.1.28": ("0.1.27", "0.1.28"),
                    "0.1.29": ("0.1.28", "0.1.29"),
                    "0.1.30": ("0.1.29", "0.1.30"),
                    "0.1.31": ("0.1.29", "0.1.30", "0.1.31"),
                    "0.1.32": ("0.1.31", "0.1.32"),
                    "0.1.33": ("0.1.31", "0.1.32", "0.1.33")}[version]

        def probe(args, error=None, check_first=False):
            before = snapshot(root)
            start = time.monotonic()
            argv = [EXE, "--check-session", *args] if check_first else [EXE, *args, "--check-session"]
            result = subprocess.run(argv, env=env,
                                    capture_output=True, text=True, timeout=5)
            elapsed = time.monotonic() - start
            assert result.returncode == (1 if error else 0), (args, result.returncode, result.stdout, result.stderr)
            assert not result.stdout, (args, result.stdout)
            if error:
                assert error in result.stderr, (args, result.stderr)
            else:
                assert not result.stderr, (args, result.stderr)
            assert snapshot(root) == before, ("probe changed files", args)
            return elapsed

        missing = root / "missing/config"
        probe(["-d", str(missing)])
        probe(["-d", str(missing), "--open-telegram-chat", "-1001", "--open-telegram-topic", "3"])
        probe(["--demo"])
        probe(["-d", str(root / "xdg/harbor"), "--demo"])
        probe(["--demo", "-d", str(root / "xdg/harbor")], "not a same-user socket")
        probe([], "not a same-user socket")
        print("PASS absent sessions: no profile, directory, log or lock created; -d/--demo order respected", flush=True)

        config = root / "session"
        config.mkdir()
        (config / "draft-sentinel").write_text("keep draft")
        path = config / "session.sock"

        def peer(data, error=None, slow=False):
            server = socket.socket(socket.AF_UNIX)
            server.bind(str(path))
            server.listen(1)
            received, failures = [], []

            def serve():
                try:
                    server.settimeout(4)
                    conn, _ = server.accept()
                    with conn:
                        conn.settimeout(4)
                        if slow:
                            # Drip a declared frame without ever finishing it. Per-read timeouts
                            # would reset forever; an absolute deadline must still stop this.
                            conn.sendall(struct.pack("<I", 100))
                            until = time.monotonic() + 3
                            while time.monotonic() < until:
                                try:
                                    conn.sendall(b"W")
                                except (BrokenPipeError, ConnectionResetError):
                                    break
                                time.sleep(.05)
                        else:
                            conn.sendall(data)
                            if not data:
                                conn.shutdown(socket.SHUT_WR)  # explicit EOF fixture, without a close race
                            received.append(conn.recv(1))
                except Exception as failure:
                    failures.append(failure)

            thread = threading.Thread(target=serve)
            thread.start()
            try:
                elapsed = probe(["-d", str(config)], error)
            finally:
                thread.join(5)
                server.close()
                path.unlink()
            assert not thread.is_alive() and not failures, failures
            assert slow or received == [b""], received  # the probe sent no protocol requests
            if slow:
                assert 1.5 < elapsed < 3.5, elapsed

        for backend in accepted:
            peer(welcome(backend))
        for backend in ("0.1.21", "0.1.24", "0.1.25", "0.1.26", "0.1.27", "0.1.28", "0.1.29", "0.1.30", "0.1.31", "0.1.32", "0.1.33", "unknown", "0.1.22.1"):
            if backend not in accepted:
                peer(welcome(backend), f"Harbor {backend} is still running in other windows; this is Harbor {version}.")
        peer(frame(b"W", struct.pack("<I", len(version)) + version.encode()), "malformed welcome")
        peer(frame(b"W", struct.pack("<I", len(version)) + version.encode() + struct.pack("<II", 7, 0) + b"extra"), "malformed welcome")
        peer(frame(b"?", b""), "unexpected first frame")
        peer(frame(b"X", b"Original backend failure: unavailable native profile"), "Original backend failure: unavailable native profile")
        peer(frame(b"X", b""), "ended before it started")
        peer(b"", "ended before it started")
        peer(b"", "within two seconds", slow=True)
        print("PASS native welcome: exact/new-to-old allowed; unknown/future/malformed refused; no requests sent; absolute deadline", flush=True)

        stale = socket.socket(socket.AF_UNIX)
        stale.bind(str(path))
        stale.close()
        probe(["-d", str(config)])  # no unlink: ordinary startup owns stale-socket recovery
        path.unlink()
        path.write_text("unknown path")
        probe(["-d", str(config)], "not a same-user socket")
        path.unlink()
        path.symlink_to(other / "sentinel")
        probe(["-d", str(config)], "not a same-user socket")
        path.unlink()
        blocked = root / "blocked"
        blocked.mkdir()
        blocked.chmod(0)
        try:
            if os.geteuid() != 0:
                probe(["-d", str(blocked)], "Permission denied")
        finally:
            blocked.chmod(0o700)
        print("PASS stale sockets remain intact; unknown, symlink and inaccessible paths refused without effects", flush=True)

        for args in (["-s"], ["-r"], ["-k"], ["-x", str(root / "export")], ["-x", ""],
                     ["--session-backend", "-1"], ["--target-surface", "s"], ["--target-pid", "123"],
                     ["--help"], ["--version"]):
            probe([*args, "-d", str(missing)], "only to ordinary Harbor startup arguments")
        for flag in ("--focus-guard", "--focus-guard-effect"):
            for check_first in (False, True):
                probe([flag, "-d", str(missing)], "only to ordinary Harbor startup arguments", check_first)
        for args, error in ((["--open-telegram-chat", "0"], "cannot be zero"),
                            (["--open-telegram-topic", "3"], "require --open-telegram-chat"),
                            (["--open-telegram-chat", "abc"], "signed decimal")):
            probe(["-d", str(missing), *args], error)
        for flag in ("-x", "--export", "-d", "--confdir", "--open-telegram-chat", "--open-telegram-topic",
                     "--open-profile", "--target-surface", "--target-pid"):
            for check_first in (False, True):
                probe(["--demo", "-d", str(missing), flag], f"{flag} requires a value", check_first)
        print("PASS startup argument validation refuses administrative and UI-control actions before effects", flush=True)
        print("PASS missing values cannot consume the probe mode flag in either position; no filesystem effects", flush=True)

        # The real native backend remains usable after a probe disconnects; its UI draft stays.
        terminal = Terminal(config_dir=root / "real-demo", executable=EXE,
            extra_env={key: env[key] for key in ("HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME")})
        try:
            terminal.picker("Marina")
            terminal.send("draft survives native preflight")
            before_other = snapshot(other)
            result = subprocess.run([EXE, "--demo", "-d", terminal.config, "--check-session"],
                                    env=env, capture_output=True, text=True, timeout=5)
            assert result.returncode == 0 and not result.stdout and not result.stderr, result
            terminal.expect("draft survives native preflight")
            terminal.send(" still usable")
            terminal.expect("still usable")
            assert snapshot(other) == before_other
        finally:
            terminal.close()
            terminal.wait_backend_shutdown()
        print("PASS real native demo backend and draft survive a read-only probe", flush=True)


if __name__ == "__main__":
    main()
