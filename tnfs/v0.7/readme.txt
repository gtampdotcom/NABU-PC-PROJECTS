NABU PC CP/M TNFS client v0.7 by GTAMP (c) 2026
Trivial/Tiny/Taco Network File System, as used by SpectraNet, FujiNet and now RetroNet
https://github.com/FujiNetWIFI/tnfsd/blob/master/tnfs-protocol.md

This a client for downloading and launching files from TNFS servers.
TNFS servers are file servers that are commonly used by FujiNet for serving files to retro computers.

Connect to the existing servers or host your own server https://github.com/FujiNetWIFI/tnfsd/releases

You'll have to edit tnfs.txt to add new servers

I put 127.0.0.1 in there because you can run a TNFS server on the same PC as your NABU Adapter software.

Only the first 2 servers currently have NABU files:

tnfs.amigaretro.com
fujinet.skdev.org

Thanks to all the cool people in the NABU, retro and FujiNet communities!

TNFS launches .com files under Cloud CPM GUI using BDOS function 59 (P_LOAD)
TNFS uses the $$$.SUB method to launch .com files on every other CP/M

TNFS /t command line if you want to use a remote terminal or BIOS input instead of raw input

If launching files just errors with a read error or quits silently, it usually means your CPM has hit the file limit. Delete some files and try again.

If you want tnfs.com to run at startup, put extra\tnfs.sub on your CPM boot drive and run submit tnfs

Changelog:

2026-04-25 v0.7
- Launching now works in the updated Cloud Text
- VRAM is cleared before launch
- Switched from inp() to __sfr to save a few bytes

2026-04-11 v0.6
- Temporary files are saved to current drive instead of A:
- Browsing position is restored when reconnecting to a server

2026-04-06 v0.5
- First file on a page switch is no longer missing
- Displayed file list increased from 20 to 21
- Legacy OPENDIR/READDIR and sort code removed

2026-04-06 v0.4
- Keyboard flushed on exit to stop $$$.SUB from quitting instantly
- Changed help to "Joystick/Arrows/GO/Q"
- WASD only works in /t mode for now

2026-04-04 v0.3
- Run .com files after download
- Added joystick support
- TNFS protocol: Uses READDIRX and OPENDIRX to sort and detect directories
- Reduced file size from 36,248 to 27,312 bytes (still bloated)

2026-04-01 v0.2
- Directories containing . correctly show up as directories
- D11:file.txt to save to custom drive user
- Last server selected is saved and restored from TNFS.INI

2026-03-31 v0.1
- First release. It can only download.

https://github.com/gtampdotcom/NABU-PC-PROJECTS
https://gtamp.com/nabu
