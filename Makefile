##
# Copyright (c) 2019 Wiele Associates.
# Copyright (c) 2020 Red Hat, Inc. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it would be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write the Free Software Foundation,
# Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
#
##

BUILDROOT = download

bindir ?= /usr/bin
INSTALLDIR = $(DESTDIR)$(bindir)
INSTALL = install
INSTALLOWNER ?= -o root -g root

UDS_DIR = $(BUILDROOT)/vdo-devel/src/c++/uds
UDS_BUILD_DIR = $(UDS_DIR)/userLinux/build
LIBUDS = $(UDS_BUILD_DIR)/libuds.a
LZ4_DIR = $(BUILDROOT)/lz4/lib
LIBLZ4=$(LZ4_DIR)/liblz4.a

DEPLIBS = $(LIBUDS) $(LIBLZ4)
LDLIBS = $(LIBUDS) -L$(LZ4_DIR) -llz4

LDFLAGS = $(LDLIBS) -pthread

DEFINES = -D_GNU_SOURCE

INCLUDES = -I$(UDS_DIR)/src/uds -I$(UDS_DIR)/userLinux/uds -I$(LZ4_DIR) 

CDEBUGFLAGS =

CFLAGS += $(DEFINES) $(INCLUDES) $(CDEBUGFLAGS)

OBJECTS = vdoestimator.o

SOURCES = $(OBJECTS:%.o=%.c)

PROGS = vdoestimator

all: $(PROGS)

test: all
	./runTest

clean:
	$(RM) $(PROGS) $(OBJECTS)

dist-clean: clean
	$(RM) -r $(BUILDROOT)

download:
	mkdir -p $(BUILDROOT)
	git clone https://github.com/lz4/lz4 $(BUILDROOT)/lz4
	git clone https://github.com/dm-vdo/vdo-devel $(BUILDROOT)/vdo-devel

$(OBJECTS): download

install: all
	$(INSTALL) $(INSTALLOWNER) -m 0755 vdoestimator $(INSTALLDIR)

$(LIBLZ4): download
	$(MAKE) -C $(LZ4_DIR)

$(LIBUDS): download
	$(MAKE) -C $(UDS_BUILD_DIR) libuds.a

vdoestimator: $(OBJECTS) $(DEPLIBS)
	$(CC) -o $@ $(OBJECTS) $(CDEBUGFLAGS) $(LDFLAGS)

.PHONY = all test clean dist-clean install
