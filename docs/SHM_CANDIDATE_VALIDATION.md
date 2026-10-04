# SHM input production candidate validation

Renderer policy commit: `3909ef79716944f3507f065c89806fae864a9760`.

Date: 2026-10-03 (America/Toronto). HEAD before work:
`3777f38f785ec0c3949b06e98756e2161e1a651d`.

## Decision and scope

The laptop2 fresh-install A/B establishes that changing nested client transport
from DMA-BUF to SHM keeps the same GLES2 compositor clean. It does not identify
one exact producer/Mesa/kernel/EGL defect. The [source audit](DMABUF_INPUT_AUDIT.md)
provides the side-by-side lifecycle, synchronization behavior, attribute checks,
SC7 patch scope and exact upstream commits assessed. No renderer backport or
wlroots version upgrade was made: none was sufficiently supported by the
physical evidence.

The production candidate now hides `zwp_linux_dmabuf_v1` and legacy `wl_drm`
through the existing private Sway global filter, including explicit numeric
registry binds. `wl_shm`, all remaining security restrictions, renderer/output
selection and allocation remain intact. Plain `rack` needs no experiment flag
or preload. Host-opened apps use the normal host environment and compositor.
There is no hardware heuristic or production DMA-BUF override.

The complete patch is authenticated in bootstrap before building. Incremental
migration must produce the identical expected Git tree. A clean previous
popup-only source is upgraded; genuine dirty source, including a manually
applied but uncommitted new policy, stays rejected. The precise historical VM
partial state still completes its authenticated popup change before migration.
Fresh builds, reruns and `--check` are covered. System Sway/wlroots are untouched.

Performance is estimated, not measured on Radeon: one full 597×307 ARGB upload
is 733,116 bytes, about 42 MiB/s at 60 Hz. SHM may change the client rendering
path as well as add upload work; GLES2 still composes the output. The main rig
has no DRM render node and cannot produce a valid GPU CPU comparison. The
existing CPU tool measures the two compositors, not client CPU or GPU time.
Physical responsiveness and total load remain part of the gate.

## Automated and development-host checks

| Check | Result |
| --- | --- |
| Final Python/unit discovery | 222/222; 124.3 s |
| Production policy, included in discovery | 9/9; exact patched Sway filter compiled; real registries/binds; upstream DMA-capable control; security/Xwayland branches; 128 SHM commit/read/release cycles |
| Sway bootstrap, included in discovery | 14/14; fresh/clean migration, exact state/stamp, rerun/check, VM partial recovery, dirty and mismatched patches rejected |
| Files scoped evidence, included in discovery | 13/13; no claim of live filesystem mutation testing |
| Acceptance | 41/41 |
| Files parity | 33/33; source/config/CLI/emulation coverage, not hours-long GUI mutations |
| Popup lifecycle / launcher menu / isolated host-leave | 100 cycles each |
| Pointer backend | 256 cycles plus historical negative control |
| Cursor/DnD boundary | 128 cycles plus historical assertion negative control |
| DnD lifetime | 240 selection swaps, 480 cancellations/unfocused drops, 240 outgoing endings/leaves, two focused drops with retained transfers, leak negative control |
| Clipboard lifetime | 2,000 cycles, 4,029 offers; ASan/UBSan/LSan clean |
| Host-open bridge | 11/11, including 100 repeated launch cycles |
| Private build / check / second bootstrap | Pass; both vendor tracked trees clean |
| Pixman visual control using production policy | 12 resizes over four requested sizes; real Monitor + three colored terminal representatives; no cross-pane pixels in inner/host captures; negative image detector passes |

The visual control recorded 241 rendered frame commits, 20 buffer identities,
ages 0/2, 201 partial and 40 full damage frames, five isolated input captures,
and zero import failures. Every recorded pane app input kind was SHM. This
control validates production composition and telemetry; it cannot replace
physical GLES2, Dashboard/CPU switching or long Monitor runtime.

The 140-swap private clipboard integration check retained stable process/FD
counts. Acceptance ran in fresh private HOME/runtime fixtures with sharing on;
live settings/autostart hashes remained unchanged. No broad process killing or
destructive cleanup was used. Known preexisting untracked vendor and live
inspection helper files were excluded from commits.

Development-host evidence:

- `/tmp/sc7-production-shm-final-python.log`
- `/tmp/sc7-production-shm-release-gates.log` and `/tmp/sc7-release-gates-c4yt5exf/`
- `/tmp/sc7-candidate-preservation-*.log`
- `/tmp/sc7-production-shm-pixman-control/`

## Changed files

Renderer policy and authentication:
`patches/sway-sc7labs-rack.patch`, `patches/sway-shm-input.patch`,
`scripts/bootstrap-sway.sh`, `tests/sway_bootstrap_tests.py`,
`tests/sway_shm_policy_tests.py`, `tests/sway_shm_policy_harness.c`.

Stronger visual verification and current-policy documentation:
`tests/render_damage_tests.py`, `scripts/run-render-diagnostic.sh`,
`bin/sc7-rack` (comment only), `README.md`,
`docs/DMABUF_INPUT_AUDIT.md`, `docs/RENDERER_COMPATIBILITY.md`,
`docs/RENDER_INPUT_DIAGNOSTICS.md`, `docs/RENDER_RESOURCE_DIAGNOSTICS.md`,
this validation record.

Separate Files blocker evidence tooling:
`scripts/capture-files-session.py`, `tests/files_session_diagnostic_tests.py`,
`docs/FILES_LONG_SESSION_DIAGNOSTICS.md`.

## Remaining physical gates

Use the **single fresh-install sequence** in
[renderer compatibility](RENDERER_COMPATIBILITY.md#dry-physical-install-gate),
then open a new terminal and launch plain `rack`. No diagnostic or manual
bootstrap is part of that gate. Inspect Dashboard → CPU → Dashboard, repeated
resizing, extended runtime and responsiveness; menus, DnD and clipboard both
ways; Files New Folder/Copy/Move To/Extract; host-open common file types and
space names; close/reopen and helper cleanup. Keep the previous clone/state
quarantined rather than deleting it.

This renderer issue qualifies as mitigated for production only after that new
dry physical install stays clean. DMA-BUF itself remains unfixed. The separate
long-session Files mutation failure remains a release blocker. A read-only
23-hour main-rig snapshot proved the live daemonized Files/bridge identities,
but did not reproduce or clear the mutation failure. Capture its before/failure/
after-reopen evidence and repeat real operations as described in
[Files diagnostics](FILES_LONG_SESSION_DIAGNOSTICS.md).

No tag, release or v0.1-ready declaration is authorized by these automated
results. Commit and pushed HEAD are reported in the task response.
