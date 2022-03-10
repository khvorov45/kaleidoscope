gcc -g -Wall code/kaleidoscope.c -o build/kaleidoscope.bin \
	-I"/usr/include/llvm-c-11" -lLLVM-11 \
	-Wno-switch -Wno-unused-function