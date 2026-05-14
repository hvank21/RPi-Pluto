# Pluto_RPi4

## Overview

This project contains a set of C++ utilities for:

- Recording dual-channel IQ data from ADALM-Pluto SDR;
- Saving long-duration binary recordings to SSD;
- Converting binary IQ recordings into TXT format;
- Building averaged FFT spectra from recorded files;
- Interactive spectrum visualization with zoom and pan;
- Preparing IQ data for GNSS / CRPA / STAP processing.

The project was designed primarily for:

- Raspberry Pi 4;
- ADALM-Pluto SDR;
- GNSS L1 experiments;
- CRPA and STAP algorithm development.

---

# Utilities

## 1. pluto_2rx_gui_record.cpp

Graphical utility for recording two synchronized RX channels from Pluto SDR.

### Features

- Dual-channel recording;
- Binary IQ output;
- Recording duration selection;
- Sampling frequency selection;
- RF bandwidth selection;
- Manual gain control;
- AGC OFF mode;
- SSD recording support.

### Binary file format

```text
I0 Q0 I1 Q1 I0 Q0 I1 Q1 ...
```

Data type:

```text
int16 little-endian
```

### Linux dependencies

Install on Raspberry Pi OS / Ubuntu:

```bash
sudo apt update

sudo apt install -y \
    g++ \
    pkg-config \
    libgtk-3-dev \
    libiio-dev
```

### Build

```bash
g++ -O2 -Wall -Wextra \
    pluto_2rx_gui_record.cpp \
    -o pluto_2rx_gui_record \
    $(pkg-config --cflags --libs gtk+-3.0) \
    -liio
```

---

## 2. pluto_2rx_spectrum_zoom_gui.cpp

Interactive spectrum analyzer for recorded Pluto IQ files.

### Features

- Open recorded binary IQ file;
- Averaged FFT spectrum over the entire recording;
- FFT size selection;
- RX0 and RX1 visualization;
- Mouse zoom;
- Mouse pan;
- Reset zoom;
- Interactive GUI.

### Linux dependencies

```bash
sudo apt update

sudo apt install -y \
    g++ \
    pkg-config \
    libgtk-3-dev \
    libfftw3-dev
```

### Build

```bash
g++ -O2 -Wall -Wextra \
    pluto_2rx_spectrum_zoom_gui.cpp \
    -o pluto_2rx_spectrum_zoom_gui \
    $(pkg-config --cflags --libs gtk+-3.0) \
    -lfftw3 -lm
```

---

## 3. pluto_2rx_bin_to_txt_gui.cpp

Linux graphical utility for converting binary Pluto IQ files into TXT format.

### Features

- Select binary file;
- Select output TXT file;
- Select number of samples to export;
- Export without header.

### TXT output format

```text
I0    Q0    I1    Q1
```

### Linux dependencies

```bash
sudo apt update

sudo apt install -y \
    g++ \
    pkg-config \
    libgtk-3-dev
```

### Build

```bash
g++ -O2 -Wall -Wextra \
    pluto_2rx_bin_to_txt_gui.cpp \
    -o pluto_2rx_bin_to_txt_gui \
    $(pkg-config --cflags --libs gtk+-3.0)
```

---

## 4. pluto_2rx_bin_to_txt_win.cpp

Windows graphical utility for converting Pluto binary IQ files into TXT format.

### Features

- Open binary IQ file;
- Save TXT file;
- Select number of samples;
- Progress bar;
- Native Windows GUI.

### TXT output format

```text
I0    Q0    I1    Q1
```

---

# Windows Build

## Visual Studio

Required libraries:

```text
user32.lib
comdlg32.lib
comctl32.lib
shell32.lib
```

### Visual Studio settings

```text
Project -> Properties -> Linker -> System
Subsystem = Windows (/SUBSYSTEM:WINDOWS)
```

```text
Project -> Properties -> Advanced
Character Set = Use Unicode Character Set
```

### Visual Studio compile command

```cmd
cl /EHsc /O2 /W4 /DUNICODE /D_UNICODE \
pluto_2rx_bin_to_txt_win.cpp \
user32.lib comdlg32.lib comctl32.lib shell32.lib
```

---

## MinGW / MSYS2

### Install MSYS2

Official website:

```text
https://www.msys2.org/
```

### Install compiler

```bash
pacman -S mingw-w64-x86_64-gcc
```

### Build

```bash
g++ -O2 -Wall -Wextra -municode \
pluto_2rx_bin_to_txt_win.cpp \
-o pluto_2rx_bin_to_txt_win.exe \
-lcomdlg32 -lcomctl32 -lshell32
```

---

# Recommended Pluto Settings for GPS L1

```text
LO Frequency : 1575.420 MHz
Sample Rate  : 4 MSPS
Bandwidth    : 3 MHz
Gain         : 35-45 dB
AGC          : OFF (manual)
```

---

# Typical Data Rate

For:

```text
2 RX channels
4 MSPS
int16 IQ
```

Binary stream:

```text
I0 Q0 I1 Q1
= 4 x int16
= 8 bytes/sample
```

Result:

```text
~32 MB/s
```

For 180 seconds:

```text
~5.76 GB
```

USB3 SSD is strongly recommended.

---

# Recommended SSD Filesystem

```text
EXT4
```

Mount options:

```text
noatime
```

---

# Future Development

The recorded IQ data can be used for:

- GNSS acquisition;
- GCC-PHAT delay estimation;
- Gain/phase calibration;
- PSD/Welch analysis;
- CRPA beamforming;
- DOA estimation;
- STAP processing;
- Null steering;
- Real-time GNSS anti-jamming experiments.

