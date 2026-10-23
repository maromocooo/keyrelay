# Host tests (no ESP-IDF needed; test_sec needs libmbedtls-dev): make -C firmware/tests/host
CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -Wno-unused-parameter
LINK = ../../components/yd_link

all: test
test_frag: test_frag.c $(LINK)/yd_frag.c
	$(CC) $(CFLAGS) -I stub -I $(LINK)/include -o $@ $^
# needs mbedtls (Debian/Ubuntu: apt install libmbedtls-dev)
test_sec: test_sec.c $(LINK)/yd_sec.c sec_vectors.h
	$(CC) $(CFLAGS) -I $(LINK) -o $@ test_sec.c $(LINK)/yd_sec.c -lmbedcrypto
test_bond_policy: test_bond_policy.c $(LINK)/yd_bond_policy.h
	$(CC) $(CFLAGS) -I $(LINK) -o $@ test_bond_policy.c
test: test_frag test_sec test_bond_policy
	./test_frag
	./test_sec
	./test_bond_policy
clean:
	rm -f test_frag test_sec test_bond_policy
.PHONY: all test clean
