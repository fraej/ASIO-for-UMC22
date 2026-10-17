CXX = x86_64-w64-mingw32-g++
CC = x86_64-w64-mingw32-gcc
CXXFLAGS = -std=c++11 -O3 -Wall -Wextra -D_WIN32_WINNT=0x0601
CFLAGS = -std=c11 -O3 -Wall -Wextra -D_WIN32_WINNT=0x0601
# Include MinGW runtimes, including winpthreads when using its POSIX compiler.
LDFLAGS = -shared -s asio_driver.def -luuid -lsetupapi -lwinmm -lole32 -ladvapi32 -luser32 -lgdi32 -static
HOST_CXX ?= g++
HOST_CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra
WINDOWS_RUNNER ?= wine64

TARGET = asio_for_umc22.dll
OBJS = asio_driver.o ks_engine.o

all: $(TARGET)

asio_driver.o: asio_driver.cpp asio_types.h ks_engine.h audio_convert.h
	$(CXX) $(CXXFLAGS) -c asio_driver.cpp -o asio_driver.o

ks_engine.o: ks_engine.c ks_engine.h
	$(CC) $(CFLAGS) -c ks_engine.c -o ks_engine.o

$(TARGET): $(OBJS) asio_driver.def
	$(CXX) $(OBJS) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf build

build/tests:
	mkdir -p $@

build/tests/audio_convert_test: tests/audio_convert_test.cpp audio_convert.h | build/tests
	$(HOST_CXX) $(HOST_CXXFLAGS) $< -o $@

build/tests/latency_calibration_test: tests/latency_calibration_test.cpp tools/latency_calibration.h | build/tests
	$(HOST_CXX) $(HOST_CXXFLAGS) $< -o $@

build/tests/engine_test.exe: tests/engine_test.c ks_engine.c ks_engine.h | build/tests
	$(CC) $(CFLAGS) $< -lsetupapi -luser32 -o $@

build/tests/driver_test.exe: tests/driver_test.cpp asio_driver.cpp asio_types.h ks_engine.h audio_convert.h | build/tests
	$(CXX) $(CXXFLAGS) $< -luuid -ladvapi32 -luser32 -lgdi32 -static -o $@

build/tests/dll_test.exe: tests/dll_test.cpp asio_types.h | build/tests
	$(CXX) $(CXXFLAGS) $< -luuid -static -o $@

test-binaries: build/tests/audio_convert_test build/tests/latency_calibration_test build/tests/engine_test.exe build/tests/driver_test.exe build/tests/dll_test.exe

build/tools:
	mkdir -p $@

# Reports what the registered driver tells hosts, and measures round-trip latency
# through a loopback cable (see README). Runs on Windows with the UMC22 attached.
build/tools/latency_test.exe: tools/latency_test.cpp tools/latency_calibration.h asio_types.h | build/tools
	$(CXX) $(CXXFLAGS) $< -lole32 -luuid -ladvapi32 -static -o $@

latency-test: build/tools/latency_test.exe

test-native: build/tests/audio_convert_test build/tests/latency_calibration_test
	./build/tests/audio_convert_test
	./build/tests/latency_calibration_test

test-windows: build/tests/engine_test.exe build/tests/driver_test.exe build/tests/dll_test.exe $(TARGET)
	$(WINDOWS_RUNNER) ./build/tests/engine_test.exe
	$(WINDOWS_RUNNER) ./build/tests/driver_test.exe
	$(WINDOWS_RUNNER) ./build/tests/dll_test.exe ./$(TARGET)

test: test-native test-windows

.PHONY: all clean test test-binaries test-native test-windows latency-test
