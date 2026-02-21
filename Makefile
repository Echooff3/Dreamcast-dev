# Dreamcast Demo Makefile
# Requires KallistiOS (KOS) toolchain to be installed and sourced.
# In the Dev Container the environment is pre-configured at:
#   /opt/toolchains/dc/kos/environ.sh

TARGET  = dreamcast_demo.elf
OBJS    = main.o romdisk.o

# KOS build infrastructure
include $(KOS_BASE)/Makefile.rules

# Extra link libraries: pvr is part of libkallisti, cosine, etc.
LDFLAGS_EXTRA =

all: $(TARGET)

$(TARGET): $(OBJS)
	$(KOS_CC) $(KOS_CFLAGS) $(KOS_LDFLAGS) -o $(TARGET) $(KOS_START) \
	    $(OBJS) $(LDFLAGS_EXTRA) $(KOS_LIBS)

# Build the empty romdisk image (no files needed for this demo)
romdisk.img:
	$(KOS_GENROMFS) -f romdisk.img -d romdisk -v

romdisk.o: romdisk.img
	$(KOS_BASE)/utils/bin2o/bin2o romdisk.img romdisk romdisk.o

clean:
	-rm -f $(TARGET) $(OBJS) romdisk.img

.PHONY: all clean
