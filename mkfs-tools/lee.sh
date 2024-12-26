#!/bin/bash

sudo ./autogen.sh
sudo ldconfig
sudo ./configure
sudo make
sudo make install
