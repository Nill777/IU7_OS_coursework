CURRENT = $(shell uname -r)
KDIR = /lib/modules/$(CURRENT)/build
PWD = $(shell pwd)
MOD_NAME := aula_monitor

obj-m := $(MOD_NAME).o

default:
	make -C $(KDIR) M=$(PWD) modules 

clean:
	make -C $(KDIR) M=$(PWD) clean

test:
	sudo insmod $(MOD_NAME).ko && sudo dmesg | tail -n 20 && sudo rmmod $(MOD_NAME) && echo "--- Module unloaded ---" && dmesg | tail -n 5

load:
	sudo insmod $(MOD_NAME).ko

reload:
	$(MAKE) -s unload || true
	$(MAKE) -s clean
	$(MAKE) -s default
	$(MAKE) -s load

unload:
	sudo rmmod $(MOD_NAME)

info:
	lsusb -t

unbind:
	sudo sh -c 'echo "3-2:1.1" > /sys/bus/usb/drivers/usbhid/unbind'