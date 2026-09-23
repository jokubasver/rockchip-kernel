# SPDX-License-Identifier: GPL-2.0
# RTL8821CU subset of lwfinger/rtw88 a56bcd26e770257612a0803249cbd4095fc6feca
ccflags-y += -O2 -std=gnu11 -Wno-declaration-after-statement
ccflags-y += -DCONFIG_RTW88_LEDS=1 -DCONFIG_RTW88_DEBUG=1
ccflags-y += -DCONFIG_RTW88_DEBUGFS=1 -D__CHECK_ENDIAN__

obj-$(CONFIG_RTW88_R36S) += rtw_core.o rtw_usb.o rtw_8821c.o rtw_8821cu.o

rtw_core-objs := main.o led.o mac80211.o util.o debug.o tx.o rx.o
rtw_core-objs += mac.o phy.o coex.o efuse.o fw.o ps.o sec.o bf.o regd.o
rtw_core-$(CONFIG_PM) += wow.o
rtw_usb-objs := usb.o
rtw_8821c-objs := rtw8821c.o rtw8821c_table.o
rtw_8821cu-objs := rtw8821cu.o
