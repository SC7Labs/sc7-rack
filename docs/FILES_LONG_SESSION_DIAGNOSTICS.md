# Files mutation failure after a long Rack session

## Status

This remains a separate release blocker. The reported symptom is that New
Folder, Copy/Paste, Move To, and Extract can stop working after a long Rack
session; closing and reopening Rack restores them. The renderer input policy
does not prove that this failure is resolved.

Existing Files parity tests cover environment, configuration, source contracts,
CLI routing, and archive integrity emulation. They do **not** exercise these
operations in a real Files window after hours of runtime. Passing that suite
cannot clear this blocker.

## What the code establishes

`bin/sc7-rack-files` opens Files' nested Wayland connection and then executes
the stock `cosmic-files` binary. It restores the host D-Bus session, runtime
directory and PATH. Rack does not intercept filesystem mutation jobs. The
layout helper finishes its startup pass rather than repeatedly sending layout
commands throughout the session.

The cached upstream Files source inspected on the main rig is commit
`5d77409e752caeb7f2505de125ed79d18373b757`, while the installed package is
`1.9.0~1790181896~24.04~d1f0af7`. These are not an authenticated source/package
match. That cached source dispatches mutation jobs through an application
operation channel and a dedicated compio runtime. A stopped job dispatcher is
therefore one possible investigation branch; missing input or a stuck dialog
is another. Neither is established as the actual defect.

Clipboard alone cannot explain New Folder and Extract failing at the same
time: those operations do not require the clipboard bridge. A sleeping thread,
`futex` wait, or high virtual memory value alone does not establish a deadlock.

## Capture before, during, and after recovery

From a terminal **outside Rack**, in the checkout used to launch it:

```bash
python3 ./scripts/capture-files-session.py --phase before
```

Use a disposable test directory. Confirm New Folder, file copy/paste, Move To,
and extraction work there. Leave that same Rack session running for at least
the interval that previously triggered the failure, and repeat the operations
at intervals. Record the duration and which operations succeed.

When the problem appears, keep Files and its dialog open. Capture immediately:

```bash
python3 ./scripts/capture-files-session.py --phase failure
```

Record these observations with the snapshot, before restarting:

- Does keyboard navigation, typing, and `Ctrl+Shift+N` still work?
- Does the same New Folder action differ between keyboard and context menu?
- Does a destination dialog appear, and can Cancel still close it?
- Does an operation enter the Files progress panel?
- Was the output actually created on disk, even if the Files listing is stale?
- Does stock host Files still perform the same operation in the disposable
  directory?

Then close and reopen Rack normally, repeat the same disposable-directory
operations, and capture:

```bash
python3 ./scripts/capture-files-session.py --phase after-reopen
```

All three JSON files are saved under
`~/.local/state/sc7-rack/files-diagnostics/`. `--output /chosen/directory`
selects another destination. Files are created with mode `0600`, and a new
output directory with mode `0700`. The tool sends no input, performs no
filesystem operations in Files, changes no settings, and stops no process.

## Evidence and ownership

The snapshot verifies the Rack PID, process start tick, exact local Sway
executable path, and installed Rack configuration. It selects that PID's IPC
socket and the nested Wayland socket held by that compositor. A locally rebuilt
Sway may leave the live executable marked `(deleted)`; only the exact original
checkout path is accepted, and `local_binary_replaced` records this situation.

Files can daemonize after connecting: the PID remembered in the Sway tree can
have exited while another Files process still owns its connection. The tool
uses Linux `UNIX_DIAG_PEER` socket inode evidence to identify the live process
connected to this compositor. It does not select arbitrary host Files
processes by name. The bridge additionally must be in Rack's tracked PID list,
use this checkout's executable, and name this nested display.

Captured evidence includes:

- Rack view geometry, visible/focused state and floating dialogs;
- Sway seat focus and running Sway version;
- positively identified nested Files PID/start tick, RSS, FD count and FD
  categories, bounded thread states and wait channels;
- positively identified bridge PID/start tick and the same counters;
- at most 64 KiB from the existing bridge metadata log, if that owned bridge
  was found. This log can include prior runs; it is not a per-operation trace.

Sway IPC does not expose the exact current pointer grab or Files' Rust task
queue. Seat focus is **not** proof that pointer events reached the intended
widget. These snapshots help distinguish wrong-window/focus symptoms,
process/FD growth and persistent worker state; they cannot by themselves prove
that mutation messages were dispatched or completed. No full environment,
clipboard payload, arbitrary open-file paths or host process dumps are saved.
Window titles can contain directory names, so review a snapshot before sharing.

If socket diagnostics are unavailable, the tool saves the IPC evidence and
returns `3` with `UNVERIFIED`; it does not guess a Files PID. If Rack ownership
or identity is invalid, it returns `2`. A completed scoped snapshot returns `0`.

## Validation performed

Thirteen automated tests cover real netlink message decoding, interrupted and
malformed replies, kernel errors, exact peer selection, host Files exclusion,
daemonized Files identification, PID identity changes, lost connections,
local binary/config rejection, the exact deleted-local-binary case, dialog
focus evidence, missing Rack refusal, and private artifact writing.

A read-only main-rig snapshot on 2026-10-04 UTC successfully identified the
roughly 23-hour-old Rack session and its live daemonized Files process:

| Observation | Value |
| --- | --- |
| Rack PID / start tick | `4125445` / `47536871` |
| Initial Files PID in Sway tree | `4125566` (already exited) |
| Live nested Files PID / start tick | `4125576` / `47537016` |
| Files FDs / threads | `66` / `44` |
| Files RSS | `10576 kB` |
| Proven tracked bridge PID / FDs | `17414` / `5` |
| Ownership errors | None |

This is a baseline observation, **not** a reproduction of the mutation failure
and **not** a successful long-session mutation test. The failure/recovery pair
and repeated real operations remain required before the release blocker can
be cleared.

## Renderer performance measurement limits

`scripts/measure-rack-renderer-cpu.py --seconds 60 --host-cosmic-comp` reads
CPU counters for Rack's compositor and, if uniquely identifiable, host COSMIC.
Use the same laptop, output size and Dashboard/CPU/resize sequence for each
renderer/input combination. Measure idle and active periods separately, repeat
three times, and compare medians. Disable capture/readback while measuring.
The existing tool reports percentage of **one core** and guards PID/start
identity; it does not measure Files/Monitor client CPU or GPU time. Client CPU
must also be measured before claiming the total SHM cost is known.

SHM introduces client rendering and texture-upload costs but retains GLES2
composition. For the observed `597x307` ARGB input, one full upload is
`733116` bytes, about `0.70 MiB`: approximately `0.70 MiB/s` at one update per
second or `42 MiB/s` at 60 updates per second for that pane. This arithmetic is
a full-upload bandwidth estimate, not measured CPU time, GPU utilization or
an assertion about COSMIC's actual redraw rate. Damage updates can reduce
upload work; SHM may also change how the client itself renders.

The main rig has no accessible DRM render node for the physical GLES2 tests.
Software/Pixman measurements there cannot establish the Radeon laptop's
GLES2+SHM versus GLES2+DMA-BUF performance. The laptop's confirmed clean
GLES2+SHM result supports the input policy; acceptable responsiveness and
compositor **and client** CPU usage still belong to the dry-install gate.
