#!/usr/bin/env python3
"""Offline checks for the wlroots build-dependency installer."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
from pathlib import Path
import subprocess
import unittest
from unittest import mock


HELPER = Path(__file__).resolve().parent.parent / "scripts/wlroots_build_deps.py"
SPEC = importlib.util.spec_from_file_location("wlroots_build_deps", HELPER)
deps = importlib.util.module_from_spec(SPEC)
import sys
sys.modules[SPEC.name] = deps
SPEC.loader.exec_module(deps)


def requirement(label):
    return next(item for item in deps.REQUIREMENTS if item.label == label)


class BuildDependencyTests(unittest.TestCase):
    def test_bridge_only_checks_and_installs_its_own_inputs(self):
        self.assertEqual(
            [item.label for item in deps.BRIDGE_REQUIREMENTS],
            ["gcc", "C headers and linker", "make", "pkg-config", "pkg:wayland-client"],
        )
        missing = [requirement("make"), requirement("pkg:wayland-client")]
        with mock.patch.object(deps, "missing_requirements", side_effect=[missing, []]) as probe, \
             mock.patch.object(deps, "supported_apt_system", return_value=True), \
             mock.patch.object(deps.os, "geteuid", return_value=1000), \
             mock.patch.object(deps.shutil, "which", return_value="/fake/sudo"), \
             mock.patch.object(deps.subprocess, "run",
                               return_value=subprocess.CompletedProcess([], 0)) as run, \
             redirect_stdout(io.StringIO()):
            result = deps.ensure_build_dependencies(install=True, bridge_only=True)

        self.assertEqual(result, 0)
        self.assertEqual(probe.call_args_list,
                         [mock.call(deps.BRIDGE_REQUIREMENTS), mock.call(deps.BRIDGE_REQUIREMENTS)])
        run.assert_called_once_with(
            [str(deps.PACKAGE_INSTALLER), "make", "libwayland-dev"], check=False,
        )

    def test_apt_mapping_is_minimal_and_deduplicated(self):
        missing = [
            requirement("ninja"),
            requirement("pkg:wayland-client"),
            requirement("pkg:wayland-server>=1.22"),
            requirement("wayland-scanner"),
            requirement("pkg:wayland-scanner"),
            requirement("pkg:wayland-protocols>=1.32"),
        ]
        self.assertEqual(
            deps.apt_packages(missing),
            ["ninja-build", "libwayland-dev", "wayland-protocols"],
        )
        self.assertEqual(
            deps.apt_packages([requirement("wayland-scanner")]),
            ["libwayland-bin"],
        )
        self.assertEqual(deps.apt_packages([]), [])
        self.assertEqual(
            deps.apt_packages([requirement("meson"), requirement("ninja"),
                               requirement("pkg:wayland-protocols>=1.32"),
                               requirement("pkg:xkbcommon")]),
            ["meson", "ninja-build", "wayland-protocols", "libxkbcommon-dev"],
        )

    def test_required_renderer_inputs_map_to_actual_pkg_config_providers(self):
        renderer = [requirement("pkg:egl"), requirement("pkg:gbm>=17.1.0"),
                    requirement("pkg:glesv2")]
        self.assertEqual(
            deps.apt_packages(renderer), ["libegl-dev", "libgbm-dev", "libgles-dev"]
        )
        self.assertEqual(renderer[1].version, "17.1.0")

        queried = []

        def fake_run(command, **_kwargs):
            queried.append(command)
            if command == ["meson", "--version"]:
                return subprocess.CompletedProcess(command, 0, stdout="1.0.0\n")
            return subprocess.CompletedProcess(command, 1 if command in (
                ["pkg-config", "--exists", "egl"],
                ["pkg-config", "--atleast-version=17.1.0", "gbm"],
                ["pkg-config", "--exists", "glesv2"],
            ) else 0)

        with mock.patch.object(deps.shutil, "which", return_value="/fake/tool"), \
             mock.patch.object(deps.subprocess, "run", side_effect=fake_run):
            missing = deps.missing_requirements()
        self.assertEqual(missing, renderer)
        self.assertIn(["pkg-config", "--atleast-version=17.1.0", "gbm"], queried)

    def test_missing_tools_and_old_pkg_config_versions_are_detected(self):
        absent_tools = {"meson", "ninja"}
        queried = []

        def fake_run(command, **kwargs):
            queried.append(command)
            if command[1].startswith("--atleast-version="):
                return subprocess.CompletedProcess(command, 1)
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(
            deps.shutil, "which",
            side_effect=lambda name: None if name in absent_tools else f"/fake/{name}",
        ), mock.patch.object(deps.subprocess, "run", side_effect=fake_run):
            missing = deps.missing_requirements()

        self.assertEqual(
            [item.label for item in missing],
            ["meson", "ninja", "pkg:wayland-server>=1.22",
             "pkg:wayland-protocols>=1.32", "pkg:libdrm>=2.4.114",
             "pkg:pixman-1>=0.42.0", "pkg:gbm>=17.1.0"],
        )
        self.assertIn(
            ["pkg-config", "--atleast-version=1.22", "wayland-server"], queried
        )
        self.assertIn(["pkg-config", "--exists", "xkbcommon"], queried)

    def test_pkg_config_missing_skips_library_probes_until_it_is_installed(self):
        def fake_run(command, **_kwargs):
            return subprocess.CompletedProcess(command, 0, stdout="1.3.0\n")

        with mock.patch.object(
            deps.shutil, "which",
            side_effect=lambda name: None if name == "pkg-config" else f"/fake/{name}",
        ), mock.patch.object(deps.subprocess, "run", side_effect=fake_run) as run:
            missing = deps.missing_requirements()

        self.assertEqual([item.label for item in missing], ["pkg-config"])
        run.assert_any_call(["meson", "--version"], check=False,
                            capture_output=True, text=True)
        self.assertFalse(any(call.args[0][0] == "pkg-config" for call in run.call_args_list))

    def test_gcc_without_headers_or_linker_is_not_a_ready_toolchain(self):
        def fake_run(command, **_kwargs):
            if command[0] == "gcc":
                return subprocess.CompletedProcess(command, 1)
            return subprocess.CompletedProcess(command, 0, stdout="1.3.2\n")

        with mock.patch.object(deps.shutil, "which", return_value="/fake/tool"), \
             mock.patch.object(deps.subprocess, "run", side_effect=fake_run) as run:
            missing = deps.missing_requirements()
        self.assertEqual([item.label for item in missing], ["C headers and linker"])
        self.assertEqual(deps.apt_packages(missing), ["libc6-dev"])
        compiler_call = next(call for call in run.call_args_list if call.args[0][0] == "gcc")
        self.assertIn("#include <stdio.h>", compiler_call.kwargs["input"])
        self.assertIn("-pthread", compiler_call.args[0])
        self.assertIn("-lrt", compiler_call.args[0])

    def test_old_meson_is_reported_before_build(self):
        def fake_run(command, **_kwargs):
            if command == ["meson", "--version"]:
                return subprocess.CompletedProcess(command, 0, stdout="0.58.2\n")
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(deps.shutil, "which", side_effect=lambda name: f"/fake/{name}"), \
             mock.patch.object(deps.subprocess, "run", side_effect=fake_run):
            missing = deps.missing_requirements()

        self.assertEqual([item.label for item in missing], ["meson"])
        self.assertFalse(deps.version_at_least("0.58.2", "0.59.0"))
        self.assertTrue(deps.version_at_least("0.59", "0.59.0"))

    def test_debian_ubuntu_and_pop_are_supported_only_with_apt(self):
        releases = (
            'ID=debian\n',
            'ID=ubuntu\nID_LIKE=debian\n',
            'ID=pop\nID_LIKE="ubuntu debian"\n',
        )
        with mock.patch.object(deps.shutil, "which", return_value="/fake/apt-get"):
            for release in releases:
                with self.subTest(release=release), mock.patch.object(
                    deps.Path, "read_text", return_value=release
                ):
                    self.assertTrue(deps.supported_apt_system())

        with mock.patch.object(deps.Path, "read_text", return_value='ID=pop\n'), \
             mock.patch.object(deps.shutil, "which", return_value=None):
            self.assertFalse(deps.supported_apt_system())

    def test_unsupported_system_does_not_use_apt_even_if_it_is_present(self):
        with mock.patch.object(deps.Path, "read_text", return_value='ID=arch\n'), \
             mock.patch.object(deps.shutil, "which", return_value="/fake/apt-get"):
            self.assertFalse(deps.supported_apt_system())

        missing = [requirement("ninja"), requirement("pkg:xkbcommon")]
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(deps, "missing_requirements", return_value=missing), \
             mock.patch.object(deps, "supported_apt_system", return_value=False), \
             mock.patch.object(deps.subprocess, "run") as run, \
             redirect_stdout(stdout), redirect_stderr(stderr):
            result = deps.ensure_build_dependencies(install=True)

        self.assertEqual(result, 1)
        self.assertIn("ninja", stdout.getvalue())
        self.assertIn("pkg:xkbcommon", stdout.getvalue())
        self.assertIn("Debian, Ubuntu, and Pop!_OS", stderr.getvalue())
        run.assert_not_called()

    def test_successful_apt_install_reprobes_all_requirements(self):
        missing = [requirement("ninja"), requirement("pkg:wayland-client"),
                   requirement("pkg:wayland-server>=1.22")]
        stdout = io.StringIO()
        with mock.patch.object(deps, "missing_requirements", side_effect=[missing, []]) as probe, \
             mock.patch.object(deps, "supported_apt_system", return_value=True), \
             mock.patch.object(deps.os, "geteuid", return_value=1000), \
             mock.patch.object(deps.shutil, "which", return_value="/fake/sudo"), \
             mock.patch.object(
                 deps.subprocess, "run",
                 return_value=subprocess.CompletedProcess([], 0),
             ) as run, redirect_stdout(stdout):
            result = deps.ensure_build_dependencies(install=True)

        self.assertEqual(result, 0)
        self.assertEqual(probe.call_count, 2)
        run.assert_called_once_with(
            [str(deps.PACKAGE_INSTALLER), "ninja-build", "libwayland-dev"], check=False,
        )
        self.assertIn("Build dependencies ready", stdout.getvalue())

    def test_missing_pkg_config_is_installed_before_library_probes(self):
        first = [requirement("pkg-config")]
        second = [requirement("pkg:xkbcommon")]
        with mock.patch.object(
            deps, "missing_requirements", side_effect=[first, second, []]
        ) as probe, mock.patch.object(
            deps, "supported_apt_system", return_value=True
        ), mock.patch.object(deps.os, "geteuid", return_value=0), mock.patch.object(
            deps.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
        ) as run, redirect_stdout(io.StringIO()):
            result = deps.ensure_build_dependencies(install=True)

        self.assertEqual(result, 0)
        self.assertEqual(probe.call_count, 3)
        self.assertEqual(
            [call.args[0] for call in run.call_args_list],
            [
                [str(deps.PACKAGE_INSTALLER), "pkg-config"],
                [str(deps.PACKAGE_INSTALLER), "libxkbcommon-dev"],
            ],
        )

    def test_failed_apt_install_fails_without_claiming_success(self):
        missing = [requirement("meson")]
        stderr = io.StringIO()
        with mock.patch.object(deps, "missing_requirements", return_value=missing) as probe, \
             mock.patch.object(deps, "supported_apt_system", return_value=True), \
             mock.patch.object(deps.os, "geteuid", return_value=0), \
             mock.patch.object(
                 deps.subprocess, "run", return_value=subprocess.CompletedProcess([], 100)
             ) as run, redirect_stdout(io.StringIO()), redirect_stderr(stderr):
            result = deps.ensure_build_dependencies(install=True)

        self.assertEqual(result, 1)
        self.assertEqual(probe.call_count, 1)
        run.assert_called_once()
        self.assertIn("Package installation failed", stderr.getvalue())

    def test_successful_apt_return_with_still_missing_dependency_fails(self):
        missing = [requirement("meson")]
        stderr = io.StringIO()
        with mock.patch.object(
            deps, "missing_requirements", side_effect=[missing, missing, missing]
        ) as probe, mock.patch.object(
            deps, "supported_apt_system", return_value=True
        ), mock.patch.object(deps.os, "geteuid", return_value=0), mock.patch.object(
            deps.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)
        ) as run, redirect_stdout(io.StringIO()), redirect_stderr(stderr):
            result = deps.ensure_build_dependencies(install=True)

        self.assertEqual(result, 1)
        self.assertEqual(probe.call_count, 2)
        self.assertEqual(run.call_count, 1)
        self.assertIn("still missing after installation", stderr.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
