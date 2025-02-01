# Convert from .cpuprofile to Spall binary or Google trace format

You can use this two ways:

- use [this website](https://convert.matradomski.com/), it's the wasm version of this repo
- clone, compile the `xform.c` file like `clang -O2 xform.c -o xform` and use like `./xform <cpuprofile files to xform>` or `./xform <directory with .cpuprofile files>`
- You can pass `-d` as an argument in the CLI to remove xform `.cpuprofile` files.

Running it generates a file at the same path with the extension replaced by either

- `.spall` if using Spall binary format
- `_gtrace.json` if using GTrace

This was written for use with [spall](https://github.com/colrdavidson/spall-web).
