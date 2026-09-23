RTL8821CU source provenance
===========================

Driver sources are the RTL8821CU USB subset of
https://github.com/lwfinger/rtw88 at
a56bcd26e770257612a0803249cbd4095fc6feca.
The local Makefile and Kconfig limit the build to rtw_core, rtw_usb,
rtw_8821c and rtw_8821cu. The imported .c and .h files are unchanged except for removal of a terminal blank line in main.h, sar.c and sar.h.
The driver was tested as an external module on R36S Type1 Panel1 with
kernel release 5.10.252-r36s-clone.

firmware/rtw8821c_fw.bin comes from that same driver revision, SHA256:
2ef409bc418549fcf294061dd0cae1fc22fd9da79b60524950b25de18732f3f0
The redistribution license is firmware/LICENCE.rtlwifi_firmware.txt.
License source: https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/refs/heads/main/LICENSES/LICENCE.rtlwifi_firmware.txt

Do not enable this backport together with the vendor tree's in-tree RTW88
implementation: both define overlapping driver symbols.
