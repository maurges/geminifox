INCLUDES = -I vendor/mbedtls/include/
LIBS = vendor/mbedtls/library/libmbedtls.a vendor/mbedtls/library/libmbedx509.a vendor/mbedtls/library/libmbedcrypto.a
FLAGS = -Wall -Wextra -std=c23

# Release mode compilation is done by setting RELEASE=target
ifneq ($(RELEASE),)
 AR := zig ar
 RANLIB := zig ranlib
 CFLAGS := -O3 -g0 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables
 LDFLAGS := -Wl,--gc-sections -Wl,-s
 ifeq ($(RELEASE),linux-amd64)
  CC := zig cc -target x86_64-linux-musl
 else ifeq ($(RELEASE),linux-aarch64)
  CC := zig cc -target aarch64-linux-musl
 else ifeq ($(RELEASE),mac)
  CC := zig cc -target aarch64-macos
 else
  $(error Unknown RELEASE target '$(RELEASE)')
 endif
endif


all: build/gemfox-native build/packet-read build/packet-write

build/gemfox-native: build/main.o vendor/mbedtls/library/libmbedtls.a
	$(CC) -o $@ $< $(LIBS) $(LDFLAGS) $(FLAGS)

build/packet-write: build/packet-write.o
	$(CC) -o $@ $< $(LIBS) $(LDFLAGS) $(FLAGS)
build/packet-read: build/packet-read.o
	$(CC) -o $@ $< $(LIBS) $(LDFLAGS) $(FLAGS)

build/%.o: src/%.c | build
	$(CC) -c -o $@ $< $(CFLAGS) $(FLAGS) $(INCLUDES)


vendor/mbedtls/library/libmbedtls.a: mbedtls_config/mbedtls/mbedtls_config.h
	make -C vendor/mbedtls/ no_test # TODO config

build:
	mkdir -p build
vendor/mbedtls/:
	git submodule update --init --recursive


clean:
	rm -rf build
	make -C vendor/mbedtls/ clean
.PHONY: clean
