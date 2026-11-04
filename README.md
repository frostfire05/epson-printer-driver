# EPSON L130 Printer Driver (C++)

A user-space printer driver for the **EPSON L130** inkjet printer, written in C++17 for Linux.
It talks to the printer directly over USB using the ESC/P2 raster command set. When no printer
is connected it still works: you can **save jobs as PDF** or run a **software simulation** of
the printer with a priority-based print queue.

> **Status:** the PDF export and simulation modes are tested. Printing on real L130 hardware
> has **not** been tested yet; the ESC/P2 command sequence is implemented from the protocol
> and may need adjustment on a physical printer.

---

## Features

- **Direct USB printing** via `libusb-1.0`, with no CUPS required
- **ESC/P2 raster encoding**: job/page headers, PackBits run-length compression
- **Colour pipeline**: RGB → CMYK separation and Floyd–Steinberg dithering
- **Print any file**: PNG, JPG, BMP, GIF, TGA, PSD, PPM/PGM and **PDF** (page ranges supported)
- **Fit to page**: images are scaled to the A4 printable area, keeping their aspect ratio
- **No-printer fallback**, chosen at runtime:
  - **Save as PDF**: each job is written to a compressed PDF in `pdf_output/`
  - **Simulation**: the printer is emulated in software with live progress
- **Priority print spooler**: Low / Normal / High / Critical; ties run first-come-first-served;
  jobs can be listed and cancelled
- **Typed error handling**: every failure has its own exception class with a clear,
  actionable message (see [Error handling](#error-handling))

---

## Requirements

Tested on **Ubuntu 24.04** with g++ 13. Any Linux with a C++17 compiler (g++ 7 or newer) should work.

```bash
sudo apt update
sudo apt install build-essential libusb-1.0-0-dev
sudo apt install poppler-utils      # only needed for printing PDF files
```

The image decoder ([stb_image](https://github.com/nothings/stb)) is bundled, so you don't need to install it.

---

## Build and run

```bash
git clone https://github.com/frostfire05/EPSON-Printer-Driver.git
cd EPSON-Printer-Driver
make
./epson_demo
```

`make clean` removes the build output and the `pdf_output/` folder.

---

## Usage

### With the printer connected

The program connects over USB and shows the device ID, status and ink levels. It then asks for a file:

```
File to print (Enter = test pages): ~/Pictures/photo.jpg
```

Press **Enter** without a file to print the built-in colour and greyscale test pages.

### Without a printer

If the printer isn't found, you choose a fallback:

```
⚠  EPSON L130 not connected.
  1) Save as PDF  — write each job to a PDF file in: ./pdf_output
  2) Simulation   — emulate the printer in software with a live priority queue
  q) Quit
```

Then add as many jobs as you like before processing:

```
  1) Colour HSV sweep          4) Solid blue block
  2) Greyscale ramp            5) Solid green block
  3) Solid red block           6) Print a file  (PNG, JPG, BMP, GIF, PDF …)
  d) Done — start processing   s) Show queue   c) Cancel a job   q) Quit
```

**To print your own file**, choose `6`:

1. Enter the path. The easiest way is to **drag the file into the terminal window**.
   `~/…` paths and names with spaces both work.
2. For a PDF, enter a page range (`2-5`, `3`, or Enter for all pages).
3. Choose colour or greyscale, a label and a priority.

Choose `d` to process the queue. Jobs run highest-priority first. Each page of a PDF becomes its own job.
Press **Ctrl+C** to stop after the current job.

---

## USB permissions

By default only root can access USB printers. Either run with `sudo`, or add a udev rule once:

```bash
echo 'ATTRS{idVendor}=="04b8", ATTRS{idProduct}=="007c", MODE="0666", GROUP="plugdev"' \
  | sudo tee /etc/udev/rules.d/99-epson-l130.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Then unplug and replug the printer.

If the program says the device is busy, CUPS or the kernel `usblp` module is holding it:

```bash
sudo systemctl stop cups      # or
sudo rmmod usblp
```

---

## Project structure

| File | Purpose |
|------|---------|
| `epson_l130_driver.hpp/.cpp` | Core driver: USB transport, ESC/P2 encoder, colour separation, dithering, print jobs |
| `print_spooler.hpp/.cpp` | Priority spooler, printer simulator, dependency-free PDF exporter, no-printer fallback menu |
| `image_loader.hpp/.cpp` | Loads image and PDF files into print-ready pages and scales them to fit A4 |
| `driver_errors.hpp` | Typed exception hierarchy used throughout |
| `main_demo.cpp` | Interactive command-line program |
| `stb_image.h`, `stb_image_write.h` | Bundled third-party image decoding and zlib compression (public domain) |
| `Makefile` | Build script |

### How a page gets printed

```
file ──► image_loader ──► RGB pixels ──► ColorProfile ──► C/M/Y/K planes
          (decode, fit to A4)              (separate + dither)
                                                │
                     EscP2Encoder ◄─────────────┘
          (1-bit raster lines, PackBits compression, ESC/P2 commands)
                          │
          ┌───────────────┼────────────────┐
          ▼               ▼                ▼
     UsbTransport   PrinterSimulator   PdfExporter
     (real printer)  (no printer)       (no printer)
```

---

## Error handling

All exceptions derive from `DriverException`, so callers can catch broadly or precisely:

```
DriverException
 ├── ConnectionError   DeviceNotFound, DeviceBusy, PermissionDenied,
 │                     KernelDriverConflict, UsbTransferError, UsbException
 ├── PrintError        InvalidSettings, PageTooLarge, EmptyJob, EncodingError,
 │                     FileLoadError, PrintException
 ├── SpoolerError      QueueFull, DuplicateJobId, JobNotFound
 ├── SimulationError   SimulationAborted
 ├── PdfExportError    FileCreateFailed, PdfWriteError
 └── HardwareError     PaperJam, OutOfPaper, InkLow, PrinterOffline
```

Each message says what went wrong and how to fix it. For example, `PermissionDenied`
includes the exact udev rule to install.

---

## Hardware reference

| | |
|---|---|
| USB vendor / product ID | `0x04B8` (Seiko Epson) / `0x007C` (L130 Series), `0x08A1` |
| Endpoints | bulk OUT `0x01`, bulk IN `0x82` |
| Resolutions | 180 (draft), 360 (normal), 720 (high) dpi |
| Paper | A4 (Letter, Legal and B5 defined) |

---

## Limitations

- Not yet tested on a physical EPSON L130 (see Status above).
- Colour conversion is a simple RGB→CMYK formula, not an ICC colour profile.
- Saving a multi-page PDF input produces one output PDF per page.
- Linux only, because it uses libusb and POSIX process APIs.

## Acknowledgements

Image decoding and zlib compression use [stb_image and stb_image_write](https://github.com/nothings/stb)
by Sean Barrett (public domain / MIT).

## Author

Vibhan Dutta, Computer Science & Engineering, Jadavpur University
