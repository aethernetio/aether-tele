# ESP-IDF measurement (not run unless IDF_PATH is set)

This directory does not invent ESP32 numbers. On a machine with ESP-IDF:

```
export IDF_PATH=...
idf.py -C tests/space_standalone/esp32 set-target esp32
idf.py -C tests/space_standalone/esp32 build
idf.py size
```

The component links `aether-tele` the same way as `space_fp_one_space` /
`space_fp_two_spaces`. Record `.text` / `.rodata` / `.dram` from `idf.py size`
into FOOTPRINT.md only after a real build.
