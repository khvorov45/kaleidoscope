clang-cl -Od -Z7 -nologo -TC -Wall -Febuild\kaleidoscope.exe .\code\kaleidoscope.c ^
	-Wno-switch -Wno-string-conversion -Wno-switch-enum -Wno-unused-function -Wno-microsoft-include -Wno-language-extension-token -Wno-documentation-unknown-command ^
	-link %LLVM_PATH%\lib\LLVM-C.lib
