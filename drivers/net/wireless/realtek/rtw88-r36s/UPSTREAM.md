rtw88 USB source provenance
===========================

Driver sources are a USB subset of
https://github.com/lwfinger/rtw88 at
a56bcd26e770257612a0803249cbd4095fc6feca.
The local Makefile and Kconfig limit the build to these modules:

| Option                   | Modules                          | Sources |
|--------------------------|----------------------------------|---------|
| RTW88_R36S (always)      | rtw_core, rtw_usb                | core and usb.c/usb.h |
| RTW88_R36S (always)      | rtw_8821c, rtw_8821cu            | rtw8821c.[ch], rtw8821c_table.[ch], rtw8821cu.c |
| RTW88_R36S_8822BU        | rtw_8822b, rtw_8822bu            | rtw8822b.[ch], rtw8822b_table.[ch], rtw8822bu.c |
| RTW88_R36S_8723DU        | rtw_8723x, rtw_8723d, rtw_8723du | rtw8723x.[ch], rtw8723d.[ch], rtw8723d_table.[ch], rtw8723du.c |
| RTW88_R36S_8812AU        | rtw_88xxa, rtw_8812a, rtw_8812au | rtw88xxa.[ch], rtw8812a.[ch], rtw8812a_table.[ch], rtw8812au.c |
| RTW88_R36S_8821AU        | rtw_88xxa, rtw_8821a, rtw_8821au | rtw88xxa.[ch], rtw8821a.[ch], rtw8821a_table.[ch], rtw8821au.c |

The imported .c and .h files are unchanged except for removal of a
terminal blank line in main.h, sar.c and sar.h. The 8822BU, 8723DU,
8812AU and 8821AU files were added later from the same revision
without changes. sar.c is not built: its contents depend on
Linux 5.11 or newer.
RTL8821CU was tested as an external module on R36S Type1 Panel1 with
kernel release 5.10.252-r36s-clone.

Firmware
--------

Each chip loads one file from /lib/firmware:

| Chip               | Firmware               |
|--------------------|------------------------|
| RTL8821CU/8811CU   | rtw88/rtw8821c_fw.bin  |
| RTL8822BU/8812BU   | rtw88/rtw8822b_fw.bin  |
| RTL8723DU          | rtw88/rtw8723d_fw.bin  |
| RTL8812AU          | rtw88/rtw8812a_fw.bin  |
| RTL8821AU/8811AU   | rtw88/rtw8821a_fw.bin  |

Only firmware/rtw8821c_fw.bin is kept here. It comes from that same
driver revision, SHA256:
2ef409bc418549fcf294061dd0cae1fc22fd9da79b60524950b25de18732f3f0
The other files are in the firmware/ directory of the same driver
revision and in linux-firmware.
The redistribution license is firmware/LICENCE.rtlwifi_firmware.txt.
License source: https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/refs/heads/main/LICENSES/LICENCE.rtlwifi_firmware.txt

Do not enable this backport together with the vendor tree's in-tree RTW88
implementation: both define overlapping driver symbols.
