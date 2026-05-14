#!/bin/bash

g++ -O2 -Wall -Wextra \
    pluto_2rx_gui_record.cpp \
    -o pluto_2rx_gui_record \
    $(pkg-config --cflags --libs gtk+-3.0) \
    -liio
