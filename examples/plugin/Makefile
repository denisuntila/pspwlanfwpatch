TARGET = wfp_example
OBJS = main.o
INCDIR = ../../psp
LIBDIR = ../../psp
LIBS = -lpspwlanfwpatch -lpsploadcore_kernel_660

BUILD_PRX = 1
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

CFLAGS = -O2 -G0 -Wall -fno-pic
ASFLAGS = $(CFLAGS)
LDFLAGS = -nostartfiles

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
