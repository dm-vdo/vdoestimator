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
#
# $Id$
##

BUILDROOT = download

bindir ?= /usr/bin
INSTALLDIR = $(DESTDIR)$(bindir)
INSTALL = install
INSTALLOWNER ?= -o root -g root

UDS_DIR = $(BUILDROOT)/vdo/utils/uds
LIBUDS=$(UDS_DIR)/libuds.a
LZ4_DIR = $(BUILDROOT)/lz4/lib
LIBLZ4=$(LZ4_DIR)/liblz4.a

DEPLIBS = $(LIBUDS) $(LIBLZ4)
LDLIBS = $(LIBUDS) -L$(LZ4_DIR) -llz4

LDFLAGS = $(LDLIBS) -pthread

DEFINES = -D_GNU_SOURCE

CDEBUGFLAGS =

CFLAGS += $(DEFINES) -I$(UDS_DIR) -I$(LZ4_DIR) $(CDEBUGFLAGS)

OBJECTS = vdoestimator.o

SOURCES = $(OBJECTS:%.o=%.c)

PROGS = vdoestimator

SUBPROGS = lz4 uds

all: third $(PROGS) test

clean:
	$(RM) $(PROGS) $(OBJECTS)
	$(RM) -r $(BUILDROOT)

download:
	mkdir -p $(BUILDROOT)
	git clone https://github.com/lz4/lz4 $(BUILDROOT)/lz4
	git clone https://github.com/dm-vdo/vdo $(BUILDROOT)/vdo

install: all
	$(INSTALL) $(INSTALLOWNER) -m 0755 vdoestimator $(INSTALLDIR)

lz4: download
	$(MAKE) -C $(LZ4_DIR)

third: $(SUBPROGS)

uds: download
	$(MAKE) -C $(UDS_DIR)

vdoestimator: $(OBJECTS) $(DEPLIBS)
	$(CC) -o $@ $(OBJECTS) $(CDEBUGFLAGS) $(LDFLAGS)

.PHONY = install clean
