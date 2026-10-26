# SPDX-License-Identifier: GPL-2.0
# rfs_cachectl —— RavenFS mdata 缓存强淘汰用户态工具
#
#   make -C tools                 # 产出 tools/rfs_cachectl
#   make -C tools install         # 安装到 /usr/bin（usrmerge 下即 /bin）
#   make -C tools install DESTDIR=/tmp/pkg PREFIX=/usr
#   make -C tools clean

PREFIX		?= /usr
BINDIR		?= $(PREFIX)/bin
DESTDIR		?=
CC		?= gcc
CFLAGS		?= -O2
CFLAGS		+= -std=gnu11 -Wall -Wextra -Werror
CPPFLAGS	+= -I$(CURDIR)/../include/linux

TARGET		:= rfs_cachectl

all: $(TARGET)

$(TARGET): rfs_cachectl.c ../include/linux/ravenfs_mdata_ioctl.h
	$(CC) $(CFLAGS) $(CPPFLAGS) rfs_cachectl.c -o $@

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install clean
