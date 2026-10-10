# Mute (or -Unmute) PenguinScreen2's audio session(s) in the Windows mixer, on every active output device.
param([switch]$Unmute)
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumeratorM { [PreserveSig] int EnumAudioEndpoints(int dataFlow, int dwStateMask, out IMMDeviceCollectionM ppDevices); }
[Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceCollectionM { [PreserveSig] int GetCount(out uint pcDevices); [PreserveSig] int Item(uint nDevice, out IMMDeviceM ppDevice); }
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceM { [PreserveSig] int Activate(ref Guid iid, int dwClsCtx, IntPtr pActivationParams, [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface); }
[Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionManager2M { int NotImpl1(); int NotImpl2(); [PreserveSig] int GetSessionEnumerator(out IAudioSessionEnumeratorM SessionEnum); }
[Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionEnumeratorM { [PreserveSig] int GetCount(out int SessionCount); [PreserveSig] int GetSession(int SessionCount, out IAudioSessionControl2M Session); }
[Guid("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionControl2M { int n1(); int n2(); int n3(); int n4(); int n5(); int n6(); int n7(); int n8(); int n9(); int n10(); int n11();
 [PreserveSig] int GetProcessId(out uint pRetVal); }
[Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface ISimpleAudioVolumeM { int SetMasterVolume(float l, ref Guid c); int GetMasterVolume(out float l); [PreserveSig] int SetMute(bool m, ref Guid c); [PreserveSig] int GetMute(out bool m); }
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumeratorCoM {}
public static class AppMuteM {
  public static int Set(uint pid, bool mute) {
    var en = (IMMDeviceEnumeratorM)(new MMDeviceEnumeratorCoM()); IMMDeviceCollectionM col; en.EnumAudioEndpoints(0, 1, out col);
    uint nd; col.GetCount(out nd); int hits = 0; var g = Guid.Empty;
    for (uint d = 0; d < nd; d++) { IMMDeviceM dev; col.Item(d, out dev);
      var iid = typeof(IAudioSessionManager2M).GUID; object o; if (dev.Activate(ref iid, 23, IntPtr.Zero, out o) != 0) continue;
      IAudioSessionEnumeratorM se; ((IAudioSessionManager2M)o).GetSessionEnumerator(out se); int n; se.GetCount(out n);
      for (int i = 0; i < n; i++) { IAudioSessionControl2M c; se.GetSession(i, out c); uint p; c.GetProcessId(out p);
        if (p == pid) { ((ISimpleAudioVolumeM)c).SetMute(mute, ref g); hits++; } } }
    return hits; } }
"@
foreach ($p in Get-Process pcsx2-qt -ErrorAction SilentlyContinue) { "pid $($p.Id): " + [AppMuteM]::Set([uint32]$p.Id, -not $Unmute) + " session(s) " + $(if ($Unmute) { 'unmuted' } else { 'muted' }) }
