# AIR

This repository contains tools and libraries for building AIR platforms,
runtimes and compilers.

Basic directory layout:

```
air
├── cmake                     CMake files
├── docs                      Additional documentation
├── examples                  Example code
├── mlir                      MLIR dialects and passes
├── platforms                 Hardware platforms
│   └── xilinx_vck190_air
├── pynq                      Board repo for building Pynq images
│   └── vck190_air
├── python                    Python libraries and bindings
├── runtime_lib               Runtime libraries for host and controllers
├── test                      In hardware tests of AIR components
├── tools                     aircc.py, air-opt, air-translate
└── utils                     Utility scripts
```

## Documentation

### MLIR Compiler
- [AIR Dialect](docs/generated/AIRDialect.md)
- [AIRRt Dialect](docs/generated/AIRRtDialect.md)
- [AIR Transform Passes](docs/generated/AIRTransformPasses.md)
- [AIR Conversion Passes](docs/generated/AIRConversionPasses.md)

### [Examples]() -- TODO: how to run examples and/or tests
### [Testing](docs/testing.md)
### VCK190 Platform
#### [Building the VCK190 ES1 AIR platform](docs/vck190_building_platform.md)
#### [Building Pynq based SD card image for the VCK190 AIR platform](docs/vck190_building_pynq.md)
#### [MicroBlaze firmware](docs/vck190_microblaze_firmware.md)
#### [Building the VCK190 Production AIR platform](docs/vck190_production_building_platform.md)

-----

<p align="center">Copyright&copy; 2019-2022 Advanced Micro Devices, Inc.</p>
