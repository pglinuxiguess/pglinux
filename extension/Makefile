# extension/Makefile — Build the linuxsql PostgreSQL extension
#
# Links the kernel's vmlinux.a (all compiled kernel objects) with
# our PG extension glue code to produce linuxsql.so.

EXTENSION = linuxsql
MODULE_big = linuxsql
OBJS = linuxsql.o lds_symbols.o 
DATA = linuxsql--1.0.sql

# Path to the kernel build output
KERNEL_DIR = ../linux-src

# Link the kernel objects into our shared library.
# --whole-archive forces all objects from vmlinux.a to be included.
# --defsym provides the jiffies alias normally set by vmlinux.lds.
# lib/lib.a contains core data structures (rbtree, xarray, kobject, etc.)
# that are referenced by vmlinux.a but built separately by kbuild.
SHLIB_LINK = -Wl,--whole-archive $(KERNEL_DIR)/vmlinux.a $(KERNEL_DIR)/lib/lib.a \
             -Wl,--no-whole-archive \
             -Wl,--defsym=jiffies=jiffies_64 \
             -Wl,--unresolved-symbols=ignore-all \
             -Wl,-T,$(CURDIR)/linuxsql.lds

# Do NOT add kernel include paths here — PG and kernel headers
# have conflicting type definitions (bool, int64_t, fd_set, etc.)
# The interface is through plain C extern declarations only.

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
