# libpspwlanfwpatch.a - kernel-mode static library (PSPSDK)
TARGET_LIB = libpspwlanfwpatch.a
OBJS = wfp.o

USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1

CFLAGS = -O2 -G0 -Wall -fno-pic
ASFLAGS = $(CFLAGS)

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
