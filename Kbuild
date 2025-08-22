# make $(src) as absolute path if it isn't already, by prefixing $(srctree)
src := $(if $(patsubst /%,,$(src)),$(srctree)/$(src),$(src))
ccflags-y := -I"$(src)/include/uapi"

# During development, to turn-off treating warning as error, pass
# `CFLAGS_MODULE=-Wno-error` to make as an argument or environment variable.
ccflags-y += -Werror

obj-m := spi-virtio.o
