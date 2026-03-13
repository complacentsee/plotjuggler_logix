# PlotJuggler Logix (CIP 0xB2)

A native C++ [PlotJuggler](https://github.com/facontidavide/PlotJuggler) DataStreamer plugin for trending tags from Rockwell ControlLogix and CompactLogix PLCs via EtherNet/IP.

> **Status:** Work in progress — not yet production ready.

## How It Works

The plugin uses CIP Trend Objects (class 0xB2) to sample PLC tags at a user-specified rate directly on the controller. This provides precise, deterministic-interval sampling independent of network polling jitter.

1. Connects to the PLC via EtherNet/IP (TCP port 44818)
2. Browses controller and program tags via CIP
3. Presents tags in a tree dialog for selection
4. Creates CIP trend instances on the PLC for each selected tag
5. Polls trend buffers and streams data into PlotJuggler

## Features

- Tag browsing with filtering, program grouping, and struct member expansion
- Configurable sample rates from 1 ms to 1 s (or custom)
- Supports all numeric CIP data types (BOOL, SINT, INT, DINT, LINT, REAL, LREAL, etc.)
- Optional CIP routing for reaching PLCs through bridges/backplanes
- Automatic buffer sizing and poll interval tuning to prevent data loss
- XML state save/restore for PlotJuggler layouts

## Building

### Prerequisites

- C++17 compiler
- Qt5 (Core, Widgets, Xml)
- PlotJuggler 3.16+ (built from source)

### Build Steps

```bash
mkdir build && cd build
cmake .. -DPLOTJUGGLER_SRC_DIR=/path/to/PlotJuggler
make
```

### Install

Copy the built plugin to PlotJuggler's plugin directory:

```bash
cp libDataStreamLogixTrend.dylib /path/to/PlotJuggler/build/bin/
```

On macOS, you may need to ad-hoc codesign after copying:

```bash
codesign -s - /path/to/PlotJuggler/build/bin/libDataStreamLogixTrend.dylib
```

## CIP Route Format

The optional CIP route field accepts comma-separated port/link pairs:

| Route | Meaning |
|-------|---------|
| *(empty)* | Direct connection (CompactLogix or local) |
| `1,0` | Backplane port 1, slot 0 |
| `1,4,2,10.10.10.9` | Backplane slot 4, then Ethernet to 10.10.10.9 |

## License

TBD
