# Convert from .cpuprofile to Google trace format

You can use this two ways:

- use [this website](https://convert.matradomski.com/), it's the wasm version of this repo
- clone, compile the `convert.c` file like `clang -O2 convert.c -o convert` and use like `./convert <cpuprofile files to convert>`

Running it generates a file at the same path with the extension replaced by `_spall.json`.

This was written for use with [spall](https://github.com/colrdavidson/spall-web).
