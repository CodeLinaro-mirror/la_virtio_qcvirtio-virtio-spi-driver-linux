#
# Makefile for virtio-spi kernel module.
#


KERNEL_SRC ?= /lib/modules/`uname -r`/build
KDIR ?= $(KERNEL_SRC)
ifeq ($(KDIR),)
$(error "KDIR must be specified.")
endif

KBUILD_OPTIONS ?= MODNAME=spi-virtio

# The variable "M" is used to point to the location of this module, and it is
# passed to kbuild to build this module.
#
# Allow to specify variable "M" from outside. It is needed to set it to the
# relative path to this module. It must be relative to the kernel source
# directory.
#
# If kernel is built with "O" option then setting "M" to the relative path
# empowers the build system to put output/object files (.o, .ko.) into a
# directory different from the module source directory.
M ?= $$PWD

default:
	$(MAKE) -C $(KDIR) M=$(M) modules $(KBUILD_OPTIONS)

modules_install:
	$(MAKE) -C $(KDIR) M=$(M) INSTALL_MOD_STRIP=1 modules_install

%:
	$(MAKE) -C $(KDIR) M=$(M) $@ $(KBUILD_OPTIONS)

clean:
	$(MAKE) -C $(KDIR) M=$(M) $@
	rm -f *.o *.ko *.mod.c *.mod.o *~ .*.cmd Module.symvers
	rm -rf .tmp_versions
