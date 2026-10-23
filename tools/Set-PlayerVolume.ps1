param(
    [Parameter(Mandatory = $true)][ValidateRange(0, 100)][int]$Percent,
    [string]$ProcessName = 'inFamousRecomp'
)
# Sets the Windows audio session volume (the per-app mixer slider) of a running player. System volume is untouched.
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace PlayerVolume {
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumerator {}
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator { int NotImpl1(); [PreserveSig] int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice dev); }
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice { [PreserveSig] int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o); }
[Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionManager2 { int NotImpl1(); int NotImpl2(); [PreserveSig] int GetSessionEnumerator(out IAudioSessionEnumerator e); }
[Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionEnumerator { [PreserveSig] int GetCount(out int n); [PreserveSig] int GetSession(int i, out IAudioSessionControl2 s); }
[Guid("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionControl2 { int n1(); int n2(); int n3(); int n4(); int n5(); int n6(); int n7(); int n8(); int n9(); int n10(); int n11();
    [PreserveSig] int GetProcessId(out uint pid); }
[Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface ISimpleAudioVolume { [PreserveSig] int SetMasterVolume(float v, ref Guid ctx); [PreserveSig] int GetMasterVolume(out float v); }
public static class Api {
    public static int Set(uint pid, float level) {
        var en = (IMMDeviceEnumerator)new MMDeviceEnumerator();
        IMMDevice dev; Marshal.ThrowExceptionForHR(en.GetDefaultAudioEndpoint(0, 1, out dev));
        Guid iid = typeof(IAudioSessionManager2).GUID; object o;
        Marshal.ThrowExceptionForHR(dev.Activate(ref iid, 23, IntPtr.Zero, out o));
        IAudioSessionEnumerator se; Marshal.ThrowExceptionForHR(((IAudioSessionManager2)o).GetSessionEnumerator(out se));
        int n; se.GetCount(out n); int hits = 0; Guid none = Guid.Empty;
        for (int i = 0; i < n; i++) {
            IAudioSessionControl2 s; se.GetSession(i, out s); uint p;
            if (s.GetProcessId(out p) == 0 && p == pid) { Marshal.ThrowExceptionForHR(((ISimpleAudioVolume)s).SetMasterVolume(level, ref none)); hits++; }
        }
        return hits;
    }
}
}
'@
foreach ($p in Get-Process -Name $ProcessName -ErrorAction Stop) {
    $n = [PlayerVolume.Api]::Set([uint32]$p.Id, $Percent / 100.0)
    Write-Output "PID $($p.Id): $n audio session(s) set to $Percent%"
}
