# COSMIC System Monitor version parity

The main rig has COSMIC System Monitor `1.9.0` (Pop!_OS package
`1.9.0~1790101371~24.04~fdcc323`). Laptop2 had `1.5.0`, although its
configured package candidate was `1.9.0`. At the same 673 × 345 logical pane
size and output scale 1.0, the main rig's 1.9.0 binary displays the CPU graph
card with Memory below. Pane dimensions alone do not explain the difference.

The [upstream change](https://github.com/pop-os/cosmic-monitor/commit/6338b151e8a7ba35d88c5d9db782388353da18f8)
places graph cards before the application and process lists in the narrow
Dashboard layout. Version 1.5.0 shows the lists first, with graphs farther
down the scroll area; version 1.9.0 shows the graph cards first. The app has
no native command-line option or config setting for this ordering.

Rack's installer checks the version of the `cosmic-monitor` binary that Rack
will launch. If it is older than 1.9.0, the installer requests a targeted
`cosmic-monitor` package upgrade, then checks again. It leaves a current
version alone and reports an error before configuring Rack if the available
package is still too old. Rack does not change the host's display scaling or
modify the Monitor executable.

On laptop2, after closing Rack:

```bash
cd ~/sc7-rack
git pull --ff-only origin main
./install.sh
cosmic-monitor --version
./bin/sc7-rack
```

If the clone is elsewhere, run those commands from the existing Rack clone.
The version command should report 1.9.0 or newer. Check the Dashboard's CPU,
Memory, and Disk cards, switch Dashboard → CPU → Dashboard, and resize Rack.
The physical laptop2 appearance still needs confirmation after upgrade.
