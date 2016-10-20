#
# Component Makefile (for esp-idf)
#
# This Makefile should, at the very least, just include $(SDK_PATH)/make/component.mk. By default, 
# this will take the sources in this directory, compile them and link them into 
# lib(subdirectory_name).a in the build directory. This behaviour is entirely configurable,
# please read the SDK documents if you need to do this.
#

COMPONENT_SRCDIRS := core espfs util
COMPONENT_ADD_INCLUDEDIRS := core espfs util include
COMPONENT_ADD_LDFLAGS := -lwebpages-espfs -llibesphttpd

COMPONENT_EXTRA_CLEAN := mkespfsimage/*

include $(IDF_PATH)/make/component_common.mk

HTMLDIR := $(subst ",,$(CONFIG_ESPHTTPD_HTMLDIR))

CFLAGS += -DFREERTOS

liblibesphttpd.a: libwebpages-espfs.a

webpages.espfs: $(PROJECT_PATH)/$(HTMLDIR) mkespfsimage/mkespfsimage
ifeq ("$(COMPRESS_W_YUI)","yes")
	$(Q) rm -rf html_compressed;
	$(Q) cp -r ($(PROJECT_PATH)/$(HTMLDIR) html_compressed;
	$(Q) echo "Compression assets with yui-compressor. This may take a while..."
	$(Q) for file in `find html_compressed -type f -name "*.js"`; do $(YUI-COMPRESSOR) --type js $$file -o $$file; done
	$(Q) for file in `find html_compressed -type f -name "*.css"`; do $(YUI-COMPRESSOR) --type css $$file -o $$file; done
	$(Q) awk "BEGIN {printf \"YUI compression ratio was: %.2f%%\\n\", (`du -b -s html_compressed/ | sed 's/\([0-9]*\).*/\1/'`/`du -b -s ../html/ | sed 's/\([0-9]*\).*/\1/'`)*100}"
# mkespfsimage will compress html, css, svg and js files with gzip by default if enabled
# override with -g cmdline parameter
	$(Q) cd html_compressed; find . | $(THISDIR)/espfs/mkespfsimage/mkespfsimage > $(THISDIR)/webpages.espfs; cd ..;
else
	$(Q) cd  $(PROJECT_PATH)/$(HTMLDIR) &&  find . | $(COMPONENT_BUILD_DIR)/mkespfsimage/mkespfsimage > $(COMPONENT_BUILD_DIR)/webpages.espfs
endif

libwebpages-espfs.a: webpages.espfs
	$(Q) $(OBJCOPY) -I binary -O elf32-xtensa-le -B xtensa --rename-section .data=.rodata \
		webpages.espfs webpages.espfs.o.tmp
	$(Q) $(CC) -nostdlib -Wl,-r webpages.espfs.o.tmp -o webpages.espfs.o -Wl,-T $(COMPONENT_PATH)/webpages.espfs.esp32.ld
	$(Q) $(AR) cru $@ webpages.espfs.o

mkespfsimage/mkespfsimage: $(COMPONENT_PATH)/espfs/mkespfsimage
	$(Q) mkdir -p $(COMPONENT_BUILD_DIR)/mkespfsimage
	$(Q) $(MAKE) -C $(COMPONENT_BUILD_DIR)/mkespfsimage -f $(COMPONENT_PATH)/espfs/mkespfsimage/Makefile \
		USE_HEATSHRINK="$(USE_HEATSHRINK)" GZIP_COMPRESSION="$(GZIP_COMPRESSION)" BUILD_DIR=$(COMPONENT_BUILD_DIR)/mkespfsimage \
		CC=$(HOSTCC)
