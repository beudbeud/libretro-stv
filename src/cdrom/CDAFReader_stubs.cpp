/* CDAFReader_stubs.cpp
 * Stubs for CD audio format readers not compiled in libretro build.
 * MPC, Vorbis, and FLAC CD tracks are not used by ST-V/Saturn games in
 * the typical Recalbox setup. These stubs return nullptr (unsupported).
 */
#include "mednafen.h"
#include "cdrom/CDAFReader.h"
#include "cdrom/CDAFReader_MPC.h"
#include "cdrom/CDAFReader_Vorbis.h"
#include "cdrom/CDAFReader_FLAC.h"

namespace Mednafen
{

CDAFReader* CDAFR_MPC_Open(Stream*) { return nullptr; }
CDAFReader* CDAFR_Vorbis_Open(Stream*) { return nullptr; }
CDAFReader* CDAFR_FLAC_Open(Stream*) { return nullptr; }

}
