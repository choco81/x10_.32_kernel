#
# Makefile for the input misc drivers.
#

# Each configuration option enables a list of files.

obj-$(CONFIG_INPUT_APANEL)		+= apanel.o
obj-$(CONFIG_INPUT_ATI_REMOTE)		+= ati_remote.o
obj-$(CONFIG_INPUT_ATI_REMOTE2)		+= ati_remote2.o
obj-$(CONFIG_INPUT_ATLAS_BTNS)		+= atlas_btns.o
obj-$(CONFIG_INPUT_BFIN_ROTARY)		+= bfin_rotary.o
obj-$(CONFIG_INPUT_CAPELLA_CM3602)	+= capella_cm3602.o
obj-$(CONFIG_INPUT_CM109)		+= cm109.o
obj-$(CONFIG_INPUT_COBALT_BTNS)		+= cobalt_btns.o
obj-$(CONFIG_INPUT_DM355EVM)		+= dm355evm_keys.o
obj-$(CONFIG_INPUT_GPIO)		+= gpio_event.o gpio_matrix.o gpio_input.o gpio_output.o gpio_axis.o
ifeq ($(CONFIG_ES209RA_INPUT_GPIO_MATRIX),y)
	obj-$(CONFIG_INPUT_GPIO)	+= es209ra_gpio_matrix.o
else
	obj-$(CONFIG_INPUT_GPIO)	+= gpio_matrix.o
endif
obj-$(CONFIG_HP_SDC_RTC)		+= hp_sdc_rtc.o
obj-$(CONFIG_INPUT_IXP4XX_BEEPER)	+= ixp4xx-beeper.o
obj-$(CONFIG_INPUT_KEYCHORD)		+= keychord.o
obj-$(CONFIG_INPUT_KEYSPAN_REMOTE)	+= keyspan_remote.o
obj-$(CONFIG_INPUT_M68K_BEEP)		+= m68kspkr.o
obj-$(CONFIG_INPUT_PCAP)		+= pcap_keys.o
obj-$(CONFIG_INPUT_PCF50633_PMU)	+= pcf50633-input.o
obj-$(CONFIG_INPUT_PCSPKR)		+= pcspkr.o
obj-$(CONFIG_INPUT_POWERMATE)		+= powermate.o
obj-$(CONFIG_INPUT_RB532_BUTTON)	+= rb532_button.o
obj-$(CONFIG_INPUT_GPIO_ROTARY_ENCODER)	+= rotary_encoder.o
obj-$(CONFIG_INPUT_SGI_BTNS)		+= sgi_btns.o
obj-$(CONFIG_INPUT_SPARCSPKR)		+= sparcspkr.o
obj-$(CONFIG_INPUT_TWL4030_PWRBUTTON)	+= twl4030-pwrbutton.o
obj-$(CONFIG_INPUT_UINPUT)		+= uinput.o
obj-$(CONFIG_INPUT_WINBOND_CIR)		+= winbond-cir.o
obj-$(CONFIG_INPUT_WISTRON_BTNS)	+= wistron_btns.o
obj-$(CONFIG_INPUT_WM831X_ON)		+= wm831x-on.o
obj-$(CONFIG_INPUT_YEALINK)		+= yealink.o
obj-$(CONFIG_PMIC8058_PWRKEY)		+= pmic8058-pwrkey.o
obj-$(CONFIG_PMIC8058_OTHC)		+= pmic8058-othc.o
obj-$(CONFIG_INPUT_KP_FLIP_SWITCH)	+= kp_flip_switch.o
obj-$(CONFIG_BMA150)			+= bma150.o
obj-$(CONFIG_INPUT_BMA150_NG)           += bma150_ng.o

