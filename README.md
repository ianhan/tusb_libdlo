# tusb_libdlo

![Pico TV typewriter demo](docs/pico_tv_typewriter_side_by_side.gif)

This is the libdlo DisplayLink library with some optimizations, ported to TinyUSB - I split it out from PicoGraph after some interest. libdlo is useful on microcontrollers because it allows them to support very high resolution devices without spending much memory on the microcontroller - the framebuffer is on the other side, and it supports accelerated blits and fills. This library supports DisplayLink DL-1x0/DL-1x5 USB adapters.

## Layout

```text
.
├── docs/                 # Original libdlo documentation
├── examples/             # Firmware examples
└── src/                  # tusb_libdlo library target and public headers
```

## Using the Library

`tusb_libdlo` links against the Pico SDK `tinyusb_host` target and enables the TinyUSB endpoint-transfer API required by the port.

## Examples

I included some examples, because - why not. The repository includes two Pico 1 examples:

`pico_gc_mystify` enumerates all DisplayLink devices and initializes all of them, then displays a screensaver.

`pico_tv_typewriter` is a dumb tv typewriter, but the pico W build supports telnet over WIFI (that's the aquarium demonstration on telehack in the gif).

### Root Build

```sh
git submodule update --init --recursive
cmake -S . -B build -DPICO_BOARD=pico
cmake --build build --target pico_tv_typewriter
cmake --build build --target pico_gc_mystify
```

The UF2s will be written under `build/examples/`. Set `TUSB_LIBDLO_BUILD_EXAMPLES=OFF` to configure the library target only.

## Hardware Notes

The examples expect the Pico USB port to run as a TinyUSB host. Use a powered USB hub or equivalent host wiring. The typewriter needs a DisplayLink adapter and a USB keyboard; the mystify example only needs the DisplayLink adapters.

## Default Art

The checked-in ANSI art was generated from Wikimedia Commons `Raspberry Pi Pico oblique.jpg` by Phiarc, licensed CC BY-SA 4.0.

## License

This repository inherits Picograph's repository-level GNU GPL version 2 license for its packaging, examples, and other code without a more specific file-level license; see `LICENSE`.

The libdlo sources retain DisplayLink's original GNU Library GPL version 2 notices; see `COPYING` and `AUTHORS`. Flanterm and TinyUSB are pulled as submodules under their upstream licenses. The generated Pico ANSI art is a transformed version of the CC BY-SA 4.0 Wikimedia Commons photo described above.
