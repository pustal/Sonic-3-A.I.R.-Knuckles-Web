# Third-party notices

| Component | Where | Licence |
|---|---|---|
| [Sonic 3 A.I.R.](https://github.com/Eukaryot/sonic3air) by Eukaryot: the game, the Oxygen engine, lemonscript and librmx | `external/sonic3air`, compiled into the app; its scripts and data in the app's `game/` folder | GPL-3.0 (`licenses/Sonic3AIR-license.txt` in the IPK) |
| [libogg and libvorbis](https://xiph.org/) by Xiph.Org | built from `external/sonic3air/framework/external/ogg-vorbis` | BSD-3-Clause (`licenses/libogg_COPYING.txt`, `licenses/libvorbis_COPYING.txt`) |
| [minizip](https://github.com/madler/zlib/tree/develop/contrib/minizip) by Gilles Vollant | built from `external/sonic3air/framework/external/zlib` (zip mods, save backups) | zlib (`licenses/zlib_README.txt`) |
| [JsonCpp](https://github.com/open-source-parsers/jsoncpp) | part of librmx (`external/sonic3air/librmx`) | MIT or public domain (`licenses/jsoncpp_LICENSE.txt`) |
| [GNU FreeFont](https://www.gnu.org/software/freefont/) | glyphs in the game's own fonts (`game/data/font`) | GPL-3.0 with font exception (`licenses/freefont_COPYING.txt`) |
| xBRZ and hqx upscaler shaders | the game's video filters (`game/data/shader`) | see `licenses/shader/` |
| [SDL-webOS](https://github.com/webosbrew/SDL-webOS) 2.30.12 | bundled as `lib/libSDL2-2.0.so.0` | zlib (`licenses/SDL2.txt`) |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.63.0 by yhirose | the transfer page's server (`third_party/cpp-httplib/`) | MIT (`licenses/cpp-httplib.txt`) |
| [font8x8](https://github.com/dhepper/font8x8) by Daniel Hepper | the setup screen, the menu and the icon lettering (`src/port/BitmapFont.cpp`) | Public domain |

The game's libcurl (for its own downloads) is not part of the IPK: the app uses
the TV's libcurl when there is one. zlib is the TV's own.

This project contains no SEGA game data. The Sonic 3 & Knuckles ROM, and the
remastered soundtrack package, are never part of this repository or the IPK:
you supply your own. Sonic the Hedgehog is a trademark of SEGA; this project is
not affiliated with or endorsed by SEGA, and not made by Eukaryot (though it
builds on his work, as the GPL allows).
