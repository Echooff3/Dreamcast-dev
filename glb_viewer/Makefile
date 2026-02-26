# GLB Model Viewer - Dreamcast Demo Makefile
# Requires KallistiOS (KOS) toolchain to be installed and sourced.

TARGET  = glb_viewer.elf
OBJS    = main.o glb_loader.o glb_anim.o romdisk.o

# KOS build infrastructure
include $(KOS_BASE)/Makefile.rules

# Include path for local deps (cgltf.h)
KOS_CFLAGS += -I./deps

# Link libraries (math)
LDFLAGS_EXTRA = -lm

all: $(TARGET)

$(TARGET): $(OBJS)
	$(KOS_CC) $(KOS_CFLAGS) $(KOS_LDFLAGS) -o $(TARGET) $(KOS_START) \
	    $(OBJS) $(LDFLAGS_EXTRA) $(KOS_LIBS)

# Build the romdisk image containing the GLB model
romdisk.img:
	$(KOS_GENROMFS) -f romdisk.img -d romdisk -v

romdisk.o: romdisk.img
	$(KOS_BASE)/utils/bin2o/bin2o romdisk.img romdisk romdisk.o

clean:
	-rm -f $(TARGET) $(OBJS) romdisk.img

.PHONY: all clean
