CONFIG_MODULE_SIG=y

obj-m:=emulator.o						#根据make的自动推导原则，make会自动将源程序hds.c编译成目标程序hds.o。
                                    #所有在配置文件中标记为-m的模块将被编译成可动态加载进内核的模块。即后缀为.ko的文件。
# EXTRA_CFLAGS += -I./include
EXTRA_CFLAGS += -I$(LINUX_KERNEL_PATH)/include

CURRENT_PATH:=$(shell pwd)  		#参数化，将模块源码路径保存在CURRENT_PATH中
LINUX_KERNEL:=$(shell uname -r) 	#参数化，将当前内核版本保存在LINUX_KERNEL中
LINUX_KERNEL_PATH:=/usr/src/linux-headers-$(LINUX_KERNEL) 	

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

