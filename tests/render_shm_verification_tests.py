"""Reject misleading physical SHM results from wrong renderers/PIDs or stale input metadata."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/verify-render-input-transport.py"
spec = importlib.util.spec_from_file_location("verify_input_transport", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RenderShmVerificationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="sc7-shm-evidence-", dir=ROOT)
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.runtime = {"environment": {"SC7_RENDER_EXPERIMENT": "shm-input", "WLR_RENDERER": "gles2"}}
        self.tree = {"nodes": [{"app_id": "com.system76.CosmicMonitor", "pid": 29867}]}
        self.trace = [
            "1 wl-buffer-attach resource=0x11 id=97 pid=29867 buffer=0x22 generation=132 kind=shm size=597x307",
            "2 render-begin frame=462 renderer=gles2 buffer=0x33 generation=222 size=1214x624",
            "3 input-capture frame=462 pid=29867 surface_id=32 wl_buffer=0 kind=owned-upload raw_ok=0 gles_status=1 gles_reason=ok",
        ]

    def write(self, *, after=None):
        (self.work / "runtime.json").write_text(json.dumps(self.runtime))
        (self.work / "tree-before-capture.json").write_text(json.dumps(self.tree))
        (self.work / "tree-at-capture.json").write_text(json.dumps(after if after is not None else self.tree))
        (self.work / "render-trace.log").write_text("\n".join(self.trace) + "\n")

    def test_shm_client_upload_and_exact_gles_frame_are_verified(self):
        self.write()
        result = module.verify(self.work, 462)
        self.assertTrue(result["verified"])
        self.assertEqual(result["monitor_pid"], 29867)
        self.assertEqual(result["monitor_input_kind"], "shm")

    def test_dma_or_mixed_transport_is_rejected(self):
        for mixed in (False, True):
            with self.subTest(mixed=mixed):
                original = self.trace[0]
                if mixed:
                    self.trace.append(original.replace("kind=shm", "kind=dmabuf"))
                else:
                    self.trace[0] = original.replace("kind=shm", "kind=dmabuf")
                self.write()
                with self.assertRaisesRegex(ValueError, "not verified SHM"):
                    module.verify(self.work, 462)
                self.trace[0] = original
                if mixed:
                    self.trace.pop()

    def test_requested_gles_but_actual_pixman_frame_is_rejected(self):
        self.trace[1] = self.trace[1].replace("renderer=gles2", "renderer=pixman")
        self.write()
        with self.assertRaisesRegex(ValueError, "verified GLES2 render pass"):
            module.verify(self.work, 462)

    def test_input_capture_kind_does_not_replace_real_attachment_proof(self):
        self.trace.pop(0)
        self.trace[-1] = self.trace[-1].replace("kind=owned-upload", "kind=shm")
        self.write()
        with self.assertRaisesRegex(ValueError, "no attachments"):
            module.verify(self.work, 462)

    def test_retained_shm_attach_cannot_hide_same_frame_dma_or_conflicting_sample(self):
        for mixed in (False, True):
            with self.subTest(mixed=mixed):
                original = self.trace[-1]
                if mixed:
                    self.trace.append(original.replace("kind=owned-upload", "kind=dmabuf"))
                else:
                    self.trace[-1] = original.replace("kind=owned-upload", "kind=dmabuf")
                self.write()
                with self.assertRaisesRegex(ValueError, "sampled input conflicts"):
                    module.verify(self.work, 462)
                if mixed:
                    self.trace.pop()
                else:
                    self.trace[-1] = original

    def test_monitor_absent_or_restarted_is_reported_inconclusive(self):
        for after in ({}, {"floating_nodes": [{"app_id": "com.system76.CosmicMonitor", "pid": 29868}]}):
            with self.subTest(after=after):
                self.write(after=after)
                with self.assertRaisesRegex(ValueError, "stable Monitor PID"):
                    module.verify(self.work, 462)

    def test_other_client_shm_and_old_frame_cannot_validate_monitor(self):
        self.trace[0] = self.trace[0].replace("pid=29867", "pid=29868")
        self.write()
        with self.assertRaisesRegex(ValueError, "no attachments"):
            module.verify(self.work, 462)
        self.trace[0] = self.trace[0].replace("pid=29868", "pid=29867")
        self.trace[-1] = self.trace[-1].replace("frame=462", "frame=461")
        self.write()
        with self.assertRaisesRegex(ValueError, "not captured in this frame"):
            module.verify(self.work, 462)

    def test_rotated_attach_evidence_remains_usable(self):
        (self.work / "render-trace.log.previous").write_text(self.trace.pop(0) + "\n")
        self.write()
        self.assertTrue(module.verify(self.work, 462)["verified"])

    def test_cli_saves_success_and_refuses_invalid_axis(self):
        self.write()
        command = ["python3", str(SCRIPT), str(self.work), "462"]
        good = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertIn("kind=shm", good.stdout)
        destination = self.work / "shm-input-frame-000000000462.json"
        self.assertTrue(json.loads(destination.read_text())["verified"])
        self.runtime["environment"]["SC7_RENDER_EXPERIMENT"] = "observe"
        self.write()
        bad = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(bad.returncode, 3)
        self.assertIn("UNVERIFIED", bad.stderr)
        self.assertFalse(json.loads(destination.read_text())["verified"])


if __name__ == "__main__":
    unittest.main()
