CXX ?= g++
CPPFLAGS += -Isrc -Ivendor/hsmodem -I/usr/local/include
CXXFLAGS ?= -O2 -g -std=c++11 -Wall -Wextra
LDFLAGS += -L/usr/local/lib -Wl,-rpath,/usr/local/lib
LDLIBS += -lliquid -lm
SOURCES = src/rx_probe.cpp src/pilot_afc.cpp vendor/hsmodem/symboltracker.cpp \
 vendor/hsmodem/constellation.cpp vendor/hsmodem/frame_packer.cpp \
 vendor/hsmodem/scrambler.cpp vendor/hsmodem/fec.cpp vendor/hsmodem/crc16.cpp

.PHONY: all clean test
all: build/rx_probe
build/rx_probe: $(SOURCES) src/hsmodem.h src/pilot_afc.h $(wildcard vendor/hsmodem/*.h) $(wildcard vendor/hsmodem/fec/*.hpp)
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
build/generate_fixture: tests/generate_fixture.cpp vendor/hsmodem/scrambler.cpp vendor/hsmodem/crc16.cpp src/hsmodem.h
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/generate_fixture.cpp vendor/hsmodem/scrambler.cpp vendor/hsmodem/crc16.cpp $(LDFLAGS) $(LDLIBS) -o $@
test: all build/generate_fixture
	/usr/bin/python3 -m unittest discover -s tests -v
clean:
	rm -rf build
