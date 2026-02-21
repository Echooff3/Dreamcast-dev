# Dreamcast Dev — Hello Dreamcast! Demo

A minimal **KallistiOS** homebrew demo for the Sega Dreamcast that demonstrates
PVR graphics, controller input, and color cycling — all built inside a
zero-install **Dev Container**.

---

## Overview

| Feature | Detail |
|---------|--------|
| Graphics | PowerVR (PVR) hardware-accelerated rendering via KOS |
| Input | Maple-bus controller polling |
| A button | Cycles background colour (black → blue → red → green → purple → cyan) |
| Start button | Exits the demo |
| Text | "Hello Dreamcast!" rendered with the built-in `bfont` API |

---

## Prerequisites

You need **one** of the following:

- [GitHub Codespaces](https://github.com/features/codespaces) (recommended — zero local install)
- [VS Code](https://code.visualstudio.com/) +
  [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) +
  [Docker Desktop](https://www.docker.com/products/docker-desktop/)

Both options use the pre-built
[`einsteinx2/dcdev-kos-toolchain:gcc-9`](https://hub.docker.com/r/einsteinx2/dcdev-kos-toolchain)
image which already contains the complete SH4 cross-compiler and KallistiOS.

---

## Opening in a Dev Container

### GitHub Codespaces

1. Click **Code → Codespaces → Create codespace on `main`** (or your branch).
2. Wait for the container to build (≈ 2 min on first launch).
3. A VS Code editor opens inside the container — the toolchain is ready.

### VS Code + Docker (local)

1. Clone this repo and open it in VS Code.
2. Install the **Dev Containers** extension if you haven't already.
3. Press `Ctrl+Shift+P` → **Dev Containers: Reopen in Container**.
4. VS Code rebuilds the container and reopens the project inside it.

---

## Building

Inside the Dev Container terminal:

```bash
# Source the KOS environment (only needed once per shell session)
source /opt/toolchains/dc/kos/environ.sh

# Build
make
```

A successful build produces **`dreamcast_demo.elf`**.

To clean build artifacts:

```bash
make clean
```

---

## Creating a Bootable `.cdi` Image

The `.elf` must be converted to a `.cdi` (DiscJuggler) image to run on real
hardware or most emulators.

```bash
# 1. Scramble the ELF into a raw binary
$KOS_BASE/utils/scramble/scramble dreamcast_demo.elf 1ST_READ.BIN

# 2. Build an ISO 9660 image with the IP.BIN bootstrap
mkisofs -C 0,11702 -V DREAMCAST_DEMO -G /opt/toolchains/dc/ip.bin \
        -joliet -rock -l -x "*.BIN" -o dreamcast_demo.iso \
        1ST_READ.BIN

# 3. Convert to CDI
cdi4dc dreamcast_demo.iso dreamcast_demo.cdi
```

> **Tip:** `cdi4dc` and `mkisofs` are included in the Dev Container image.

---

## Testing in an Emulator

### Flycast (recommended)

1. Download [Flycast](https://github.com/flyinghead/flycast/releases).
2. Supply a Dreamcast BIOS (place `dc_boot.bin` & `dc_flash.bin` in the
   Flycast data directory).
3. Open **File → Open…** and select `dreamcast_demo.cdi`.

### Redream

1. Download [Redream](https://redream.io/).
2. Open the `.cdi` file directly from **File → Open Game**.

---

## Controller Mapping

| Dreamcast Button | Action |
|------------------|--------|
| **A** | Cycle background colour |
| **Start** | Exit the demo |

---

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `KOS_BASE` not set | Run `source /opt/toolchains/dc/kos/environ.sh` |
| `make: command not found` | Reopen in Dev Container — the image includes GNU make |
| Black screen in emulator | Ensure you are loading the `.cdi`, not the raw `.elf` |
| No controller input | Check that Flycast/Redream has a controller mapped to Port A |
| Codespace build slow | First launch pulls the Docker image (~2 GB); subsequent starts are fast |

---

## Project Structure

```
.
├── .devcontainer/
│   └── devcontainer.json   # Dev Container configuration
├── romdisk/                # Empty romdisk directory (no assets needed)
├── main.c                  # Demo source code
├── Makefile                # KOS build rules
└── README.md               # This file
```

---

## License

This project is released into the public domain. Do whatever you like with it.
