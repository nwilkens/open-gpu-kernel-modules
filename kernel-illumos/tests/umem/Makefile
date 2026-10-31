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

TESTS = test_nv_umem_registry test_nv_umem_callback

all: $(TESTS)

test_nv_umem_registry: test_nv_umem_registry.c ../../nvidia/nvidia_umem_registry_impl.h
	$(CC) $(CFLAGS) -UNDEBUG -pthread -o $@ $<

test_nv_umem_callback: test_nv_umem_callback.c nvidia_umem_test_kernel.h \
    ../../nvidia/nvidia_umem.c ../../nvidia/nvidia_umem_registry_impl.h
	$(CC) $(CFLAGS) -Wno-unused-function -UNDEBUG -pthread -I. -o $@ $<

check: all
	./test_nv_umem_registry
	./test_nv_umem_callback

clean:
	rm -f $(TESTS)

.PHONY: all check clean
