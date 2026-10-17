#include "../asio_types.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Load the shipped binary, not an included copy of its source. This catches
// missing compiler-runtime DLLs, missing exports and stale versioned artifacts.
// No registry writes, device discovery or audio streaming are needed.
int main(int argc, char** argv) {
    assert(argc == 2);
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        fprintf(stderr, "Cannot load %s: Windows error %lu\n", argv[1], GetLastError());
        return 1;
    }
    typedef HRESULT (WINAPI *GetClassObjectFn)(REFCLSID, REFIID, void**);
    typedef HRESULT (WINAPI *CanUnloadFn)(void);
    GetClassObjectFn getClass = (GetClassObjectFn)(void*)GetProcAddress(module, "DllGetClassObject");
    CanUnloadFn canUnload = (CanUnloadFn)(void*)GetProcAddress(module, "DllCanUnloadNow");
    assert(getClass && canUnload && canUnload() == S_OK);
    const GUID clsid = {0xb2a0f51e, 0x32e8, 0x48d3, {0x89, 0x02, 0x6c, 0xd4, 0x52, 0xd7, 0xef, 0x31}};
    IClassFactory* factory = NULL;
    assert(getClass(clsid, IID_IClassFactory, (void**)&factory) == S_OK);
    IASIO* driver = NULL;
    assert(factory->CreateInstance(NULL, clsid, (void**)&driver) == S_OK);
    assert(driver->getDriverVersion() == 10000);
    char name[64];
    driver->getDriverName(name);
    assert(strcmp(name, "ASIO-for-UMC22") == 0);
    long inputs, outputs;
    assert(driver->getChannels(&inputs, &outputs) == ASE_OK && inputs == 2 && outputs == 2);
    assert(canUnload() == S_FALSE);
    driver->Release();
    factory->Release();
    assert(canUnload() == S_OK && FreeLibrary(module));
    puts("PASS built DLL: load, exports, COM instance, version, stereo channels and unload");
}
