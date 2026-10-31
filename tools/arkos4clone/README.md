# ArkOS4Clone kernel package

`build.sh` builds `clone_defconfig` out of tree and lays the result out
exactly like the ArkOS4Clone OS repository: `dist` holds only
`boot/dArkOS` and `rootfs/dArkOS` files, at the paths the OS uses. The OS
keeps its 4.4 kernel; each 5.10 file goes in a `5.10` folder next to the
4.4 file it stands in for, and the DTB selector picks one kernel.

```
tools/arkos4clone/build.sh --os ../arkos4clone
```

`--os` defaults to `../arkos4clone` next to this repository. Each console
folder in its `boot/dArkOS/consoles` gets the 5.10 DTB of the same name as
its 4.4 DTB; the build fails if this kernel does not build one of them.

The source tree must be clean of in-tree builds (no `.config`,
`include/config` or `arch/arm64/include/generated`), or Kbuild refuses `O=`.
Uncommitted changes are built but flagged in `build.json`.

## Updating the OS repository

`dist` drops straight in. The module directory keeps its name between
builds, so replace it rather than copying over it, or modules removed from
the kernel stay behind:

```
rm -rf ../arkos4clone/rootfs/dArkOS/usr/lib/modules/5.10.*-arkos4clone
cp -a out/arkos4clone/dist/. ../arkos4clone/
```

## Output (`out/arkos4clone`)

| Path | Contents |
| --- | --- |
| `dist/boot/dArkOS/consoles/kernel/5.10/Image` | the kernel, one for both battery gauges |
| `dist/boot/dArkOS/consoles/<console>/5.10/<dtb>` | the DTB each OS console folder uses |
| `dist/boot/dArkOS/consoles/dtbo/5.10/rk3326-oc-voltage.dtbo` | overclock voltage overlay for this kernel |
| `dist/rootfs/dArkOS/usr/lib/modules/<release>/` | stripped modules, depmod run |
| `extra/dtbs/` | every RK3326 DTB, including ones no console folder uses |
| `extra/modules-<release>.tar.gz` | the same modules as a tarball |
| `extra/kernel.config`, `extra/System.map` | build configuration and symbols |
| `extra/build.json`, `extra/SHA256SUMS` | release, commit, compiler and counts; checksums of `dist` |

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
