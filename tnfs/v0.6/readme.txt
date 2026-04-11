NABU CP/M TNFS client v0.6 by GTAMP (c) 2026
Trivial/Tiny/Taco Network File System, as used by SpectraNet, FujiNet and now RetroNet

I had never owned or used a FujiNet device or TNFS before creating this for NABU
I knew nothing about the TNFS protocol but I knew FujiNet had a way to download files.
I had already been making NABU and RetroNet software, so making a TNFS client seemed like a good next step.
Shoutout to the NABU, retro and FujiNet communities.

TNFS can launch .com files under Cloud CPM GUI using BDOS function 59 (P_LOAD)
Auto launching is currently not supported on Cloud CPM Text.
TNFS uses the $$$.SUB method to launch .com files on every other CP/M

TNFS /t command line if you want to use a remote terminal or BIOS input instead of raw input

Changelog:

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
