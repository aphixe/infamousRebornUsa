// Handwritten volume companion + single-file updater. No game code or assets.
// Build on the maintainer PC only; players need neither Python nor C++ tools.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <audioclient.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace fs = std::filesystem;
template<class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
static std::wstring error_text(const std::wstring& text) {
    return text + L" (Windows error " + std::to_wstring(GetLastError()) + L")";
}
static fs::path module_path() {
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, p.data(), (DWORD)p.size());
    p.resize(n); return p;
}
static bool installed(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_regular_file(root / L"installer/launch.txt",ec)) return false;
    return fs::is_regular_file(root / L"inFamous.exe",ec);
}
static std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), towlower); return s;
}
static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static std::wstring utf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n); return w;
}
static fs::path game_exe(const fs::path& root) {
    std::ifstream file(root / L"installer/launch.txt"); std::string line;
    while (std::getline(file, line)) if (line.rfind("exe=", 0) == 0) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        fs::path p(utf8(line.substr(4))); return p.is_absolute() ? p : root / p;
    }
    return {};
}
static bool native_volume(const fs::path& exe) {
    // Stream rather than load a large game EXE into RAM. Keep overlap for a
    // marker split across two chunks. Never modify or inject into the player.
    std::ifstream f(exe, std::ios::binary); char block[65536]; std::string tail;
    const std::string marker = "[volume] Restored %d%% from audio.ini";
    while (f) {
        f.read(block, sizeof block);
        tail.append(block, (size_t)f.gcount());
        if (tail.find(marker) != std::string::npos) return true;
        if (tail.size() > marker.size()) tail.erase(0, tail.size() - marker.size());
    }
    return false;
}
static std::vector<fs::path> discover() {
    std::vector<fs::path> out;
    auto add = [&](fs::path p) {
        if (!installed(p)) return;
        std::error_code ec;
        auto canonical=fs::weakly_canonical(p,ec);
        if (!ec) p=canonical;
        for (const auto& e : out) if (lower(e.wstring()) == lower(p.wstring())) return;
        out.push_back(p);
    };
    for (fs::path p : {module_path().parent_path(), fs::current_path()})
        for (int i = 0; i < 3 && !p.empty(); ++i, p = p.parent_path()) add(p);
    add(L"C:/Games/inFamous");
    // Read the player's existing desktop/Start-menu shortcut targets, including
    // redirected/OneDrive known folders. No recursive drive scan or network use.
    for (const KNOWNFOLDERID* folder : {&FOLDERID_Desktop, &FOLDERID_Programs,
                                      &FOLDERID_PublicDesktop, &FOLDERID_CommonPrograms}) {
        PWSTR path = nullptr;
        if (FAILED(SHGetKnownFolderPath(*folder, 0, nullptr, &path))) continue;
        fs::path base(path); CoTaskMemFree(path);
        std::error_code ec;
        fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 3) { it.disable_recursion_pending(); continue; }
            if (lower(it->path().extension().wstring()) != L".lnk" ||
                lower(it->path().filename().wstring()).find(L"infamous") == std::wstring::npos) continue;
            IShellLinkW* link = nullptr; IPersistFile* persist = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
                SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist))) &&
                SUCCEEDED(persist->Load(it->path().c_str(), STGM_READ))) {
                wchar_t target[32768]{};
                if (SUCCEEDED(link->GetPath(target, 32768, nullptr, SLGP_RAWPATH))) {
                    fs::path p(target);
                    for (int i = 0; i < 3 && !p.empty(); ++i, p = p.parent_path()) add(p);
                }
            }
            release(persist); release(link);
        }
    }
    return out;
}
static fs::path choose_folder() {
    IFileOpenDialog* dialog = nullptr; IShellItem* item = nullptr; fs::path result;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Choose your existing inFamous installation folder");
        if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { result = path; CoTaskMemFree(path); }
        }
    }
    release(item); release(dialog); return result;
}
static bool game_running() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw error_text(L"Cannot check whether the game is running");
    PROCESSENTRY32W p{}; p.dwSize = sizeof p; bool found = false;
    if (Process32FirstW(snapshot, &p)) do {
        if (lower(p.szExeFile) == L"infamousrecomp.exe") { found = true; break; }
    } while (Process32NextW(snapshot, &p));
    CloseHandle(snapshot); return found;
}
static void install_update(const fs::path& root) {
    if (!installed(root) || !fs::is_regular_file(game_exe(root)))
        throw std::wstring(L"Choose an existing, working installation containing inFamous.exe.");
    if (game_running()) throw std::wstring(L"Close inFamous, then run this update again.");
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(201), RT_RCDATA);
    DWORD size = res ? SizeofResource(nullptr, res) : 0;
    const void* bytes = res ? LockResource(LoadResource(nullptr, res)) : nullptr;
    if (!bytes || size < 1024) throw std::wstring(L"This updater is damaged. Download it again.");
    fs::path launcher = root/L"inFamous.exe", helper = root/L"installer/infamous-volume.exe";
    fs::path backup = root/L".installer/volume-exe-backups"/std::to_wstring(GetTickCount64());
    fs::create_directories(backup);
    fs::copy_file(launcher, backup/L"inFamous.exe");
    bool had_helper = fs::is_regular_file(helper);
    if (had_helper) fs::copy_file(helper, backup/L"infamous-volume.exe");
    fs::path staged_launcher = root/L".installer/volume-launcher.tmp";
    fs::path staged_helper = root/L".installer/volume-helper.tmp";
    try {
        { std::ofstream f(staged_launcher, std::ios::binary); f.write((const char*)bytes, size);
          f.close(); if (!f) throw std::wstring(L"Cannot write the updated launcher."); }
        fs::copy_file(module_path(), staged_helper, fs::copy_options::overwrite_existing);
        if (!MoveFileExW(staged_launcher.c_str(), launcher.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw error_text(L"Cannot replace the launcher; close it and try again");
        if (!MoveFileExW(staged_helper.c_str(), helper.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw error_text(L"Cannot install the volume helper");
    } catch (...) {
        fs::copy_file(backup/L"inFamous.exe", launcher, fs::copy_options::overwrite_existing);
        if (had_helper) fs::copy_file(backup/L"infamous-volume.exe", helper, fs::copy_options::overwrite_existing);
        std::error_code ec; fs::remove(staged_launcher, ec); fs::remove(staged_helper, ec);
        throw;
    }
    // Deliberately do not touch the game EXE, build tools, launch.txt, INIs or saves.
}

static ISimpleAudioVolume* find_volume(DWORD pid) {
    IMMDeviceEnumerator* en = nullptr; IMMDeviceCollection* devices = nullptr; ISimpleAudioVolume* result = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return nullptr;
    HRESULT enum_hr=en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
#ifdef VOLUME_TEST_TRACE
    fprintf(stderr,"Enum endpoints hr=%08lx pid=%lu\n",enum_hr,pid);
#endif
    if (SUCCEEDED(enum_hr)) {
        UINT count = 0; devices->GetCount(&count);
#ifdef VOLUME_TEST_TRACE
        fprintf(stderr,"Devices=%u\n",count);
#endif
        for (UINT d = 0; d < count && !result; ++d) {
            IMMDevice* dev = nullptr; IAudioSessionManager2* manager = nullptr; IAudioSessionEnumerator* sessions = nullptr;
            HRESULT device_hr=devices->Item(d,&dev);
            HRESULT manager_hr=SUCCEEDED(device_hr)?dev->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,(void**)&manager):device_hr;
            HRESULT sessions_hr=SUCCEEDED(manager_hr)?manager->GetSessionEnumerator(&sessions):manager_hr;
#ifdef VOLUME_TEST_TRACE
            fprintf(stderr,"Device %u item=%08lx manager=%08lx sessions=%08lx\n",d,device_hr,manager_hr,sessions_hr);
#endif
            if (SUCCEEDED(sessions_hr)) {
                int n = 0; sessions->GetCount(&n);
#ifdef VOLUME_TEST_TRACE
                fprintf(stderr,"Device %u sessions=%d\n",d,n);
#endif
                for (int i = 0; i < n && !result; ++i) {
                    IAudioSessionControl* control = nullptr; IAudioSessionControl2* control2 = nullptr;
                    if (SUCCEEDED(sessions->GetSession(i, &control)) && SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&control2)))) {
                        DWORD owner = 0;
                        HRESULT pid_hr=control2->GetProcessId(&owner);
#ifdef VOLUME_TEST_TRACE
                        fprintf(stderr,"Session %d pid=%lu hr=%08lx\n",i,owner,pid_hr);
#endif
                        if (SUCCEEDED(pid_hr) && owner == pid)
                            control->QueryInterface(IID_PPV_ARGS(&result));
                    }
                    release(control2); release(control);
                }
            }
            release(sessions); release(manager); release(dev);
        }
    }
    release(devices); release(en); return result;
}
static int load_volume(const fs::path& ini) {
    wchar_t value[32]{}, *end;
    if (!GetPrivateProfileStringW(L"audio", L"volume", L"", value, 32, ini.c_str())) return -1;
    long n = wcstol(value, &end, 10); return end != value && !*end && n >= 0 && n <= 100 ? (int)n : -1;
}
static bool save_volume(const fs::path& ini, int volume) {
    return WritePrivateProfileStringW(L"audio", L"volume", std::to_wstring(volume).c_str(), ini.c_str()) != FALSE;
}
static int adjust_volume(ISimpleAudioVolume* volume, int dir, const fs::path& ini) {
    float level=0;
    HRESULT hr=volume->GetMasterVolume(&level);
    int pct=std::clamp((int)std::lround(level*100)+dir*5,0,100);
    if (SUCCEEDED(hr)) hr=volume->SetMasterVolume(pct/100.0f,nullptr);
    if (SUCCEEDED(hr) && pct>0) hr=volume->SetMute(FALSE,nullptr);
    if (FAILED(hr)) return -1;
    if (!save_volume(ini,pct)) return -2;
    return pct;
}
static int shown_volume;
static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps); RECT r; GetClientRect(hwnd, &r);
        HBRUSH bg = CreateSolidBrush(RGB(14,12,21)), purple = CreateSolidBrush(RGB(179,107,255)), track = CreateSolidBrush(RGB(64,56,79));
        FillRect(dc, &r, bg); float scale = r.bottom/76.0f;
        RECT accent{0,0,(LONG)(3*scale),r.bottom}; FillRect(dc,&accent,purple);
        HFONT font = CreateFontW(-(int)(24*scale),0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        HGDIOBJ old = SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255));
        RECT text{(LONG)(18*scale),(LONG)(8*scale),r.right,(LONG)(43*scale)};
        std::wstring label = L"Volume  " + std::to_wstring(shown_volume) + L"%";
        DrawTextW(dc,label.c_str(),-1,&text,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
        RECT bar{(LONG)(18*scale),(LONG)(53*scale),r.right-(LONG)(18*scale),(LONG)(59*scale)};
        FillRect(dc,&bar,track); bar.right=bar.left+(LONG)((bar.right-bar.left)*shown_volume/100.0f);
        FillRect(dc,&bar,purple); SelectObject(dc,old); DeleteObject(font);
        DeleteObject(bg); DeleteObject(purple); DeleteObject(track); EndPaint(hwnd,&ps); return 0;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd,msg,wp,lp);
}
struct WindowSearch { DWORD pid; HWND hwnd; };
static BOOL CALLBACK find_window(HWND hwnd, LPARAM context) {
    auto* s = (WindowSearch*)context; DWORD pid = 0; GetWindowThreadProcessId(hwnd,&pid);
    wchar_t cls[128]{}; GetClassNameW(hwnd,cls,128);
    if (pid == s->pid && IsWindowVisible(hwnd) && !wcscmp(cls,L"ps3recomp_d3d12")) { s->hwnd=hwnd; return FALSE; }
    return TRUE;
}
static int watch(DWORD pid, const fs::path& cwd, const fs::path& exe) {
    if (native_volume(exe)) return 0;
    HANDLE process = OpenProcess(SYNCHRONIZE,FALSE,pid); if (!process) return 1;
    std::wstring mutex_name = L"Local\\inFamousVolume-" + std::to_wstring(pid);
    HANDLE mutex = CreateMutexW(nullptr,FALSE,mutex_name.c_str());
    if (!mutex || GetLastError()==ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); CloseHandle(process); return 0; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls{}; cls.hInstance=GetModuleHandleW(nullptr); cls.lpfnWndProc=overlay_proc; cls.lpszClassName=L"inFamousVolumeOverlay";
    RegisterClassW(&cls);
    HWND overlay = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|WS_EX_LAYERED,
        cls.lpszClassName,L"inFamous Volume",WS_POPUP,0,0,252,76,nullptr,nullptr,cls.hInstance,nullptr);
    if (!overlay) { CloseHandle(mutex); CloseHandle(process); return 1; }
    ISimpleAudioVolume* volume = nullptr; fs::path ini=fs::absolute(cwd/L"audio.ini");
    ULONGLONG changed=0, retry=0, next_repeat=0; int last_dir=0; WindowSearch window{pid,nullptr};
    while (WaitForSingleObject(process,20)==WAIT_TIMEOUT) {
        MSG msg; while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        ULONGLONG now=GetTickCount64();
        if (!volume && now>=retry) {
            retry=now+500; volume=find_volume(pid);
            if (volume) { int saved=load_volume(ini); if (saved>=0) { volume->SetMasterVolume(saved/100.0f,nullptr); if (saved>0) volume->SetMute(FALSE,nullptr); } }
        }
        if (!window.hwnd || !IsWindow(window.hwnd)) { window.hwnd=nullptr; EnumWindows(find_window,(LPARAM)&window); }
        HWND fg=GetForegroundWindow();
        bool focused=window.hwnd && (fg==window.hwnd || fg==overlay) && !IsIconic(window.hwnd);
        int dir=0;
        if (focused && !(GetAsyncKeyState(VK_CONTROL)&0x8000) && !(GetAsyncKeyState(VK_MENU)&0x8000)) {
            bool up=(GetAsyncKeyState(VK_OEM_PLUS)&0x8000)||(GetAsyncKeyState(VK_ADD)&0x8000);
            bool down=(GetAsyncKeyState(VK_OEM_MINUS)&0x8000)||(GetAsyncKeyState(VK_SUBTRACT)&0x8000);
            dir=(int)up-(int)down;
        }
        if (dir && volume && (dir!=last_dir || now>=next_repeat)) {
            next_repeat=now+(dir!=last_dir?350:100);
            int pct=adjust_volume(volume,dir,ini);
            if (pct>=0) { shown_volume=pct; changed=now; InvalidateRect(overlay,nullptr,FALSE); }
            else release(volume);
        }
        last_dir=dir;
        if (focused && changed && now-changed<2200) {
            RECT r; GetClientRect(window.hwnd,&r); POINT origin{0,0}; ClientToScreen(window.hwnd,&origin);
            float scale=std::clamp(r.bottom/720.0f,0.7f,3.0f);
            int pad=(int)(24*scale), w=(int)(252*scale), h=(int)(76*scale);
            SetLayeredWindowAttributes(overlay,0,(BYTE)(now-changed>1800?235*(2200-(now-changed))/400:235),LWA_ALPHA);
            SetWindowPos(overlay,HWND_TOPMOST,origin.x+pad,origin.y+r.bottom-pad-h,w,h,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        } else ShowWindow(overlay,SW_HIDE);
    }
    release(volume); DestroyWindow(overlay); CloseHandle(mutex); CloseHandle(process); return 0;
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc=0; wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc); int result=0;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try {
        if (argc==5 && !wcscmp(argv[1],L"--watch")) result=watch(wcstoul(argv[2],nullptr,10),argv[3],argv[4]);
        else if (argc==3 && !wcscmp(argv[1],L"--install-dir")) install_update(argv[2]);
        else if (argc==3 && !wcscmp(argv[1],L"--discover")) {
            std::ofstream f(argv[2],std::ios::binary);
            for (const auto& p:discover()) {
                std::wstring value=p.wstring()+L"\n";
                int n=WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),nullptr,0,nullptr,nullptr);
                std::string bytes(n,0); WideCharToMultiByte(CP_UTF8,0,value.data(),(int)value.size(),bytes.data(),n,nullptr,nullptr);
                f<<bytes;
            }
        }
        else if (argc==3 && !wcscmp(argv[1],L"--check-native")) result=native_volume(argv[2])?0:2;
        else {
            auto candidates=discover(); fs::path root=candidates.size()==1?candidates[0]:choose_folder();
            if (!root.empty()) { install_update(root); MessageBoxW(nullptr,
                (L"Volume controls installed. No game compilation was needed.\n\nInstallation: "+root.wstring()+
                 L"\n\nStart inFamous using its normal shortcut or inFamous.exe.\n= / + or numpad +: volume up\n- or numpad -: volume down\n\nYour volume saves in audio.ini.").c_str(),
                L"inFamous v2.1 - Update complete",MB_OK|MB_ICONINFORMATION); }
        }
    } catch (const std::wstring& e) {
        if (argc==1) MessageBoxW(nullptr,e.c_str(),L"inFamous volume update",MB_OK|MB_ICONERROR);
        result=1;
    } catch (const std::exception& e) {
        if (argc==1) MessageBoxW(nullptr,utf8(e.what()).c_str(),L"inFamous volume update",MB_OK|MB_ICONERROR);
        result=1;
    }
    CoUninitialize(); LocalFree(argv); return result;
}
