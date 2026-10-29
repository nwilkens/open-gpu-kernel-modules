# SPDX-FileCopyrightText: Copyright (c) 2026 Carlos Neira
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror

TESTS := $(patsubst %.c,%,$(wildcard test_*.c)) test_uvm_channel_stop_illumos test_stop_channel_control_linux test_uvm_import_platform_illumos test_uvm_unset_page_dir_illumos test_rm_dup_free_status_illumos

all: $(TESTS)

test_%: test_%.c
	$(CC) $(CFLAGS) -UNDEBUG -o $@ $<

test_uvm_channel_stop_illumos: test_uvm_channel_stop_platform.c
	$(CC) $(CFLAGS) -UNDEBUG -DNV_SUNOS -o $@ $<

test_stop_channel_control_linux: test_stop_channel_control.c
	$(CC) $(CFLAGS) -UNDEBUG -DTEST_LINUX -o $@ $<

test_uvm_import_platform_illumos: test_uvm_import_platform.c
	$(CC) $(CFLAGS) -UNDEBUG -DNV_SUNOS -o $@ $<

test_uvm_unset_page_dir_illumos: test_uvm_unset_page_dir_checked.c
	$(CC) $(CFLAGS) -UNDEBUG -DNV_SUNOS -o $@ $<

test_rm_dup_free_status_illumos: test_rm_dup_free_status.c
	$(CC) $(CFLAGS) -UNDEBUG -DNV_SUNOS -o $@ $<

check: all
	@set -e; for test in $(TESTS); do ./$$test; done
	python3 test_kernel_channel_schedulable.py
	python3 test_kernel_channel_bind.py
	python3 test_uvm_import_owner_lifetime.py
	python3 test_uvm_import_owner_failure.py

clean:
	rm -f $(TESTS)

.PHONY: all check clean
