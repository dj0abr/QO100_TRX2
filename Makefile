.PHONY: all test clean
all:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPython3_EXECUTABLE=/usr/bin/python3
	cmake --build build -j
test: all
	ctest --test-dir build --output-on-failure
clean:
	cmake --build build --target clean
