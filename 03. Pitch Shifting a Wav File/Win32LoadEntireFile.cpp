#include "Win32LoadEntireFile.h"

#include <windows.h>

bool win32LoadEntireFile(const char* filename, void** data, uint32_t* numBytesRead)
{    
    HANDLE file = CreateFileA(filename, GENERIC_READ, 0, 0, OPEN_EXISTING, 0, 0);  
    if((file == INVALID_HANDLE_VALUE)) return false;
    
    DWORD fileSize = GetFileSize(file, 0);
    if(!fileSize) return false;
    
    *data = HeapAlloc(GetProcessHeap(), 0, fileSize+1);
    if(!*data) return false;

    if(!ReadFile(file, *data, fileSize, (LPDWORD)numBytesRead, 0))
        return false;
    
    CloseHandle(file);
    ((uint8_t*)*data)[fileSize] = 0;
    
    return true;
}

void Win32FreeFileData(void *data)
{
    HeapFree(GetProcessHeap(), 0, data);
}
