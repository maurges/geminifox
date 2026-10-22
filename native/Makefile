INCLUDES = -I vendor/mbedtls/include/
LIBS = vendor/mbedtls/library/libmbedtls.a vendor/mbedtls/library/libmbedx509.a vendor/mbedtls/library/libmbedcrypto.a
FLAGS = -Wall -Wextra -std=c23

# Release mode compilation is done by setting RELEASE=target
ifneq ($(RELEASE),)
 AR := zig ar
 RANLIB := zig ranlib
 CFLAGS := -O3 -g0 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -flto -fvisibility=hidden -DNDEBUG
 LDFLAGS := -Wl,--gc-sections -Wl,-s -Wl,-dead_strip -Wl,--build-id=none
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


all: build/geminifox-native build/packet-read build/packet-write

build/geminifox-native: build/main.o build/install.o $(LIBS)
	$(CC) -o $@ $^ $(LDFLAGS) $(FLAGS)

build/packet-write: build/packet-write.o
	$(CC) -o $@ $^ $(LDFLAGS) $(FLAGS) $(CFLAGS)
build/packet-read: build/packet-read.o
	$(CC) -o $@ $^ $(LDFLAGS) $(FLAGS) $(CFLAGS)

build/%.o: src/%.c src/*.h | build
	$(CC) -c -o $@ $< $(CFLAGS) $(FLAGS) $(INCLUDES)


vendor/mbedtls/%.a: mbedtls_config/mbedtls/mbedtls_config.h
	$(MAKE) -C $(@D) $(@F) CFLAGS="$(CFLAGS)" # TODO config

build:
	mkdir -p build
vendor/mbedtls/:
	git submodule update --init --recursive


clean:
	rm -rf build
	make -C vendor/mbedtls/ clean
.PHONY: clean
