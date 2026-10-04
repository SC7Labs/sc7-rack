#!/usr/bin/env python3
"""Exercise the opt-in renderer diagnostics without touching a live desktop."""

import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[1]


def executable(path, body):
    path.write_text(body)
    path.chmod(0o755)


def wait_for(path, process=None):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process is not None and process.poll() is not None:
            raise AssertionError(f"process exited before {path.name} appeared")
        time.sleep(0.01)
    raise AssertionError(f"timed out waiting for {path.name}")


class DiagnosticRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sc7-render-workflow-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.repo = self.work / "repo with spaces"
        self.runtime = self.work / "runtime"
        self.runtime.mkdir()
        self.mockbin = self.work / "mockbin"
        self.mockbin.mkdir()
        for name in ("scripts", "bin", "tests", "vendor/wlroots/build/include"):
            (self.repo / name).mkdir(parents=True)
        shutil.copy2(ROOT / "scripts/run-render-diagnostic.sh", self.repo / "scripts")
        (self.repo / "tests/render_damage_trace.c").touch()
        executable(self.mockbin / "pkg-config", "#!/bin/sh\nexit 0\n")
        executable(self.mockbin / "cc", "#!/usr/bin/python3\n"
                   "import os, pathlib, sys\n"
                   "pathlib.Path(os.environ['SC7_TEST_COMPILE']).write_text('called')\n"
                   "pathlib.Path(sys.argv[sys.argv.index('-o') + 1]).touch()\n")
        executable(self.repo / "bin/sc7-rack", "#!/usr/bin/python3\n"
                   "import json, os, pathlib, signal, sys, time\n"
                   "if sys.argv[1:] == ['--status']: raise SystemExit(1)\n"
                   "keys = ('LD_PRELOAD', 'SC7_RACK_RENDER_TRACE_LIB', 'SC7_RENDER_TRACE', "
                   "'SC7_RENDER_EXPERIMENT', 'SC7_RENDER_CAPTURE_INPUTS', 'SC7_RACK_FULL_REPAINT', 'WLR_RENDERER')\n"
                   "pathlib.Path(os.environ['SC7_TEST_LAUNCH']).write_text(json.dumps("
                   "dict(pid=os.getpid(), environment={key: os.environ.get(key) for key in keys})))\n"
                   "if os.environ.get('SC7_TEST_WAIT'):\n"
                   "    def stop(*args):\n"
                   "        pathlib.Path(os.environ['SC7_TEST_STOPPED']).write_text('stopped')\n"
                   "        raise SystemExit(0)\n"
                   "    signal.signal(signal.SIGTERM, stop)\n"
                   "    while True: time.sleep(.02)\n")
        self.env = os.environ.copy()
        for name in ("LD_PRELOAD", "SC7_RACK_SWAY_BINARY", "SC7_RACK_RENDER_TRACE_LIB"):
            self.env.pop(name, None)
        self.env.update(
            HOME=str(self.work / "home"),
            XDG_RUNTIME_DIR=str(self.runtime),
            XDG_STATE_HOME=str(self.work / "state with spaces"),
            PATH=f"{self.mockbin}:/usr/bin:/bin",
            SC7_TEST_COMPILE=str(self.work / "compiled"),
            SC7_TEST_LAUNCH=str(self.work / "launch.json"),
            SC7_TEST_STOPPED=str(self.work / "stopped"),
        )
        self.runner = self.repo / "scripts/run-render-diagnostic.sh"

    def run_runner(self, *args, env=None):
        return subprocess.run(["bash", str(self.runner), *args], env=env or self.env,
                              text=True, capture_output=True, timeout=8)

    def test_invalid_modes_and_renderers_are_rejected_before_build_or_launch(self):
        for args in (("--mode", "invalid"), ("--renderer", "vulkan"),
                     ("--renderer", "pixman", "--mode", "fresh-target"),
                     ("--renderer", "pixman", "--mode", "shm-input"),
                     ("--mode",), ("--unexpected",)):
            with self.subTest(args=args):
                result = self.run_runner(*args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse((self.work / "compiled").exists())
                self.assertFalse((self.work / "launch.json").exists())
                self.assertFalse((self.work / "state with spaces").exists())

    def test_pinned_sway_override_is_rejected_without_compiling(self):
        result = self.run_runner(env=self.env | {"SC7_RACK_SWAY_BINARY": "/untrusted/sway"})
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("pinned local Sway", result.stderr)
        self.assertFalse((self.work / "compiled").exists())

    def test_shm_input_keeps_gles2_and_selects_only_requested_diagnostic(self):
        result = self.run_runner("--renderer", "gles2", "--mode", "shm-input", "--capture-inputs")
        self.assertEqual(result.returncode, 0, result.stderr)
        environment = json.loads((self.work / "launch.json").read_text())["environment"]
        self.assertEqual(environment["WLR_RENDERER"], "gles2")
        self.assertEqual(environment["SC7_RENDER_EXPERIMENT"], "shm-input")
        self.assertEqual(environment["SC7_RENDER_CAPTURE_INPUTS"], "1")
        self.assertEqual(environment["SC7_RACK_FULL_REPAINT"], "0")
        self.assertIsNone(environment["LD_PRELOAD"])

    def test_runner_scopes_preload_to_launcher_variable_and_cleans_temporary_library(self):
        result = self.run_runner("--renderer", "gles2", "--mode", "fresh-input", "--capture-inputs")
        self.assertEqual(result.returncode, 0, result.stderr)
        launch = json.loads((self.work / "launch.json").read_text())
        environment = launch["environment"]
        self.assertIsNone(environment["LD_PRELOAD"])
        self.assertEqual(environment["SC7_RENDER_EXPERIMENT"], "fresh-input")
        self.assertEqual(environment["SC7_RENDER_CAPTURE_INPUTS"], "1")
        self.assertEqual(environment["SC7_RACK_FULL_REPAINT"], "0")
        self.assertEqual(environment["WLR_RENDERER"], "gles2")
        trace = Path(environment["SC7_RENDER_TRACE"])
        self.assertTrue(trace.is_file())
        self.assertTrue((trace.parent / "captures").is_dir())
        self.assertEqual(trace.parent.stat().st_mode & 0o777, 0o700)
        self.assertFalse(Path(environment["SC7_RACK_RENDER_TRACE_LIB"]).exists())
        self.assertEqual(list(self.runtime.glob("sc7-rack-render-diagnostic.*")), [])
        self.assertFalse((self.runtime / "sc7-rack/render-diagnostic-dir").exists())

    def test_termination_stops_only_the_launched_process_and_cleans_marker(self):
        process = subprocess.Popen(["bash", str(self.runner)], env=self.env | {"SC7_TEST_WAIT": "1"},
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(lambda: process.kill() if process.poll() is None else None)
        wait_for(self.work / "launch.json", process)
        launch = json.loads((self.work / "launch.json").read_text())
        marker = self.runtime / "sc7-rack/render-diagnostic-dir"
        self.assertTrue(marker.exists())
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=8)
        self.assertEqual(process.returncode, 143, stdout + stderr)
        self.assertTrue((self.work / "stopped").exists())
        self.assertFalse(marker.exists())
        self.assertFalse(Path(launch["environment"]["SC7_RACK_RENDER_TRACE_LIB"]).exists())
        self.assertEqual(list(self.runtime.glob("sc7-rack-render-diagnostic.*")), [])


class DiagnosticCaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sc7-render-capture-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.runtime = self.work / "runtime"
        (self.runtime / "sc7-rack").mkdir(parents=True)
        self.artifacts = self.work / "artifacts with spaces"
        (self.artifacts / "captures").mkdir(parents=True)
        (self.artifacts / "render-trace.log").touch()
        (self.runtime / "sc7-rack/render-diagnostic-dir").write_text(str(self.artifacts) + "\n")
        self.mockbin = self.work / "mockbin"
        self.mockbin.mkdir()
        executable(self.mockbin / "swaymsg", "#!/usr/bin/python3\n"
                   "import json, os, sys\n"
                   "with open(os.environ['SC7_TEST_IPC_CALLS'], 'a') as out:\n"
                   "    out.write(json.dumps(sys.argv[1:]) + '\\n')\n"
                   "print(os.environ.get('SC7_TEST_TREE_JSON', '{}') if 'get_tree' in sys.argv else '[]')\n")
        executable(self.mockbin / "grim", "#!/bin/sh\nexit 1\n")
        executable(self.mockbin / "sleep", "#!/usr/bin/python3\nimport time\ntime.sleep(.002)\n")
        self.env = os.environ.copy()
        self.env.update(XDG_RUNTIME_DIR=str(self.runtime), PATH=f"{self.mockbin}:/usr/bin:/bin",
                        SC7_TEST_IPC_CALLS=str(self.work / "ipc-calls.jsonl"))
        self.env.pop("LD_PRELOAD", None)
        self.process = self.start_idle(diagnostic=True)
        self.sockets = []
        self.addCleanup(lambda: [sock.close() for sock in self.sockets])

    def start_idle(self, diagnostic, extra_env=None):
        env = self.env.copy()
        env.update(extra_env or {})
        names = ("SC7_RENDER_CAPTURE_DIR", "SC7_RENDER_CAPTURE_TRIGGER", "SC7_RENDER_TRACE")
        for name in names:
            env.pop(name, None)
        if diagnostic:
            env.update(SC7_RENDER_CAPTURE_DIR=str(self.artifacts / "captures"),
                       SC7_RENDER_CAPTURE_TRIGGER=str(self.artifacts / "capture-now"),
                       SC7_RENDER_TRACE=str(self.artifacts / "render-trace.log"))
        process = subprocess.Popen(["/usr/bin/python3", "-c", "import time; time.sleep(30)"], env=env)
        def stop():
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
        self.addCleanup(stop)
        (self.runtime / "sc7-rack/sc7-rack.pid").write_text(str(process.pid) + "\n")
        return process

    def ipc_socket(self, pid):
        path = self.runtime / f"sway-ipc.1000.{pid}.sock"
        sock = socket.socket(socket.AF_UNIX)
        self.sockets.append(sock)
        sock.bind(str(path))
        return path

    def run_capture(self, *args):
        return subprocess.run(["bash", str(ROOT / "scripts/capture-render-frame.sh"), *map(str, args)],
                              env=self.env, capture_output=True, text=True, timeout=8)

    def produce_after_trigger(self, name, content):
        errors = []
        def produce():
            try:
                wait_for(self.artifacts / "capture-now")
                (self.artifacts / "capture-now").unlink()
                target = self.artifacts / "captures" / name
                temporary = target.with_suffix(".tmp")
                temporary.write_bytes(content)
                temporary.replace(target)
            except Exception as error:
                errors.append(error)
        thread = threading.Thread(target=produce, daemon=True)
        thread.start()
        return thread, errors

    def test_capture_selects_new_frame_and_encodes_valid_png_using_exact_ipc_pid(self):
        expected_socket = self.ipc_socket(self.process.pid)
        self.ipc_socket(self.process.pid + 12345)
        old = self.artifacts / "captures/pre-submit-999999999999.ppm"
        old.write_bytes(b"P6\n1 1\n255\n\x00\x00\x00")
        pixels = bytes((255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255))
        thread, errors = self.produce_after_trigger("pre-submit-000000000007.ppm", b"P6\n2 2\n255\n" + pixels)
        result = self.run_capture()
        thread.join(2)
        self.assertEqual(errors, [])
        self.assertEqual(result.returncode, 0, result.stderr)
        png = self.artifacts / "captures/pre-submit-000000000007.png"
        self.assertTrue(png.is_file(), result.stdout)
        self.assertFalse(old.with_suffix(".png").exists())
        data = png.read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        offset, chunks = 8, []
        while offset < len(data):
            size = struct.unpack_from(">I", data, offset)[0]
            kind, body = data[offset + 4:offset + 8], data[offset + 8:offset + 8 + size]
            crc = struct.unpack_from(">I", data, offset + 8 + size)[0]
            self.assertEqual(crc, zlib.crc32(kind + body))
            chunks.append((kind, body))
            offset += 12 + size
        self.assertEqual([kind for kind, body in chunks], [b"IHDR", b"IDAT", b"IEND"])
        self.assertEqual(struct.unpack(">IIBBBBB", chunks[0][1]), (2, 2, 8, 2, 0, 0, 0))
        self.assertEqual(zlib.decompress(chunks[1][1]), b"\0" + pixels[:6] + b"\0" + pixels[6:])
        calls = [json.loads(line) for line in (self.work / "ipc-calls.jsonl").read_text().splitlines()]
        self.assertEqual(len(calls), 3)
        self.assertTrue(all(call[:2] == ["-s", str(expected_socket)] for call in calls))
        self.assertEqual(json.loads((self.artifacts / "runtime.json").read_text())["pid"], self.process.pid)
        self.assertFalse((self.artifacts / "capture-now").exists())

    def test_decoy_ipc_socket_does_not_match_current_pid(self):
        self.ipc_socket(self.process.pid + 12345)
        result = self.run_capture()
        self.assertEqual(result.returncode, 1)
        self.assertIn("exact IPC socket", result.stderr)
        self.assertFalse((self.work / "ipc-calls.jsonl").exists())
        self.assertFalse((self.artifacts / "capture-now").exists())

    def test_shm_input_capture_runs_transport_verification_for_exact_frame(self):
        process = self.start_idle(True, {"SC7_RENDER_EXPERIMENT": "shm-input", "WLR_RENDERER": "gles2"})
        self.ipc_socket(process.pid)
        monitor_pid = process.pid + 100
        self.env["SC7_TEST_TREE_JSON"] = json.dumps({"nodes": [
            {"app_id": "com.system76.CosmicMonitor", "pid": monitor_pid}]})
        (self.artifacts / "render-trace.log").write_text(
            f"1 wl-buffer-attach resource=0x1 id=97 pid={monitor_pid} kind=shm size=597x307\n"
            "2 render-begin frame=7 renderer=gles2 buffer=0x2 size=1214x624\n"
            f"3 input-capture frame=7 pid={monitor_pid} kind=owned-upload gles_status=1 gles_reason=ok\n")
        thread, errors = self.produce_after_trigger("pre-submit-000000000007.ppm",
                                                   b"P6\n1 1\n255\n\x00\x00\x00")
        result = self.run_capture()
        thread.join(2)
        self.assertEqual(errors, [])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"shm-input verified: Monitor pid={monitor_pid} kind=shm", result.stdout)
        evidence = self.artifacts / "shm-input-frame-000000000007.json"
        self.assertTrue(json.loads(evidence.read_text())["verified"])

    def test_input_png_alpha_and_manifest_use_only_the_exact_composed_frame(self):
        self.ipc_socket(self.process.pid)
        captures = self.artifacts / "captures"
        name = f"frame-000000000007-pid-{self.process.pid}-surface-12-input-1-gles.ppm"
        source = captures / name
        source.write_bytes(b"P6\n2 1\n255\n" + bytes((17, 33, 49, 64, 32, 16)))
        Path(str(source) + ".alpha.pgm").write_bytes(b"P5\n2 1\n255\n" + bytes((0, 128)))
        old = captures / "frame-000000000006-pid-1-surface-12-input-1-gles.ppm"
        old.write_bytes(b"P6\n1 1\n255\n\x00\x00\x00")
        (self.artifacts / "render-trace.log").write_text(
            "1 input-capture frame=6 pid=1 gles_status=1\n"
            f"2 input-capture frame=7 pid={self.process.pid} gles_status=1 raw_ok=0\n")
        thread, errors = self.produce_after_trigger("pre-submit-000000000007.ppm",
                                                   b"P6\n1 1\n255\n\x00\x00\x00")
        result = self.run_capture()
        thread.join(2)
        self.assertEqual(errors, [])
        self.assertEqual(result.returncode, 0, result.stderr)
        png = source.with_suffix(".png")
        data = png.read_bytes()
        self.assertEqual(struct.unpack(">IIBBBBB", data[16:29]), (2, 1, 8, 6, 0, 0, 0))
        idat_size = struct.unpack_from(">I", data, 33)[0]
        self.assertEqual(data[37:41], b"IDAT")
        self.assertEqual(zlib.decompress(data[41:41 + idat_size]),
                         b"\0" + bytes((17, 33, 49, 0, 64, 32, 16, 128)))
        manifest = json.loads((captures / "pre-submit-000000000007-inputs.json").read_text())
        self.assertEqual(manifest["frame"], 7)
        self.assertEqual(manifest["input_images"], [{"file": png.name, "size": [2, 1], "alpha": True}])
        self.assertEqual(len(manifest["input_trace"]), 1)
        self.assertIn("frame=7", manifest["input_trace"][0])
        self.assertFalse(old.with_suffix(".png").exists())

    def test_invalid_pid_is_rejected_without_creating_capture_request(self):
        (self.runtime / "sc7-rack/sc7-rack.pid").write_text("invalid\n")
        result = self.run_capture()
        self.assertEqual(result.returncode, 1)
        self.assertIn("not running", result.stderr)
        self.assertFalse((self.artifacts / "capture-now").exists())

    def test_invalid_or_incomplete_pixels_do_not_create_png(self):
        self.ipc_socket(self.process.pid)
        for header in (b"P6\n4097 1\n255\n", b"P6\n2 2\n255\n\x00"):
            with self.subTest(header=header):
                for capture in (self.artifacts / "captures").iterdir():
                    capture.unlink()
                thread, errors = self.produce_after_trigger("pre-submit-000000000008.ppm", header)
                result = self.run_capture(self.artifacts)
                thread.join(2)
                self.assertEqual(errors, [])
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(list((self.artifacts / "captures").glob("*.png")), [])

    def test_capture_limit_requests_restart_without_selecting_an_old_frame(self):
        self.ipc_socket(self.process.pid)
        for frame in range(8):
            (self.artifacts / f"captures/pre-submit-{frame:012}.ppm").write_bytes(
                b"P6\n1 1\n255\n\x00\x00\x00")
        (self.artifacts / "render-trace.log").write_text(
            "123 pre-submit frame=10 ok=0 reason=capture-limit limit=8\n")
        result = self.run_capture()
        self.assertEqual(result.returncode, 2)
        self.assertIn("limit reached", result.stderr)
        self.assertIn("Restart", result.stderr)
        self.assertEqual(list((self.artifacts / "captures").glob("*.png")), [])
        self.assertFalse((self.artifacts / "capture-now").exists())

    def test_old_explicit_artifact_directory_is_rejected_without_mutating_evidence(self):
        self.ipc_socket(self.process.pid)
        old = self.work / "old diagnostic"
        (old / "captures").mkdir(parents=True)
        for name in ("tree-before-capture.json", "outputs-before-capture.json", "runtime.json"):
            (old / name).write_text("previous evidence\n")
        result = self.run_capture(old)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("does not belong", result.stderr)
        for name in ("tree-before-capture.json", "outputs-before-capture.json", "runtime.json"):
            self.assertEqual((old / name).read_text(), "previous evidence\n")
        self.assertFalse((self.work / "ipc-calls.jsonl").exists())
        self.assertFalse((old / "capture-now").exists())
        self.assertFalse((self.artifacts / "capture-now").exists())

    def test_non_diagnostic_live_rack_is_rejected_without_writing_capture_metadata(self):
        ordinary = self.start_idle(diagnostic=False)
        self.ipc_socket(ordinary.pid)
        result = self.run_capture()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("not running this renderer diagnostic", result.stderr)
        self.assertFalse((self.work / "ipc-calls.jsonl").exists())
        self.assertFalse((self.artifacts / "runtime.json").exists())
        self.assertFalse((self.artifacts / "tree-before-capture.json").exists())
        self.assertFalse((self.artifacts / "capture-now").exists())


class TracerPreloadIsolationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="sc7-tracer-preload-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        packages = ["pixman-1", "wayland-server", "wayland-client", "libdrm", "egl", "glesv2"]
        if not shutil.which("cc") or not shutil.which("pkg-config"):
            raise unittest.SkipTest("C compiler and pkg-config required for actual preload isolation")
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", *packages],
                               capture_output=True, text=True)
        if flags.returncode or not (ROOT / "vendor/wlroots/build/include").is_dir():
            raise unittest.SkipTest("local wlroots headers and renderer development libraries required")
        cls.library = cls.work / "render-trace.so"
        result = subprocess.run(["cc", "-std=gnu11", "-fPIC", "-shared", "-DWLR_USE_UNSTABLE",
                                 "-I", str(ROOT / "vendor/wlroots/include"),
                                 "-I", str(ROOT / "vendor/wlroots/build/include"),
                                 str(ROOT / "tests/render_damage_trace.c"),
                                 str(ROOT / "tests/render_input_experiment.c"),
                                 str(ROOT / "tests/render_output_experiment.c"),
                                 str(ROOT / "tests/render_input_capture.c"),
                                 str(ROOT / "tests/render_raw_snapshot.c"),
                                 str(ROOT / "tests/render_protocol_trace.c"),
                                 str(ROOT / "tests/render_hold_input.c"),
                                 str(ROOT / "tests/render_shm_input.c"),
                                 "-o", str(cls.library), *shlex.split(flags.stdout), "-ldl"],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)

    def test_actual_trace_rotation_retains_recent_data_with_bounded_storage(self):
        trace = self.work / "rotation.log"
        trace.write_bytes(b"old\n" + b"x" * (16 * 1024 * 1024 - 4))
        script = """import ctypes, json, pathlib, sys
lib = ctypes.CDLL(sys.argv[1])
lib.sc7_render_trace_file.restype = ctypes.c_void_p
libc = ctypes.CDLL(None)
libc.fseek.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.c_int]
libc.fputc.argtypes = [ctypes.c_int, ctypes.c_void_p]
libc.fflush.argtypes = [ctypes.c_void_p]
stream = lib.sc7_render_trace_file()
first = pathlib.Path(sys.argv[2] + '.previous').read_bytes()[:4]
libc.fseek(stream, 16 * 1024 * 1024, 0)
libc.fputc(120, stream)
libc.fflush(stream)
stream = lib.sc7_render_trace_file()
libc.fflush(stream)
print(json.dumps({'first': first.decode(), 'previous': pathlib.Path(sys.argv[2] + '.previous').stat().st_size,
                  'current': pathlib.Path(sys.argv[2]).read_text()}))
"""
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(self.library), SC7_RENDER_TRACE=str(trace))
        result = subprocess.run(["/usr/bin/python3", "-c", script, str(self.library), str(trace)],
                                env=env, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["first"], "old\n")
        self.assertEqual(report["previous"], 16 * 1024 * 1024 + 1)
        self.assertEqual(report["current"], "trace-rotated chunk_limit=16777216\n")

    def test_actual_tracer_is_loaded_in_parent_and_removes_preload_from_descendant(self):
        script = """import json, os, pathlib, subprocess, sys
maps = pathlib.Path('/proc/self/maps').read_text()
child = subprocess.check_output([sys.executable, '-c',
    "import json, os, pathlib; print(json.dumps({'preload': os.getenv('LD_PRELOAD'), 'maps': pathlib.Path('/proc/self/maps').read_text()}))"], text=True)
print(json.dumps({'preload': os.getenv('LD_PRELOAD'), 'loaded': sys.argv[1] in maps, 'child': json.loads(child)}))
"""
        env = os.environ.copy()
        env.update(LD_PRELOAD=str(self.library), SC7_RENDER_TRACE=str(self.work / "trace.log"))
        result = subprocess.run(["/usr/bin/python3", "-c", script, str(self.library)], env=env,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report["loaded"])
        self.assertIsNone(report["preload"])
        self.assertIsNone(report["child"]["preload"])
        self.assertNotIn(str(self.library), report["child"]["maps"])

    def test_actual_launcher_preloads_sway_but_not_bootstrap_or_client_child(self):
        with tempfile.TemporaryDirectory(prefix="sc7-preload-launcher-") as directory:
            work = Path(directory)
            repo = work / "repo"
            for name in ("bin/rack-private", "scripts", "config", "vendor/sway/build/sway",
                         "home", "runtime", "mockbin"):
                (repo / name).mkdir(parents=True)
            shutil.copy2(ROOT / "bin/sc7-rack", repo / "bin")
            for name in ("sc7-rack-files", "sc7-rack-open-host", "rack-private/xdg-open"):
                executable(repo / "bin" / name, "#!/bin/sh\nexit 0\n")
            (repo / "config/config").touch()
            (repo / "config/inner.sh").write_text("#!/bin/sh\n")
            executable(repo / "mockbin/pgrep", "#!/bin/sh\nexit 1\n")
            executable(repo / "scripts/bootstrap-sway.sh", "#!/usr/bin/python3\n"
                       "import os, pathlib\n"
                       "pathlib.Path(os.environ['SC7_TEST_BOOTSTRAP']).write_text("
                       "pathlib.Path('/proc/self/maps').read_text())\n")
            executable(repo / "vendor/sway/build/sway/sway", "#!/usr/bin/python3\n"
                       "import json, os, pathlib, subprocess, sys\n"
                       "child = subprocess.check_output([sys.executable, '-c', "
                       "\"import json, os, pathlib; print(json.dumps({'preload': os.getenv('LD_PRELOAD'), 'maps': pathlib.Path('/proc/self/maps').read_text()}))\"], text=True)\n"
                       "pathlib.Path(os.environ['SC7_TEST_CAPTURE']).write_text(json.dumps({"
                       "'preload': os.getenv('LD_PRELOAD'), 'maps': pathlib.Path('/proc/self/maps').read_text(), 'child': json.loads(child)}))\n")
            env = os.environ.copy()
            env.pop("SC7_RACK_SWAY_BINARY", None)
            env.pop("LD_PRELOAD", None)
            env.update(HOME=str(repo / "home"), XDG_RUNTIME_DIR=str(repo / "runtime"),
                       XDG_CONFIG_HOME=str(repo / "installed-config"),
                       XDG_BIN_HOME=str(repo / "mockbin"), WAYLAND_DISPLAY="wayland-test-host",
                       SC7_RACK_RENDER_TRACE_LIB=str(self.library), SC7_RENDER_TRACE=str(work / "trace.log"),
                       SC7_TEST_BOOTSTRAP=str(work / "bootstrap-maps"),
                       SC7_TEST_CAPTURE=str(work / "sway-report.json"))
            result = subprocess.run(["bash", str(repo / "bin/sc7-rack")], env=env,
                                    capture_output=True, text=True, timeout=8)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(str(self.library), (work / "bootstrap-maps").read_text())
            report = json.loads((work / "sway-report.json").read_text())
            self.assertIn(str(self.library), report["maps"])
            self.assertIsNone(report["preload"])
            self.assertIsNone(report["child"]["preload"])
            self.assertNotIn(str(self.library), report["child"]["maps"])
            self.assertFalse((repo / "runtime/sc7-rack/sc7-rack.pid").exists())


if __name__ == "__main__":
    unittest.main()
