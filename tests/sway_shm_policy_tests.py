"""Compile the authenticated production Sway filter and exercise real protocols."""
from __future__ import annotations

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def extract_function(source, name):
    start = source.index(f"static bool {name}(")
    opening = source.index("{", start)
    depth = 1
    position = opening + 1
    while depth:
        depth += (source[position] == "{") - (source[position] == "}")
        position += 1
    return source[start:position]


FIXTURE = r"""
#include <stdbool.h>
#include <string.h>
#include <wayland-server-core.h>
struct wlr_security_context_v1_state { int marker; };
struct shell_fixture { struct wl_global *global; };
struct xway_server_fixture { struct wl_client *client; };
struct wlr_xwayland {
    struct shell_fixture *shell_v1;
    struct xway_server_fixture *server;
};
static struct {
    struct { struct wlr_xwayland *wlr_xwayland; } xwayland;
    void *security_context_manager_v1;
} server;
static struct wl_client *sandboxed_client;
static struct wl_global *privileged_global;
static struct wlr_security_context_v1_state security_context;
static struct shell_fixture shell;
static struct xway_server_fixture xway_server;
static struct wlr_xwayland xwayland;
static bool is_privileged(const struct wl_global *global) {
    return global == privileged_global;
}
static const struct wlr_security_context_v1_state *
wlr_security_context_manager_v1_lookup_client(void *manager, struct wl_client *client) {
    (void)manager;
    return client == sandboxed_client ? &security_context : NULL;
}
"""
FIXTURE_EXPORT = r"""
void rack_test_install_policy(struct wl_display *display) {
    wl_display_set_global_filter(display, filter_global, NULL);
}
void rack_test_set_context(struct wl_client *sandboxed, struct wl_global *privileged,
        struct wl_global *xway_shell, struct wl_client *xway_client, bool has_xway_server) {
    sandboxed_client = sandboxed;
    privileged_global = privileged;
    shell.global = xway_shell;
    xway_server.client = xway_client;
    xwayland.shell_v1 = &shell;
    xwayland.server = has_xway_server ? &xway_server : NULL;
    server.xwayland.wlr_xwayland = xway_shell ? &xwayland : NULL;
}
"""


class SwayShmPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix=".sc7-production-shm-", dir=ROOT)
        work = Path(cls.work.name)
        base = (ROOT / "patches/SWAY_BASE_REVISION").read_text().strip()
        original = subprocess.check_output(["git", "-C", str(ROOT / "vendor/sway"),
                                            "show", f"{base}:sway/server.c"], text=True)
        source = work / "sway/server.c"
        source.parent.mkdir()
        source.write_text(original)
        # Isolate git apply from the enclosing Rack checkout; otherwise Git
        # silently excludes paths outside the temporary working subdirectory.
        subprocess.run(["git", "init", "-q", str(work)], check=True)
        subprocess.run(["git", "apply", "--include=sway/server.c",
                        str(ROOT / "patches/sway-sc7labs-rack.patch")], cwd=work, check=True)
        patched = source.read_text()
        cls.library = ROOT / "vendor/wlroots/build"
        cflags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "wayland-server", "wayland-client", "pixman-1"], text=True))
        libs = shlex.split(subprocess.check_output(
            ["pkg-config", "--libs", "wayland-server", "wayland-client", "pixman-1"], text=True))
        common = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                  "-Wno-unused-parameter", "-DWLR_USE_UNSTABLE",
                  "-I", str(ROOT / "vendor/wlroots/include"),
                  "-I", str(ROOT / "vendor/wlroots/build/include"), *cflags]
        cls.clients = {}
        for production, code in ((False, original), (True, patched)):
            policy = extract_function(code, "filter_global")
            if production:
                policy = extract_function(code, "rack_client_buffer_protocol_allowed") + "\n" + policy
            unit = work / f"policy-{production}.c"
            unit.write_text(FIXTURE + policy + FIXTURE_EXPORT)
            for xwayland in (False, True):
                client = work / f"client-{production}-{xwayland}"
                subprocess.run([*common, f"-DHAVE_XWAYLAND={int(xwayland)}", str(unit),
                                str(ROOT / "tests/sway_shm_policy_harness.c"), "-o", str(client),
                                "-L", str(cls.library), "-lwlroots", *libs], check=True)
                cls.clients[production, xwayland] = client

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def run_case(self, *, production=True, xwayland=False, scenario="plain", cycles=1,
                 environment=None):
        env = dict(os.environ, LD_LIBRARY_PATH=str(self.library))
        for name in ("LD_PRELOAD", "SC7_RENDER_TRACE", "SC7_RENDER_EXPERIMENT",
                     "SC7_RENDER_CAPTURE_INPUTS", "WLR_RENDERER"):
            env.pop(name, None)
        if environment:
            env.update(environment)
        run = subprocess.run([str(self.clients[production, xwayland]),
                              "production" if production else "upstream", scenario,
                              str(int(xwayland)), str(cycles)], env=env, text=True,
                             capture_output=True, timeout=20)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("production policy assertions passed", run.stdout)
        self.assertIn(f"dmabuf={0 if production else 1} drm={0 if production else 1} shm=1",
                      run.stdout)
        self.assertIn(f"commits={cycles} releases={cycles}", run.stdout)
        self.assertIn("environment=not-required", run.stdout)
        return run.stdout

    def test_upstream_control_advertises_dma_buf_factories(self):
        self.run_case(production=False)

    def test_plain_production_without_environment_hides_both_dma_buf_factories(self):
        self.run_case()

    def test_real_shm_commit_and_release_cycles_work_under_production_policy(self):
        self.run_case(cycles=128)

    def test_diagnostic_environment_is_not_required_and_cannot_reenable_dma_buf(self):
        self.run_case(environment={"SC7_RENDER_EXPERIMENT": "observe", "SC7_RENDER_TRACE": ""})

    def test_original_privileged_protocol_restrictions_remain_for_sandboxed_client(self):
        output = self.run_case(scenario="sandboxed")
        self.assertIn("privileged=0", output)

    def test_original_unsandboxed_client_keeps_privileged_and_other_protocols(self):
        output = self.run_case()
        self.assertIn("ordinary=1 privileged=1", output)

    def test_xwayland_build_hides_shell_from_unrelated_clients(self):
        self.assertIn("xway=0", self.run_case(xwayland=True))

    def test_xwayland_shell_remains_available_to_authorized_client(self):
        self.assertIn("xway=1", self.run_case(xwayland=True, scenario="xway-authorized"))

    def test_xwayland_shell_is_hidden_when_xwayland_server_is_absent(self):
        self.assertIn("xway=0", self.run_case(xwayland=True, scenario="xway-absent"))


if __name__ == "__main__":
    unittest.main()
