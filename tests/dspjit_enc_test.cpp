// Compares DSPJIT_EncodeSelfTest() against GNU as output (dspjit_enc.s).
// See tests/run_dspjit_enc.sh.
#include <stdio.h>
#include <string.h>
#include "mednafen/ss/scsp_dspjit_a64.h"

int main(int argc, char** argv)
{
 if(argc < 2) { fprintf(stderr, "usage: %s x.bin\n", argv[0]); return 2; }
 unsigned ref[256]; size_t nref = 0;
 FILE* f = fopen(argv[1], "rb"); if(!f) { perror(argv[1]); return 2; }
 nref = fread(ref, 4, 256, f); fclose(f);
 unsigned mine[256]; size_t n = DSPJIT_EncodeSelfTest(mine, 256);
 int bad = 0;
 if(n != nref) { printf("count mismatch: mine %zu ref %zu\n", n, nref); bad = 1; }
 for(size_t i = 0; i < n && i < nref; i++)
  if(mine[i] != ref[i]) { printf("#%zu: mine %08x ref %08x\n", i, mine[i], ref[i]); bad = 1; }
 printf(bad ? "FAIL\n" : "OK: %zu encodings match\n", n);
 return bad;
}
