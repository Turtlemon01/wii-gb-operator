AI Disclaimer:
I am not a software engineer and my only coding experience is 2 coding classes in school almost 4 years ago. I used Claude Code to support effectively all coding requirements while I managed decision making, direction, and troubleshooting. I also synthesized from many other projects that I was deliberate about crediting below. I am not affiliated with anyone credited nor do I, have I, or will I receive money for this project. I just like homebrew projects and hoped to contribute to the community. 

Description:
Wii GB Operator is a homebrew software designed to replicate the Epilogue’s GB Operator experience on the Wii. This project is very juvenile and is almost exclusively tested using Pokemon Games. It uses the GB Operator in the USB Port of the Wii to download roms and saves and then mGBA to play the games. I hope to use this project as a starting point for my end goal which is a Wii version of Pokemon Box that is expanded to include GB/C games and potentially even DS games. I decided to use the GB Operator since it can support GB/C titles. I do also hope to include the GC-GBA link cable to be able to trade/battle in adventure mode. If you’d like to help design borders for the games please let me know! It’s one of my favorite parts of playing Pokemon games on the big screen. Thanks!

How to use:
Your SD should have the following:

Borders (folder)

Gba (folder)

Gbc (folder)

Roms (folder)

Saves (folder)

Boot.dol (renamed from wii_gb_operator.dol)

Meta.xml

Settings.ini

Please make backups of all of your saves before using. This is a very untested program. Plug the gb operator into your wii and go to the homebrew channel. Boot up the wii gb operator program. Insert a GB/C/A cart into the GB Operator and hit play. If your cart doesn’t appear on the title you can select the manual detect cart swap option. When you hit play the rom and save data will download. The rom dump only occurs on the first attempt. Save dump occurs every attempt to allow you to play from your cart’s current progress. While playing GB/C games, saves do not sync to your cart automatically. After saving in game, press Z to bring up the pause menu and manually sync to cart or sync on exit. If you do not wish to save your progress, exit without syncing. GBA games sync to the cart automatically when a save is detected by the program. Saving in game triggers the sync to cart indicated by a square in the top right of the emulation. When the square is complete your save is synced to cart. The Z menu also controls the scale of the emulation on your display. The settings.ini file controls the default scale as well as the dev menu option which has some more features and is less “plug and play”. Borders placed in the border folder are examples that you can use as templates. The naming convention is border_XXXX with the x’s being the game code on your cart. For example, English fire red is BPRE. Save borders as a .bmp file. I used Aseprite and it worked well.

Border format: .bmp file, RGB color mode, 256x224 with 160x144 internal window (GBC), 320x240 with 240x160 internal window (GBA). Examples are posted.

Credits:
Claude AI - coded the project https://claude.ai/ 
Gbopyrator - used as foundation reference for building usb protocol https://github.com/N0ciple/gbopyrator
Epilogue - built GB Operator and playback software. Software usb codes were scanned to get handshakes needed to build the wii program https://www.epilogue.co/product/gb-operator?srsltid=AfmBOoqRyJEfX_3DIkXuI7ECNlJM8ezVSYYi24CniTjfDne1hwioBBey
Wireshark - captured USB traffic to understand handshakes with Epilogue Playback https://www.wireshark.org
PKHex - save editor used to validate save dumping https://github.com/kwsch/PKHeX
Gba-link-cable-dumper - consulted for understanding GBA ROM layouts and transfer https://github.com/FIX94/gba-link-cable-dumper 
Various sources to get info on ROMs - No-Intro https://www.no-intro.org, libretro-database, https://github.com/libretro/libretro-database
DevKitPro + libogc + libfat - used to build homebrew software https://devkitpro.org
mGBA - emulator used for the GB Operator style experience https://github.com/mgba-emu/mgba 
DogToon64 and FrenchOrange +Nintendo/GameFreak on The Spriters Resource - used to make example FireRed, Ruby and Gold/Silver borders for emulation https://www.spriters-resource.com/game_boy_advance/pokemonrubysapphire/asset/153230/ https://www.spriters-resource.com/game_boy_advance/pokemonfireredleafgreen/asset/3850/ 
https://www.spriters-resource.com/game_boy_gbc/pokemongoldsilver/asset/9098/ 
