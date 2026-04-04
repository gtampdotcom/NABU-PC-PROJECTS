NABU CP/M TNFS client v0.3 by GTAMP (c) 2026
Trivial/Tiny/Taco Network File System, as used by SpectraNet, FujiNet and now RetroNet

I had never owned or used a FujiNet device or TNFS before creating this for NABU
I knew nothing about the TNFS protocol but I knew FujiNet had a way to download files.
I had already been making NABU and RetroNet software, so making a TNFS client seemed like a good next step.
Shoutout to the NABU, retro and FujiNet communities.

TNFS can launch .com files under Cloud CPM GUI using BDOS function 59 (P_LOAD)
Auto launching is currently not supported on Cloud CPM Text.

TNFS uses the $$$.SUB method to launch .com files on every other CP/M
It tries to append to the end if it already exists. It can fail if there are too many files or it can't write.

It uses direct hardware access for keyboard/joystick but you can force it to use the CPM BIOS with /t command line.

Changelog:

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
