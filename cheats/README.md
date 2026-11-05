# Cheats

RetroArch cheat files for the ST-V games of the MAME cheat collection (Pugsy's `cheat.7z`,
version 0.279, July 2025, https://www.mamecheat.co.uk), converted with `tools/mame2cht.py`.
Credit for the cheats themselves goes to the collection's contributors (see its `cheat.txt`).

Install: copy the `Mednafen STV` folder into RetroArch's cheat directory (Settings > Directory >
Cheat Files; `/recalbox/share_init/cheats/` on Recalbox). RetroArch then loads
`<cheat dir>/Mednafen STV/<rom name>.cht` by itself when the game starts; enable the entries in
Quick Menu > Cheats and apply. Entries named "... Now!" are one-shot: they fire once each time
they are enabled.

Regenerate after extracting `cheat.7z`:

    for x in cotton2 vfkids ...; do tools/mame2cht.py "$x.xml" > "cheats/Mednafen STV/$x.cht"; done
