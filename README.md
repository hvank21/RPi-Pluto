[README.md](https://github.com/user-attachments/files/27751402/README.md)
# Pluto_RPi4 — утиліти для ADALM-Pluto 2RX

Проєкт містить набір C/C++ утиліт для роботи з ADALM-Pluto / Pluto+ у режимі 2RX:

- запис двох IQ-каналів у binary-файл на SSD;
- побудова спектра/спектрограми з binary-файлу;
- конвертація binary IQ у TXT;
- Windows GUI-конвертер binary → TXT.

Основний формат binary-файлів:

```text
int16 little-endian:
I0 Q0 I1 Q1 I0 Q0 I1 Q1 ...
```

Для двох каналів Pluto:

```text
RX0 = I0 + jQ0
RX1 = I1 + jQ1
```

---

## 1. Структура папки

Рекомендована структура:

```text
~/Pluto_RPi4/
 ├── pluto_2rx_gui_record.cpp
 ├── pluto_2rx_spectrum_gui.cpp
 ├── pluto_2rx_spectrum_zoom_gui.cpp
 ├── pluto_2rx_spectrogram_gui.cpp
 ├── pluto_2rx_bin_to_txt_gui.cpp
 ├── pluto_2rx_bin_to_txt_win.cpp
 ├── build.sh
 ├── run.sh
 └── records/
```

Створити папку на Raspberry Pi:

```bash
mkdir -p ~/Pluto_RPi4/records
cd ~/Pluto_RPi4
```

---

## 2. Raspberry Pi / Linux залежності

### 2.1. Базові інструменти компіляції

Використовуються:

- `g++` — компілятор C++;
- `pkg-config` — пошук параметрів компіляції бібліотек;
- `make`, `cmake` — бажано мати для майбутніх збірок.

Інсталяція:

```bash
sudo apt update
sudo apt install -y g++ gcc make cmake pkg-config
```

---

### 2.2. libiio

Використовується у програмі запису з Pluto:

```text
pluto_2rx_gui_record.cpp
```

Потрібна для підключення до ADALM-Pluto через IIO:

- `iio_create_context_from_uri()`;
- `iio_context_find_device()`;
- `iio_channel_attr_write()`;
- `iio_buffer_refill()`.

Інсталяція:

```bash
sudo apt install -y libiio-dev iiod iio-utils
```

Перевірка Pluto:

```bash
iio_info -s
```

Приклад перевірки через IP:

```bash
iio_info -u ip:192.168.2.1
```

---

### 2.3. GTK3

Використовується для графічного інтерфейсу Linux/Raspberry Pi:

```text
pluto_2rx_gui_record.cpp
pluto_2rx_spectrum_gui.cpp
pluto_2rx_spectrum_zoom_gui.cpp
pluto_2rx_spectrogram_gui.cpp
pluto_2rx_bin_to_txt_gui.cpp
```

Потрібна бібліотека:

```text
libgtk-3-dev
```

Інсталяція:

```bash
sudo apt install -y libgtk-3-dev
```

Перевірка параметрів компіляції:

```bash
pkg-config --cflags --libs gtk+-3.0
```

---

### 2.4. FFTW3

Використовується для FFT, спектра та спектрограми:

```text
pluto_2rx_spectrum_gui.cpp
pluto_2rx_spectrum_zoom_gui.cpp
pluto_2rx_spectrogram_gui.cpp
```

Потрібна бібліотека:

```text
libfftw3-dev
```

Інсталяція:

```bash
sudo apt install -y libfftw3-dev
```

---

## 3. Повна інсталяція залежностей на Raspberry Pi

Однією командою:

```bash
sudo apt update
sudo apt install -y \
    g++ gcc make cmake pkg-config \
    libiio-dev iiod iio-utils \
    libgtk-3-dev \
    libfftw3-dev
```

---

## 4. Компіляція Linux/Raspberry Pi утиліт

### 4.1. Запис 2RX IQ у binary на SSD

Файл:

```text
pluto_2rx_gui_record.cpp
```

Компіляція:

```bash
g++ -O2 -Wall -Wextra pluto_2rx_gui_record.cpp -o pluto_2rx_gui_record \
    $(pkg-config --cflags --libs gtk+-3.0) -liio
```

Запуск:

```bash
./pluto_2rx_gui_record
```

---

### 4.2. Загальний спектр по всьому запису

Файл:

```text
pluto_2rx_spectrum_gui.cpp
```

Компіляція:

```bash
g++ -O2 -Wall -Wextra pluto_2rx_spectrum_gui.cpp -o pluto_2rx_spectrum_gui \
    $(pkg-config --cflags --libs gtk+-3.0) -lfftw3 -lm
```

Запуск:

```bash
./pluto_2rx_spectrum_gui
```

---

### 4.3. Спектр з масштабуванням

Файл:

```text
pluto_2rx_spectrum_zoom_gui.cpp
```

Компіляція:

```bash
g++ -O2 -Wall -Wextra pluto_2rx_spectrum_zoom_gui.cpp -o pluto_2rx_spectrum_zoom_gui \
    $(pkg-config --cflags --libs gtk+-3.0) -lfftw3 -lm
```

Запуск:

```bash
./pluto_2rx_spectrum_zoom_gui
```

Керування:

```text
Колесо миші      — масштабування
Ліва кнопка миші — переміщення графіка
Reset Zoom       — скидання масштабу
```

---

### 4.4. Спектрограма

Файл:

```text
pluto_2rx_spectrogram_gui.cpp
```

Компіляція:

```bash
g++ -O2 -Wall -Wextra pluto_2rx_spectrogram_gui.cpp -o pluto_2rx_spectrogram_gui \
    $(pkg-config --cflags --libs gtk+-3.0) -lfftw3 -lm
```

Запуск:

```bash
./pluto_2rx_spectrogram_gui
```

---

### 4.5. Linux GUI-конвертер binary → TXT

Файл:

```text
pluto_2rx_bin_to_txt_gui.cpp
```

Компіляція:

```bash
g++ -O2 -Wall -Wextra pluto_2rx_bin_to_txt_gui.cpp -o pluto_2rx_bin_to_txt_gui \
    $(pkg-config --cflags --libs gtk+-3.0)
```

Запуск:

```bash
./pluto_2rx_bin_to_txt_gui
```

Вихідний TXT формат без заголовка:

```text
I0 Q0 I1 Q1
```

---

## 5. Windows GUI-конвертер binary → TXT

Файл:

```text
pluto_2rx_bin_to_txt_win.cpp
```

Це окрема Windows-програма на WinAPI. Зовнішні бібліотеки типу GTK або Qt не потрібні.

Використовуються стандартні Windows-бібліотеки:

- `user32.lib` — вікна, кнопки, edit-поля;
- `comdlg32.lib` — діалог вибору файлу;
- `comctl32.lib` — progress bar, common controls;
- `shell32.lib` — допоміжні shell-функції.

У коді мають бути підключення:

```cpp
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#pragma comment(lib, "comctl32.lib")
```

---

### 5.1. Компіляція у Visual Studio

Створити проєкт:

```text
Windows Desktop Application
```

або для існуючого проєкту встановити:

```text
Project → Properties → Linker → System → SubSystem
Windows (/SUBSYSTEM:WINDOWS)
```

Character Set:

```text
Project → Properties → Advanced → Character Set
Use Unicode Character Set
```

Additional Dependencies:

```text
Project → Properties → Linker → Input → Additional Dependencies
```

Додати:

```text
user32.lib
comdlg32.lib
comctl32.lib
shell32.lib
```

---

### 5.2. Компіляція у Developer Command Prompt for Visual Studio

```cmd
cl /EHsc /O2 /W4 /DUNICODE /D_UNICODE pluto_2rx_bin_to_txt_win.cpp user32.lib comdlg32.lib comctl32.lib shell32.lib
```

Якщо код має точку входу `wWinMain`, потрібно використовувати Windows subsystem:

```cmd
cl /EHsc /O2 /W4 /DUNICODE /D_UNICODE pluto_2rx_bin_to_txt_win.cpp user32.lib comdlg32.lib comctl32.lib shell32.lib /link /SUBSYSTEM:WINDOWS
```

---

### 5.3. Компіляція MinGW / MSYS2

Встановити MSYS2:

```text
https://www.msys2.org/
```

У MSYS2 MinGW64 shell:

```bash
pacman -Syu
pacman -S --needed mingw-w64-x86_64-gcc
```

Компіляція:

```bash
g++ -O2 -Wall -Wextra -municode pluto_2rx_bin_to_txt_win.cpp -o pluto_2rx_bin_to_txt_win.exe \
    -lcomdlg32 -lcomctl32 -lshell32
```

---

## 6. Налаштування Visual Studio Code для Windows

Для IntelliSense створити або відредагувати:

```text
.vscode/c_cpp_properties.json
```

Приклад для MSYS2 MinGW64:

```json
{
  "configurations": [
    {
      "name": "Win32",
      "includePath": [
        "${workspaceFolder}/**",
        "C:/msys64/mingw64/include",
        "C:/msys64/mingw64/x86_64-w64-mingw32/include"
      ],
      "defines": [
        "UNICODE",
        "_UNICODE",
        "NOMINMAX"
      ],
      "compilerPath": "C:/msys64/mingw64/bin/g++.exe",
      "cppStandard": "c++17",
      "intelliSenseMode": "windows-gcc-x64"
    }
  ],
  "version": 4
}
```

---

## 7. SSD для запису IQ

Перевірити монтування:

```bash
df -h
ls -ld /mnt/ssd
```

Якщо каталог SSD не доступний для запису користувачу `val`:

```bash
sudo chown -R val:val /mnt/ssd
chmod -R 775 /mnt/ssd
```

Перевірка запису:

```bash
touch /mnt/ssd/test.txt
ls -l /mnt/ssd/test.txt
```

Тест швидкості SSD:

```bash
dd if=/dev/zero of=/mnt/ssd/test.bin bs=1M count=2048 oflag=direct
```

Для Pluto 2RX при 4 MSPS потрібен потік приблизно:

```text
4e6 samples/s × 8 bytes = 32 MB/s
```

За 180 секунд:

```text
32 MB/s × 180 s ≈ 5.76 GB
```

---

## 8. Типові параметри для GPS L1

```text
URI:        ip:192.168.2.1
LO:         1575.420 MHz
Fs:         4.0 MSPS
BW:         3.0 або 5.0 MHz
Gain RX0:   35...45 dB
Gain RX1:   35...45 dB
AGC:        OFF / manual
Time:       до 180 секунд
```

---

## 9. Типові помилки

### 9.1. `ssd directory is not writable by current user`

Причина: немає прав запису у `/mnt/ssd`.

Виправлення:

```bash
sudo chown -R val:val /mnt/ssd
chmod -R 775 /mnt/ssd
```

---

### 9.2. `PBM_SETPOS undeclared`, `PROGRESS_CLASSW undefined`

Причина: не підключений `commctrl.h` або не підключена бібліотека `comctl32`.

Виправлення у коді:

```cpp
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
```

Виправлення при компіляції MinGW:

```bash
-lcomctl32
```

---

### 9.3. `unresolved external symbol main`

Причина: Windows GUI-програма має `wWinMain`, а проєкт створений як Console Application.

Виправлення:

```text
Project → Properties → Linker → System → SubSystem
Windows (/SUBSYSTEM:WINDOWS)
```

---

### 9.4. Помилка `std::min` або `std::max` після `windows.h`

Причина: `windows.h` створює макроси `min`/`max`.

Виправлення перед `#include <windows.h>`:

```cpp
#define NOMINMAX
```

---

## 10. Подальший пайплайн

Після запису binary-файлу:

```text
Pluto 2RX binary IQ
   ↓
TXT conversion або пряме читання binary
   ↓
спектр / спектрограма
   ↓
оцінка gain/phase/delay RX0-RX1
   ↓
компенсація каналів
   ↓
GNSS acquisition GPS L1 C/A
   ↓
CRPA / STAP / null steering
```
