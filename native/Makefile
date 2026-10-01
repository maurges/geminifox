INCLUDES = -I vendor/mbedtls/include/
LIBS = vendor/mbedtls/library/libmbedtls.a vendor/mbedtls/library/libmbedx509.a vendor/mbedtls/library/libmbedcrypto.a
FLAGS = -Wall -Wextra -std=c23

all: build/gemfox-native build/packet-read build/packet-write

build/gemfox-native: build/main.o vendor/mbedtls/library/libmbedtls.a
	$(CC) -o $@ $< $(LIBS) $(FLAGS)

build/packet-write: build/packet-write.o
	$(CC) -o $@ $< $(LIBS) $(FLAGS)
build/packet-read: build/packet-read.o
	$(CC) -o $@ $< $(LIBS) $(FLAGS)

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
.PHONY: clean
