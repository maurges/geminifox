INCLUDES = -I vendor/mbedtls/include/
LIBS = vendor/mbedtls/library/libmbedtls.a vendor/mbedtls/library/libmbedx509.a vendor/mbedtls/library/libmbedcrypto.a
FLAGS = -Wall -Wextra -Wpedantic -Wsign-conversion -std=c23
MBEDTLS_CONFIG = -DMBEDTLS_CONFIG_FILE=\"$(CURDIR)/mbedtls_config.h\"

# Release mode compilation is done by setting RELEASE=target
ifneq ($(RELEASE),)
 AR := zig ar
 RANLIB := zig ranlib
 CFLAGS := -O3 -g0 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -flto -fvisibility=hidden -DNDEBUG
 LDFLAGS := -Wl,--gc-sections -Wl,-s -Wl,--build-id=none

 ifeq ($(RELEASE),linux-amd64)
  CC := zig cc -target x86_64-linux-musl
 else ifeq ($(RELEASE),linux-aarch64)
  CC := zig cc -target aarch64-linux-musl
 else ifeq ($(RELEASE),mac)
  LDFLAGS := $(LDFLAGS) -Wl,-dead_strip
  CC := zig cc -target aarch64-macos

 else ifeq ($(RELEASE),native)
  AR := ar
  RANLIB := ranlib
  CFLAGS := $(CFLAGS) -march=native

 else
  $(error Unknown RELEASE target '$(RELEASE)')
 endif
endif


all: build/geminifox-native build/packet-read build/packet-write

SOURCES = \
  build/main.o \
  build/install.o \
  build/crypto_external.o \
  build/utils.o
build/geminifox-native: $(SOURCES) $(LIBS)
	$(CC) -o $@ $^ $(LDFLAGS) $(FLAGS)

build/packet-write: build/packet-write.o
	$(CC) -o $@ $^ $(LDFLAGS) $(FLAGS) $(CFLAGS)
build/packet-read: build/packet-read.o
	$(CC) -o $@ $^ $(LDFLAGS) $(FLAGS) $(CFLAGS)

build/%.o: src/%.c src/*.h | build
	$(CC) -c -o $@ $< $(CFLAGS) $(FLAGS) $(INCLUDES) $(MBEDTLS_CONFIG)


vendor/mbedtls/%.a: mbedtls_config.h
	$(MAKE) -C $(@D) $(@F) CFLAGS='$(CFLAGS) $(MBEDTLS_CONFIG)'

build:
	mkdir -p build
vendor/mbedtls/:
	git submodule update --init --recursive


install: build/geminifox-native
	$< --install


clean:
	rm -rf build
clean-all: clean
	$(MAKE) -C vendor/mbedtls/ clean

.PHONY: all install clean clean-all
