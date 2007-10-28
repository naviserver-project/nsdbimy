#
# The contents of this file are subject to the Mozilla Public License
# Version 1.1 (the "License"); you may not use this file except in
# compliance with the License. You may obtain a copy of the License at
# http://mozilla.org/
#
# Software distributed under the License is distributed on an "AS IS"
# basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
# the License for the specific language governing rights and limitations
# under the License.
#
# Copyright (C) 2006 Stephen Deasey <sdeasey@gmail.com>
#
# Alternatively, the contents of this file may be used under the terms
# of the GNU General Public License (the "GPL"), in which case the
# provisions of GPL are applicable instead of those above.  If you wish
# to allow use of your version of this file only under the terms of the
# GPL and not to allow others to use your version of this file under the
# License, indicate your decision by deleting the provisions above and
# replace them with the notice and other provisions required by the GPL.
# If you do not delete the provisions above, a recipient may use your
# version of this file under either the License or the GPL.
#

NAVISERVER   = /usr/local/ns
NSD          = $(NAVISERVER)/bin/nsd

MYSQL_CONFIG = mysql_config


MODNAME      = nsdbimy

MOD          = $(MODNAME).so
MODOBJS      = $(MODNAME).o
MODLIBS      = -lnsdbi `$(MYSQL_CONFIG) --libs_r`


include $(NAVISERVER)/include/Makefile.module


#
# The MySQL database to use for testing.
#

export DBIMY_USER=root
export DBIMY_PASSWORD=
export DBIMY_DBNAME=test


test: all
	LD_LIBRARY_PATH="./:$LD_LIBRARY_PATH" $(NSD) -c -d -t tests/config.tcl tests/all.tcl $(TESTFLAGS) $(TCLTESTARGS)

runtest: all
	LD_LIBRARY_PATH="./:$LD_LIBRARY_PATH" $(NSD) -c -d -t tests/config.tcl

gdbtest: all
	@echo "set args -c -d -t tests/config.tcl tests/all.tcl $(TESTFLAGS) $(TCLTESTARGS)" > gdb.run
	LD_LIBRARY_PATH="./:$LD_LIBRARY_PATH" gdb -x gdb.run $(NSD)
	rm gdb.run

gdbruntest: all
	@echo "set args -c -d -t tests/config.tcl" > gdb.run
	LD_LIBRARY_PATH="./:$LD_LIBRARY_PATH" gdb -x gdb.run $(NSD)
	rm gdb.run
