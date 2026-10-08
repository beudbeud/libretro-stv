#!/bin/sh
# Checks the DSP JIT's AArch64 encoders against GNU as. Needs aarch64-linux-gnu-{as,objcopy}.
set -e
cd "$(dirname "$0")/.."
T=${TMPDIR:-/tmp}/dspjit_enc.$$
mkdir -p "$T"
aarch64-linux-gnu-as -o "$T/x.o" tests/dspjit_enc.s
aarch64-linux-gnu-objcopy -O binary -j .text "$T/x.o" "$T/x.bin"
${CXX:-g++} -std=gnu++11 -I. tests/dspjit_enc_test.cpp -o "$T/enc_test"
"$T/enc_test" "$T/x.bin"
rm -rf "$T"
