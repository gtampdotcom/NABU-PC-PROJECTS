NABU VGMP by GTAMP (c) 2026
---------------------------

VGM (video game music) player for the Z80 NABU PC

Usage:

VGMP (look for *.VGM in current location)
VGMP A B (look for *.VGM on drive user 0-F)
VGMP DOOM.VGM (play DOOM.VGM on current drive)
VGMP A B DOOM.VGM (play DOOM.VGM on destination drive and user)

It will autoplay *.VGM files

Controls:
< previous
> next
+ increase speed
- decrease speed
L loop
P pause
Q quit

It can play uncompressed VGM files for AY-3-8910 and YM2149 chips. Other chips will not work.

If you use Cloud CP/M, you can find VGM files on A11:

Most VGM files are compressed as .vgz files. You need to uncompress them. 7-Zip works fine with gzip files.

https://vgmrips.net/packs/chip/AY-3-8910
https://vgmrips.net/packs/chip/YM2149
https://vgmrips.net/forum/viewtopic.php?t=496&start=75 4GB packs with many chips, not just supported chips

It sets terminal type with this hack:

VT100 if 0x03 CP/M IO byte is 01, 93, 94 or 9B. All other cases it uses ADM-3A.


Links:

https://gtamp.com/nabu
https://github.com/gtampdotcom
https://nabu.ca