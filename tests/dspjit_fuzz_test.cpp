/* dspjit_fuzz_test: random SCSP DSP programs and states, JIT vs interpreter.
**
**   dspjit_fuzz_test [iterations] [seed] [clear_mask_hex] [set_mask_hex]
**
** Exit status 0 when every iteration matches bit for bit (or when the core was
** built without the JIT, which it reports). Links the core library directly
** and supplies the driver callbacks the library expects.
*/
#include <stdio.h>
#include <stdlib.h>
#include <mednafen/mednafen.h>
#include <mednafen/state-driver.h>
#include <mednafen/netplay-driver.h>
#include <mednafen/ss/scsp_dspjit.h>

namespace Mednafen
{
 void MDFND_OutputNotice(MDFN_NoticeType, const char* s) noexcept { fprintf(stderr, "%s\n", s); }
 void MDFND_OutputInfo(const char* s) noexcept { fputs(s, stderr); }
 void MDFND_MidSync(EmulateSpecStruct*, const unsigned) {}
 bool MDFND_CheckNeedExit(void) { return false; }
 void MDFND_MediaSetNotification(uint32, uint32, uint32, uint32) {}
 void MDFND_SetStateStatus(StateStatusStruct*) noexcept {}
 void MDFND_SetMovieStatus(StateStatusStruct*) noexcept {}
 void MDFND_NetplayText(const char*, bool) {}
 void MDFND_NetplaySetHints(bool, bool, uint32) {}
 void MDFND_DispMessage(char*) {}
 void MDFN_RunCheapTests(void) {}	/* tests.cpp is not part of the core build */
}

namespace MDFN_IEN_SS { unsigned SOUND_DSPJIT_Fuzz(unsigned iterations, unsigned seed, uint64_t clear_mask, uint64_t set_mask); }

int main(int argc, char** argv)
{
 const unsigned iterations = argc > 1 ? (unsigned)atoi(argv[1]) : 2000;
 const unsigned seed = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
 /* optional: hex mask of MPROG bits to clear, e.g. 60000000 = no MRT/MWT */
 const uint64_t clear_mask = argc > 3 ? strtoull(argv[3], NULL, 16) : 0;
 const uint64_t set_mask = argc > 4 ? strtoull(argv[4], NULL, 16) : 0;
#if MDFN_SS_SCSP_DSP_JIT
 const unsigned bad = MDFN_IEN_SS::SOUND_DSPJIT_Fuzz(iterations, seed, clear_mask, set_mask);
 printf("%s: %u iterations, %u mismatches\n", bad ? "FAIL" : "OK", iterations, bad);
 return bad ? 1 : 0;
#else
 (void)iterations; (void)seed;
 printf("SKIP: core built without the DSP JIT on this platform\n");
 return 0;
#endif
}
