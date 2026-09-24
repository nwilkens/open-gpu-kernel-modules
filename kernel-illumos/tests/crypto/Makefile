#
# Userland test for the nvidia libspdm crypto backend.  Needs illumos libmd and
# OpenSSL 3 headers and libcrypto (pkgsrc, /opt/local).
#
#   gmake check
#

NV_ROOT         ?= $(abspath ../../..)
OPENSSL         ?= /opt/local
CC              := gcc

CFLAGS          := -m64 -std=gnu11 -O2 -g -Wall -Wextra -Werror \
                   -Wno-unused-parameter -Wno-sign-compare \
                   -Wno-deprecated-declarations \
                   -DLIBSPDM_CONFIG=\"nvspdm_rmconfig.h\" \
                   -I$(NV_ROOT)/kernel-open/nvidia -I$(OPENSSL)/include
LDLIBS          := -lmd -L$(OPENSSL)/lib -R$(OPENSSL)/lib -lcrypto

crypto_test: crypto_test.c ../../nvidia/nvidia_crypto.c \
             ../../nvidia/nvidia_crypto_hash.c ../../nvidia/nvidia_crypto.h
	$(CC) $(CFLAGS) -o $@ crypto_test.c $(LDLIBS)

.PHONY: check clean
check: crypto_test
	UMEM_DEBUG=default LD_PRELOAD=libumem.so.1 ./crypto_test

clean:
	rm -f crypto_test
