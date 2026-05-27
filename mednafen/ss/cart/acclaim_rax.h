/******************************************************************************/
/* Mednafen Sega Saturn Emulation Module                                      */
/******************************************************************************/
/* acclaim_rax.h - Acclaim RAX Sound Board HLE
**  Copyright (C) 2024 Mednafen Team
**
** HLE for the Acclaim RAX (Reconfigurable Audio eXtension) board found in
** Batman Forever (STV). The board contains an ADSP-2181 DSP running at
** 16.670 MHz, fed commands by the SH-2 via a 16-bit host port at 0x04800000.
** Audio is output via SPORT0 serial at approximately 44100 Hz stereo.
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
*/

#ifndef __MDFN_SS_CART_ACCLAIM_RAX_H
#define __MDFN_SS_CART_ACCLAIM_RAX_H

namespace MDFN_IEN_SS
{

/* Maximum RAX ROM size: 16 MB (0x1000000 bytes).
 * Layout from MAME "rax" region:
 *   0x000000..0x07FFFF  350snda1.u52  ADSP boot + program (512 KB)
 *   0x400000..0x5FFFFF  snd0.u48      sample bank 0 (2 MB)
 *   0x600000..0x7FFFFF  snd1.u49      sample bank 1 (2 MB)
 *   0x800000..0x9FFFFF  snd2.u50      sample bank 2 (2 MB)
 *   0xA00000..0xBFFFFF  snd3.u51      sample bank 3 (2 MB) */
static const uint32 RAX_ROM_SIZE = 0x1000000;

/* SH-2 write address for sound communication (both master and slave).
 * MAME maps the full CS3 range 0x04000000-0x04FFFFFF to the RAX. */
static const uint32 RAX_COMM_ADDR = 0x04800000;

void RAX_Init(VirtualFS* vfs, const std::string& dir) MDFN_COLD;
void RAX_Reset() MDFN_COLD;

/* Called by the SH-2 read handler at 0x04000000.
 * A&2==0: status (bit 0 = adsp_snd_pf0: 1=ADSP idle/ready)
 * A&2==2: data_out_latch (ADSP response) */
void RAX_Read16(uint32 A, uint16* DB);

/* Called by the SH-2 write handler at 0x04800000.
 * MAME: batmanfr_sound_comms_w — only upper 16 bits carry sound data. */
void RAX_WriteCommand(uint16 data);

/* Called once per SCSP sample (~44100 Hz) to mix RAX audio into the output.
 * Currently produces silence; future HLE will mix decoded PCM here. */
void RAX_MixSample(int16* left, int16* right);

void RAX_StateAction(StateMem* sm, const unsigned load, const bool data_only) MDFN_COLD;
void RAX_Kill() MDFN_COLD;

}

#endif
