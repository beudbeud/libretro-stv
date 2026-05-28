// mednafen/time.h — merged Time.h + system <time.h> forwarder
//
// On Windows (case-insensitive filesystem), Time.h and time.h resolve to the
// same file. This merged header handles both inclusion contexts:
//
//   C++ context  (#include "Time.h" from mednafen code)
//     → provides the Mednafen::Time namespace (original Time.h content)
//   C context    (#include <time.h> from trio/triostr.h etc.)
//     → forwards to the real system <time.h> via #include_next

#ifdef __cplusplus
// ── C++ : Mednafen Time.h ────────────────────────────────────────────────────
#ifndef __MDFN_TIME_H
#define __MDFN_TIME_H

#include <mednafen/types.h>

// Use #include_next to skip mednafen/ and reach the real system time.h
// (plain #include <time.h> would find this file again on Windows)
#include_next <time.h>

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
#endif // __MDFN_TIME_H

#else
// ── C : forward to system <time.h> ──────────────────────────────────────────
#include_next <time.h>
#endif
