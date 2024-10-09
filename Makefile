obj-m:=map_test.o 
obj-m+=xsp_queue_test.o
obj-m+=queue_array_test.o
obj-m+=xsp.o
                  
# EXTRA_CFLAGS += -I./include
# EXTRA_CFLAGS += -I$(LINUX_KERNEL_PATH)/include

# CURRENT_PATH:=$(shell pwd)  		#参数化，将模块源码路径保存在CURRENT_PATH中
LINUX_KERNEL:=$(shell uname -r) 	
LINUX_KERNEL_PATH:=/usr/src/linux-headers-$(LINUX_KERNEL) 	

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
