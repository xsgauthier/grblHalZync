#!/bin/bash

apt list --installed 2>/dev/null >.apt-installed.tmp

grep cmake >/dev/null 2>/dev/null <.apt-installed.tmp
if [ $? -ne 0 ]; then
	sudo apt update
	sudo apt install -y cmake
fi
grep libbsd-dev >/dev/null 2>/dev/null <.apt-installed.tmp
if [ $? -ne 0 ]; then
	sudo apt update
	sudo apt install -y libbsd-dev
fi

#sudo apt install socat

if [ ! -f src/sdcard/CMakeLists.txt ]; then
	git submodule update --init --recursive
fi

git pull

mkdir build
cd build
cmake ..

make -j 6 grblHAL_zynq


