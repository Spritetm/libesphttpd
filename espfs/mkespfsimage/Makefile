GZIP_COMPRESSION ?= no
USE_HEATSHRINK ?= yes
USE_MMAP ?= yes

CFLAGS=-I../../lib/heatshrink -I../../include -I.. -std=gnu99
ifeq ("$(GZIP_COMPRESSION)","yes")
CFLAGS		+= -DESPFS_GZIP
endif

ifeq ("$(USE_HEATSHRINK)","yes")
CFLAGS		+= -DESPFS_HEATSHRINK
endif

ifeq ("$(USE_MMAP)","yes")
CFLAGS		+= -DUSE_MMAP
endif

OBJS=main.o heatshrink_encoder.o
TARGET=mkespfsimage

$(TARGET): $(OBJS)
ifeq ("$(GZIP_COMPRESSION)","yes")
	$(CC) -o $@ $^ -lz
else
	$(CC) -o $@ $^
endif

clean:
	rm -f $(TARGET) $(OBJS)