ifneq ($(KERNELRELEASE),)
	obj-m := scull.o
	scull-objs := main.o pipe.o access.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)
default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
endif
