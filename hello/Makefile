KDIR := /home/ubuntu/Intel_study/Intel_BSP/linux
ARCH := arm
CROSS_COMPILE := arm-linux-gnueabihf-

obj-m := Hello_kernel.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
