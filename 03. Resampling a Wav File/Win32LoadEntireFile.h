
#include <stdint.h>

bool win32LoadEntireFile(const char* filename, void** data, uint32_t* numBytesRead);
void Win32FreeFileData(void *data);
