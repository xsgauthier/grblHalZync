#!/bin/bash

apt list --installed 2>/dev/null >.apt-installed.tmp

apt_update_once() {
	if [ -z "$APT_UPDATED" ]; then
		sudo apt update
		APT_UPDATED=1
	fi
}

grep cmake >/dev/null 2>/dev/null <.apt-installed.tmp
if [ $? -ne 0 ]; then
	apt_update_once
	sudo apt install -y cmake
fi
grep libbsd-dev >/dev/null 2>/dev/null <.apt-installed.tmp
if [ $? -ne 0 ]; then
	apt_update_once
	sudo apt install -y libbsd-dev
fi
grep socat >/dev/null 2>/dev/null <.apt-installed.tmp
if [ $? -ne 0 ]; then
	apt_update_once
	sudo apt install -y socat
fi
grep libgpiod-dev >/dev/null 2>/dev/null <.apt-installed.tmp
if [ $? -ne 0 ]; then
	apt_update_once
	sudo apt install -y libgpiod-dev
fi

if [ ! -f src/sdcard/CMakeLists.txt ]; then
	git submodule update --init --recursive
fi

git pull

mkdir build
cd build
cmake ..

make -j 6 grblHAL_zynq


