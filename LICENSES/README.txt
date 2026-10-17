Third-party font licenses
=========================

The fonts in resources/fonts/ are bundled unmodified under permissive
open-source licenses. The full license text for each is in this folder.

Font files                          License        License file              Source
----------------------------------  -------------  ------------------------  ------
Inter-Regular/Medium/SemiBold       SIL OFL 1.1    Inter-OFL.txt             https://github.com/rsms/inter
JetBrainsMono-Regular/Medium        SIL OFL 1.1    JetBrainsMono-OFL.txt     https://github.com/JetBrains/JetBrainsMono
SpaceGrotesk-Medium                 SIL OFL 1.1    SpaceGrotesk-OFL.txt      https://github.com/floriankarsten/space-grotesk
tabler-icons                        MIT            TablerIcons-LICENSE.txt   https://github.com/tabler/tabler-icons
NotoSansJP-Regular/Medium           SIL OFL 1.1    NotoSansJP-OFL.txt        https://github.com/notofonts/noto-cjk
NotoSansSC-Regular/Medium           SIL OFL 1.1    NotoSansSC-OFL.txt        https://github.com/notofonts/noto-cjk
NotoSansKR-Regular/Medium           SIL OFL 1.1    NotoSansKR-OFL.txt        https://github.com/notofonts/noto-cjk

Notes
-----
- All fonts are shipped unmodified, so the SIL OFL "Reserved Font Name" clause
  imposes no extra obligation (it only restricts reusing the original name on a
  MODIFIED version of the font).
- The OFL forbids selling the fonts on their own; bundling them in this engine
  or in applications built with it is explicitly permitted.
- MIT requires only that the copyright notice and license text (this folder)
  accompany the distribution.

Third-party libraries
=====================

Fetched at configure time (CMake FetchContent) and compiled into the editor,
its DLL and the package DLLs; a packaged editor ships them in binary form, so
their license texts are here too.

Library          License                 License file                      Source
---------------  ----------------------  --------------------------------  ------
Dear ImGui       MIT                     ImGui-LICENSE.txt                 https://github.com/ocornut/imgui (docking branch)
SDL3             zlib                    SDL3-LICENSE.txt                  https://github.com/libsdl-org/SDL
FreeType         FreeType License (FTL)  FreeType-FTL.txt                  https://freetype.org
stb              MIT / public domain     stb-LICENSE.txt                   https://github.com/nothings/stb
cpp-httplib      MIT                     cpp-httplib-LICENSE.txt           https://github.com/yhirose/cpp-httplib
efsw             MIT                     efsw-LICENSE.txt                  https://github.com/SpartanJ/efsw
googletest       BSD-3-Clause            googletest-LICENSE.txt            https://github.com/google/googletest (tests only, not shipped)
nlohmann/json    MIT                     (deki-engine/NOTICE)              https://github.com/nlohmann/json (vendored in the engine)
doxygen-awesome  MIT                     doxygen-awesome-css-LICENSE.txt   https://github.com/jothepro/doxygen-awesome-css (docs theme only)

The FreeType License requires a credit in the documentation of a product that
uses FreeType: "Portions of this software are copyright © The FreeType Project
(www.freetype.org). All rights reserved." - this file is that credit.
