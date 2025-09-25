# SPDX-License-Identifier: GPL-2.0
KDIR ?= $(abspath $(CURDIR)/../../linux)
ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-

obj-m := gpio_pwm_irq.o

all:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		M=$(CURDIR) clean
