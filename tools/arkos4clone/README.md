# ArkOS4Clone kernel package

`build.sh` builds `clone_defconfig` out of tree and lays the result out like
the ArkOS4Clone OS repository, so `dist/boot` and `dist/rootfs` can be copied
over `boot/dArkOS` and `rootfs/dArkOS`.

```
tools/arkos4clone/build.sh --consoles ../arkos4clone/boot/dArkOS/consoles
```

The source tree must be clean of in-tree builds (no `.config`,
`include/config` or `arch/arm64/include/generated`), or Kbuild refuses `O=`.
Uncommitted changes are built but flagged in `build.json`.

## Output (`out/arkos4clone/dist`)

| Path | Contents |
| --- | --- |
| `boot/Image` | the kernel, one for both battery gauges |
| `boot/consoles/<console>/<dtb>` | with `--consoles`: the DTB each OS console folder uses |
| `boot/consoles/dtbo/rk3326-oc-voltage.dtbo` | overclock voltage overlay for this kernel |
| `boot/dtbs/` | every RK3326 DTB |
| `rootfs/usr/lib/modules/<release>/` | stripped modules, depmod run |
| `modules-<release>.tar.gz` | the same modules as a tarball |
| `kernel.config`, `System.map` | build configuration and symbols |
| `build.json`, `SHA256SUMS` | release, commit, compiler and counts; checksums |

The release is `<version>-arkos4clone` (for example `5.10.252-arkos4clone`),
which names the module directory. `LOCALVERSION=-test1` in the environment
appends to it, for test builds that must not clash with an installed release.

## Battery gauge

Both RK817 gauges are built in. The kernel command line picks one:

- no parameter, or `rk817_gauge=rockchip`: the standard Rockchip gauge
  (the OS pairs it with batteryplus);
- `rk817_gauge=arkos4clone`: the ArkOS4Clone coulomb-counting gauge. Its
  `rk817-battery` device then has the `rk817_calibration` attribute that
  rk817-calibrationd saves and restores.

The ArkOS4Clone gauge also takes `rk817_battery_arkos4clone.lowpwr_poweroff`
(default 1), `.drift_correct` and `.drift_log` (default 0). With
`drift_log=1` it logs each rest sample and the correction `drift_correct`
would make (`hybrid: rest ... fix -N%`), without applying it. It always logs
the counter at boot, at each finished charge (`counter N% -> 100%`) and at a
low-battery power off (`counter N% -> 0%`).
