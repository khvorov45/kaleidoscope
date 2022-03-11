call shell.bat
cl -Od -Z7 -nologo -TC -Wall -Febuild\kaleidoscope.exe .\code\kaleidoscope.c ^
	-link %LLVM_PATH%\lib\LLVM-C.lib
