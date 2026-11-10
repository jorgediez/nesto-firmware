# Third-party software

The Nesto program built from this repository includes the following, each
under its own licence. All are fetched at build time, pinned by version and
checksum (see `cmake/Dependencies.cmake` and `docker/Dockerfile`).

| Component | Version | Licence | What it's for |
|---|---|---|---|
| [LVGL](https://github.com/lvgl/lvgl) | 9.6.0 | MIT | Graphics and widgets; its tiny_ttf module draws the fonts, using stb_truetype (public domain or MIT) |
| [Monocypher](https://github.com/LoupVaillant/Monocypher) | 4.0.3 | BSD-2-Clause or CC0-1.0 | X25519, ChaCha20-Poly1305, SHA-512, HMAC and HKDF |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT | The protocol's JSON messages |
| [mdns](https://github.com/mjansson/mdns) | 1.4.3 | Public domain (Unlicense) | mDNS / DNS-SD responder |
| [Montserrat](https://github.com/JulietaUla/Montserrat) | 9.000 (Medium) | SIL Open Font License 1.1 | Text. Embedded as a subset (Latin-1 and Latin Extended-A); the font's copyright and licence names are kept in it |
| [Material Design Icons](https://github.com/Templarian/MaterialDesign-Webfont) | 7.4.47 | Apache License 2.0 | Icons. Embedded as a subset of the icons the panel uses |
| [musl](https://musl.libc.org) | 1.2.6 | MIT | The C library, linked statically into the thermostat's program |
| GCC runtime libraries (libstdc++, libgcc) | 15.3 | GPL-3.0 with the GCC Runtime Library Exception 3.1 | C++ runtime, linked statically into the thermostat's program; the exception allows this under any licence |

Only for building and testing, not included in the program:

| Component | Licence |
|---|---|
| [Bootlin toolchains](https://toolchains.bootlin.com) (GCC, binutils) | GPL and others |
| [doctest](https://github.com/doctest/doctest) | MIT |
| [SDL2](https://www.libsdl.org), for the simulator's window | zlib |
| Python's [cryptography](https://cryptography.io), [fontTools](https://github.com/fonttools/fonttools), [python-zeroconf](https://github.com/python-zeroconf/python-zeroconf), [pytest](https://pytest.org) | Apache-2.0 / BSD, MIT, LGPL-2.1, MIT |

When Nesto is distributed in built form, for instance by its installer, these
notices and the licences they name go with it.
