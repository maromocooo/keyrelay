# libFuzzer targets (clang with the libFuzzer runtime; on Debian/Ubuntu: apt install clang libclang-rt-dev
# libmbedtls-dev). make -C firmware/tests/fuzz run   (FUZZ_SECONDS=60 by default)
CC ?= clang
ifeq ($(CC),cc)
CC = clang
endif
FUZZ_SECONDS ?= 60
CFLAGS = -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
LINK = ../../components/yd_link

all: fuzz_link_rx fuzz_frag
fuzz_link_rx: fuzz_link_rx.c $(LINK)/yd_sec.c $(LINK)/yd_sec.h
	$(CC) $(CFLAGS) -I $(LINK) -o $@ fuzz_link_rx.c $(LINK)/yd_sec.c -lmbedcrypto
fuzz_frag: fuzz_frag.c $(LINK)/yd_frag.c
	$(CC) $(CFLAGS) -I ../host/stub -I $(LINK)/include -o $@ fuzz_frag.c $(LINK)/yd_frag.c
run: all
	mkdir -p corpus/link_rx corpus/frag
	./fuzz_link_rx -max_total_time=$(FUZZ_SECONDS) -max_len=300 corpus/link_rx
	./fuzz_frag -max_total_time=$(FUZZ_SECONDS) -max_len=4096 corpus/frag
clean:
	rm -rf fuzz_link_rx fuzz_frag corpus crash-* leak-* timeout-* oom-*
.PHONY: all run clean
