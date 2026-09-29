# SPDX-License-Identifier: GPL-2.0
# USB subset of lwfinger/rtw88 a56bcd26e770257612a0803249cbd4095fc6feca
ccflags-y += -O2 -std=gnu11 -Wno-declaration-after-statement
ccflags-y += -DCONFIG_RTW88_LEDS=1 -DCONFIG_RTW88_DEBUG=1
ccflags-y += -DCONFIG_RTW88_DEBUGFS=1 -D__CHECK_ENDIAN__

rtw88-r36s-mods-y := rtw_core.o rtw_usb.o rtw_8821c.o rtw_8821cu.o
rtw88-r36s-mods-$(CONFIG_RTW88_R36S_8822BU) += rtw_8822b.o rtw_8822bu.o
rtw88-r36s-mods-$(CONFIG_RTW88_R36S_8723DU) += rtw_8723x.o rtw_8723d.o rtw_8723du.o
rtw88-r36s-mods-$(CONFIG_RTW88_R36S_8812AU) += rtw_88xxa.o rtw_8812a.o rtw_8812au.o
rtw88-r36s-mods-$(CONFIG_RTW88_R36S_8821AU) += rtw_88xxa.o rtw_8821a.o rtw_8821au.o
# rtw_88xxa is shared by 8812AU and 8821AU; list each module once.
obj-$(CONFIG_RTW88_R36S) += $(sort $(rtw88-r36s-mods-y))

rtw_core-objs := main.o led.o mac80211.o util.o debug.o tx.o rx.o
rtw_core-objs += mac.o phy.o coex.o efuse.o fw.o ps.o sec.o bf.o regd.o
rtw_core-$(CONFIG_PM) += wow.o
rtw_usb-objs := usb.o
rtw_8821c-objs := rtw8821c.o rtw8821c_table.o
rtw_8821cu-objs := rtw8821cu.o
rtw_8822b-objs := rtw8822b.o rtw8822b_table.o
rtw_8822bu-objs := rtw8822bu.o
rtw_8723x-objs := rtw8723x.o
rtw_8723d-objs := rtw8723d.o rtw8723d_table.o
rtw_8723du-objs := rtw8723du.o
rtw_88xxa-objs := rtw88xxa.o
rtw_8812a-objs := rtw8812a.o rtw8812a_table.o
rtw_8812au-objs := rtw8812au.o
rtw_8821a-objs := rtw8821a.o rtw8821a_table.o
rtw_8821au-objs := rtw8821au.o
