// Maintainer integration checks against the real companion implementation.
#define wWinMain volume_program_entry
#define VOLUME_TEST_TRACE
#include "volume-update.cpp"
#undef wWinMain
#include <cassert>
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    assert(argc==2); fs::path dir=fs::absolute(argv[1]); fs::create_directories(dir);
    HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED); assert(SUCCEEDED(hr));
    fs::path ini=dir/L"audio.ini";
    assert(load_volume(ini)==-1);
    assert(save_volume(ini,0)); assert(load_volume(ini)==0);
    assert(save_volume(ini,100)); assert(load_volume(ini)==100);
    WritePrivateProfileStringW(L"audio",L"volume",L"101",ini.c_str()); assert(load_volume(ini)==-1);
    WritePrivateProfileStringW(L"audio",L"volume",L"bad",ini.c_str()); assert(load_volume(ini)==-1);
    assert(save_volume(ini,31));
    IMMDeviceEnumerator* en=nullptr; IMMDevice* dev=nullptr; IAudioClient* client=nullptr;
    assert(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&en))));
    assert(SUCCEEDED(en->GetDefaultAudioEndpoint(eRender,eConsole,&dev)));
    assert(SUCCEEDED(dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&client)));
    WAVEFORMATEX* fmt=nullptr; assert(SUCCEEDED(client->GetMixFormat(&fmt)));
    GUID guid; CoCreateGuid(&guid);
    assert(SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_NOPERSIST,600000,0,fmt,&guid)));
    CoTaskMemFree(fmt); assert(SUCCEEDED(client->Start()));
    ISimpleAudioVolume* volume=find_volume(GetCurrentProcessId()); assert(volume);
    assert(SUCCEEDED(volume->SetMasterVolume(0.31f,nullptr)));
    assert(SUCCEEDED(volume->SetMute(TRUE,nullptr)));
    assert(adjust_volume(volume,1,ini)==36); assert(load_volume(ini)==36);
    BOOL muted=TRUE; volume->GetMute(&muted); assert(!muted);
    assert(adjust_volume(volume,-1,ini)==31);
    volume->SetMasterVolume(0.99f,nullptr); assert(adjust_volume(volume,1,ini)==100);
    assert(adjust_volume(volume,1,ini)==100);
    volume->SetMasterVolume(0.01f,nullptr); assert(adjust_volume(volume,-1,ini)==0);
    assert(adjust_volume(volume,-1,ini)==0);
    assert(adjust_volume(volume,1,ini)==5);
    std::ofstream(dir/L"native.bin",std::ios::binary)<<"padding[volume] Restored %d%% from audio.ini";
    std::ofstream(dir/L"legacy.bin",std::ios::binary)<<"legacy player";
    assert(native_volume(dir/L"native.bin")); assert(!native_volume(dir/L"legacy.bin"));
    // Verify the overlay creates and paints without taking input or focus.
    WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=overlay_proc; wc.lpszClassName=L"volume-test";
    assert(RegisterClassW(&wc));
    HWND window=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_LAYERED,wc.lpszClassName,L"volume test",WS_POPUP,0,0,252,76,nullptr,nullptr,wc.hInstance,nullptr);
    assert(window); shown_volume=36; SendMessageW(window,WM_PAINT,0,0); DestroyWindow(window);
    release(volume); client->Stop(); release(client); release(dev); release(en); CoUninitialize();
    std::cout<<"PASS real WASAPI PID routing, unmute, clamp, save/load, native detection and overlay paint\n";
}
