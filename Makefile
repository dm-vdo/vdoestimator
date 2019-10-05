##
# Copyright (c) 2019 Wiele Associates.
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

BUILD_DIR=..

UDS_DIR = $(BUILD_DIR)/vdo/utils/uds
KUDS_DIR = $(BUILD_DIR)/kvdo/uds
LIBUDS=$(UDS_DIR)/libuds.a
LZ4_DIR = $(BUILD_DIR)/lz4/lib
LIBLZ4=$(LZ4_DIR)/liblz4.a

DEPLIBS = $(LIBUDS) $(LIBLZ4)
LDLIBS = $(LIBUDS) -L$(LZ4_DIR) -llz4

LDFLAGS = $(LDLIBS) -pthread

DEFINES = -D_GNU_SOURCE

CDEBUGFLAGS =

CFLAGS += $(DEFINES) -I$(UDS_DIR) -I$(LZ4_DIR) $(CDEBUGFLAGS)

OBJECTS = vdoestimator.o block.o

SOURCES = $(OBJECTS:%.o=%.c)

PROGS = vdoestimator

all: $(PROGS)

clean:
	$(RM) $(PROGS) $(OBJECTS) block.c

vdoestimator: $(OBJECTS) $(DEPLIBS)
	$(CC) -o $@ $(OBJECTS) $(CDEBUGFLAGS) $(LDFLAGS)

block.c: $(KUDS_DIR)/block.c
	ln -s $< $@

