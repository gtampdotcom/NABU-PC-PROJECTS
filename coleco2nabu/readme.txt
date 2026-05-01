This program tries to convert ColecoVision roms to NABU PC

It won't work on all games and many games will have bugs. It can only accept roms under 32KB.

coleco2nabu v0.2 by GTAMP (c) 2026
based on Coleco Loader by Brian Johnson

Usage: coleco2nabu <roms> [-2] [-nopatch] [-swapjoy]

You can either run it from the command line or drag the Coleco roms that you want to patch onto coleco2nabu.exe

Wildcards are accepted from the command line ex: coleco2nabu *.rom or *.col

Use -2 for 2 player games like Joust
Use -nopatch for games that purely use the BIOS like Donkey Kong
Use -swapjoy for games that use the 2nd joystick for player 1 like Dig Dug

You don't need those command lines for those specific games because they are already listed in patches.z80

Existing patches are read from patches.z80, if no crc16 matches then automatic patches are attempted.

It will create a .com and a .nabu file.

https://gtamp.com/nabu
https://github.com/gtampdotcom/NABU-PC-PROJECTS
https://github.com/brijohn/nabupc/blob/master/games/nabu_coleco/

