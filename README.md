# PlotJuggler Logix Trend

[![CI](https://github.com/complacentsee/plotjuggler_logix/actions/workflows/ci.yml/badge.svg)](https://github.com/complacentsee/plotjuggler_logix/actions/workflows/ci.yml)

A native C++ [PlotJuggler](https://github.com/facontidavide/PlotJuggler) DataStreamer plugin for trending tags from Rockwell ControlLogix and CompactLogix PLCs via EtherNet/IP.

> **Status:** Work in progress — functional but not yet production ready.

## How It Works

The plugin uses CIP Trend Objects (class 0xB2) to sample PLC tags at a user-specified rate directly on the controller. This provides precise, deterministic-interval sampling independent of network polling jitter.

1. Connects to the PLC via EtherNet/IP (TCP port 44818)
2. Browses controller and program tags via CIP
3. Presents tags in a tree dialog for selection (with PLC RAM estimate)
4. Creates CIP trend instances on the PLC for each selected tag
5. Polls trend buffers and streams data into PlotJuggler

## Features

- Tag browsing with filtering, program grouping, and struct/UDT member expansion
- Configurable sample rates from 1 ms to 1 s (or custom)
- Supports all numeric CIP data types (BOOL, SINT, INT, DINT, LINT, REAL, LREAL, etc.)
- Up to 32 tags trended simultaneously
- Optional CIP routing for reaching PLCs through bridges/backplanes
- PLC memory usage estimate with 50 KB warning threshold
- Large Forward Open (4002 bytes) with automatic fallback to standard (504 bytes)
- XML state save/restore for PlotJuggler layouts

## Installation

### Pre-built Binaries

Download the shared library for your platform from the [Releases](https://github.com/complacentsee/plotjuggler_logix/releases) page and copy it to your PlotJuggler plugin directory.

On macOS, remove the quarantine attribute and ad-hoc codesign after downloading:

```bash
xattr -d com.apple.quarantine libDataStreamLogixTrend.dylib
codesign --force --sign - libDataStreamLogixTrend.dylib
```

### Building from Source

**Prerequisites:** C++17 compiler, Qt5 (Core, Widgets, Xml), PlotJuggler 3.16+ source

```bash
# Clone PlotJuggler and build the base libraries
git clone --depth 1 --branch 3.16.0 https://github.com/facontidavide/PlotJuggler.git
cmake -S PlotJuggler -B PlotJuggler/build -DCMAKE_BUILD_TYPE=Release
cmake --build PlotJuggler/build --target plotjuggler_base plotjuggler_qwt fmt

# Build the plugin
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPLOTJUGGLER_SRC_DIR=$(pwd)/PlotJuggler
cmake --build build
```

## CIP Route Format

The optional CIP route field accepts comma-separated port/link pairs:

| Route | Meaning |
|-------|---------|
| *(empty)* | Direct connection (CompactLogix or local) |
| `1,0` | Backplane port 1, slot 0 |
| `1,4,2,10.10.10.9` | Backplane slot 4, then Ethernet to 10.10.10.9 |

## Known Limitations

- Maximum 32 concurrent trend instances per PLC connection
- One tag per trend instance (PLC firmware limitation in high-speed mode)
- Target PLCs with limited RAM (1 MB) should monitor the memory estimate in the config dialog

## License

[Mozilla Public License 2.0](https://mozilla.org/MPL/2.0/)
