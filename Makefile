.PHONY: all clean

DEBUG = 0
VER = 1.0.0
KVER = $(shell uname -r)
PWD = $(shell pwd)

EXTRA_CFLAGS := -DDEVICE_VERSION="\"${VER}\""

ifeq ($(DEBUG), 1)
	EXTRA_CFLAGS += -DDEBUG
endif

ifeq ($(KVER),$(shell uname -r))
    obj-m += kscandir.o
	kscandir-objs := scan_dir.o
else
    obj-m += kscandir-$(KVER).o
	kscandir-objs := scan_dir.o
endif

all:
	make -C /lib/modules/$(KVER)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(KVER)/build M=$(PWD) clean
