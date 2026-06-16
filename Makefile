# AmigaMCP - build with the Bebbo amiga-gcc toolchain (m68k-amigaos-gcc).
#
#   make            -> AmigaMCP using the cleartext bsdsocket transport
#                      (HTTP endpoints / LAN proxy / local llama.cpp)
#   make amissl     -> AmigaMCP using AmiSSL for real HTTPS to api.anthropic.com
#                      (requires the AmiSSL SDK; set AMISSL below)
#
# Build notes for this Windows-hosted toolchain:
#  * -pipe avoids a temp .s file (make's shell has a broken TMP path).
#  * Link as a separate step from a LOCAL .o; a one-step gcc puts the object
#    under /tmp, which the Windows linker fails to read back.

CC      = m68k-amigaos-gcc
# 68020 baseline (runs on 020/030/040/060). Nobody's on a bare 68000 anymore.
CFLAGS  = -Os -m68020 -noixemul -fomit-frame-pointer -pipe -Wall -Wno-pointer-sign
LDFLAGS = -noixemul -s
TARGET  = AmigaMCP

# Path to the AmiSSL SDK (the dir containing include/ with openssl/ and
# proto/amissl.h). Override on the command line: make amissl AMISSL=/path/sdk
AMISSL  = /opt/amissl-sdk

COMMON_OBJ = AmigaMCP.o

all: $(TARGET)

# --- cleartext build (always available) ---------------------------------
$(TARGET): $(COMMON_OBJ) net_plain.o
	$(CC) $(LDFLAGS) $(COMMON_OBJ) net_plain.o -o $(TARGET)

AmigaMCP.o: AmigaMCP.c jsmn.h net.h
	$(CC) $(CFLAGS) -c AmigaMCP.c -o AmigaMCP.o

net_plain.o: net_plain.c net.h
	$(CC) $(CFLAGS) -c net_plain.c -o net_plain.o

# --- HTTPS build via AmiSSL ----------------------------------------------
amissl: $(COMMON_OBJ) net_amissl.o
	$(CC) $(LDFLAGS) $(COMMON_OBJ) net_amissl.o -o $(TARGET) -lamiga

net_amissl.o: net_amissl.c net.h
	$(CC) $(CFLAGS) -DUSE_AMISSL -I$(AMISSL)/include -c net_amissl.c -o net_amissl.o

# --- host-side logic test (needs a native gcc, not the cross compiler) ---
# Exercises extract_text / apply_lines exactly as built into AmigaMCP.c.
HOSTCC = gcc
test: test_logic.c jsmn.h
	$(HOSTCC) -Wall -O2 -o test_logic test_logic.c
	./test_logic

clean:
	rm -f $(TARGET) *.o test_logic test_logic.exe

.PHONY: all amissl test clean
