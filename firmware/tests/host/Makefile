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
test_otp_batch: test_otp_batch.c ../../desk_unit/main/otp_batch.h
	$(CC) $(CFLAGS) -I ../../desk_unit/main -o $@ test_otp_batch.c
test_kbd_filter: test_kbd_filter.c ../../receiver/main/kbd_filter.h
	$(CC) $(CFLAGS) -I ../../receiver/main -o $@ test_kbd_filter.c
test_ccid_power: test_ccid_power.c $(LINK)/include/yd_ccid.h
	$(CC) $(CFLAGS) -I $(LINK)/include -o $@ test_ccid_power.c
test_otp_ctl_filter: test_otp_ctl_filter.c ../../desk_unit/main/otp_ctl_filter.h
	$(CC) $(CFLAGS) -I ../../desk_unit/main -o $@ test_otp_ctl_filter.c
test: test_frag test_sec test_bond_policy test_otp_batch test_kbd_filter test_ccid_power test_otp_ctl_filter
	./test_frag
	./test_sec
	./test_bond_policy
	./test_otp_batch
	./test_kbd_filter
	./test_ccid_power
	./test_otp_ctl_filter
clean:
	rm -f test_frag test_sec test_bond_policy test_otp_batch test_kbd_filter test_ccid_power test_otp_ctl_filter
.PHONY: all test clean
