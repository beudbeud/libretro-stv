// mednafen/time.h — merged Time.h + system <time.h> forwarder
//
// On Windows (case-insensitive filesystem) Time.h == time.h, so this single
// file must serve both roles:
//
//   • system <time.h> replacement : always forward via #include_next
//   • Mednafen::Time C++ namespace : only when types.h is fully initialised
//
// The split-guard design avoids the circular-include problem: types.h sets
// __MDFN_TYPES_H at its opening line (before int64 is defined) and sets
// __MDFN_TYPES_H_COMPLETE at its very last line.  We gate the namespace on
// the completion flag so it is never emitted during types.h initialisation.

// ── Part 1 : system <time.h> (one-shot, no dependency on types.h) ───────────
#ifndef _MDFN_TIME_H_SYSTEM_INCLUDED
#define _MDFN_TIME_H_SYSTEM_INCLUDED
#include_next <time.h>
#endif

// ── Part 2 : Mednafen::Time namespace (C++ only, after types.h is complete) ─
#if defined(__cplusplus) && defined(__MDFN_TYPES_H_COMPLETE) && !defined(__MDFN_TIME_H)
#define __MDFN_TIME_H

namespace Mednafen
{

namespace Time
{
 void Time_Init(void) MDFN_COLD;

 int64 EpochTime(void);
 struct tm LocalTime(const int64 ept);
 static INLINE struct tm LocalTime(void) { return LocalTime(EpochTime()); }

 struct tm UTCTime(const int64 ept);
 static INLINE struct tm UTCTime(void) { return UTCTime(EpochTime()); }

 std::string StrTime(const char* format, const struct tm& tin);
 static INLINE std::string StrTime(const char* format) { return StrTime(format, LocalTime()); }
 static INLINE std::string StrTime(const struct tm& tin) { return StrTime("%c", tin); }
 static INLINE std::string StrTime(void) { return StrTime("%c", LocalTime()); }

 int64 MonoUS(void);	// Microseconds
 static INLINE uint32 MonoMS(void) { return MonoUS() / 1000; } // Milliseconds

 void SleepMS(uint32) noexcept;
}

}
#endif
