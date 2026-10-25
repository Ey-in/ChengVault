using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

// Test infrastructure: create a private desktop and a kill-on-close process
// job. There is deliberately no SwitchDesktop entry point in this helper.
public static class IsolatedDesktop {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct StartupInfo {
        public uint cb; public string reserved; public string desktop; public string title;
        public uint x,y,width,height,xChars,yChars,fill,flags;
        public ushort show,reservedSize; public IntPtr reservedPointer,input,output,error;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct ProcessInfo { public IntPtr process,thread; public uint pid,tid; }
    [StructLayout(LayoutKind.Sequential)]
    struct BasicLimits {
        public long processTime,jobTime; public uint flags;
        public UIntPtr minimum,maximum; public uint processes; public UIntPtr affinity;
        public uint priority,scheduling;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct Counters { public ulong readOps,writeOps,otherOps,readBytes,writeBytes,otherBytes; }
    [StructLayout(LayoutKind.Sequential)]
    struct ExtendedLimits { public BasicLimits basic; public Counters io; public UIntPtr processMemory,jobMemory,peakProcess,peakJob; }
    [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern IntPtr CreateDesktopW(string name,IntPtr device,IntPtr mode,uint flags,uint access,IntPtr security);
    [DllImport("user32.dll",SetLastError=true)] public static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("user32.dll",SetLastError=true)] static extern IntPtr OpenInputDesktop(uint flags,bool inherit,uint access);
    [DllImport("user32.dll",SetLastError=true)] public static extern IntPtr GetThreadDesktop(uint thread);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern bool GetUserObjectInformationW(IntPtr handle,int index,StringBuilder buffer,uint bytes,out uint required);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern bool CreateProcessW(string application,StringBuilder command,IntPtr processSecurity,IntPtr threadSecurity,bool inherit,uint flags,IntPtr environment,string directory,ref StartupInfo startup,out ProcessInfo process);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateJobObjectW(IntPtr security,string name);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job,int type,ref ExtendedLimits limits,uint bytes);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
    [DllImport("kernel32.dll",SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool TerminateProcess(IntPtr process,uint exitCode);
    [DllImport("kernel32.dll",SetLastError=true)] public static extern bool CloseHandle(IntPtr handle);
    public static string Name(IntPtr desktop) {
        var name=new StringBuilder(512); uint needed;
        if(desktop==IntPtr.Zero || !GetUserObjectInformationW(desktop,2,name,1024,out needed)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return name.ToString();
    }
    public static string InputName() {
        var input=OpenInputDesktop(0,false,1);
        try { return Name(input); } finally { if(input!=IntPtr.Zero) CloseDesktop(input); }
    }
    public static IntPtr Create(string name) {
        // 0xFF grants object/window access; the switch-desktop right is absent.
        var desktop=CreateDesktopW(name,IntPtr.Zero,IntPtr.Zero,0,0xFF,IntPtr.Zero);
        if(desktop==IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        return desktop;
    }
    public static IntPtr Job() {
        var job=CreateJobObjectW(IntPtr.Zero,null);
        if(job==IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        var limits=new ExtendedLimits(); limits.basic.flags=0x2000;
        if(!SetInformationJobObject(job,9,ref limits,(uint)Marshal.SizeOf<ExtendedLimits>())) {
            var error=Marshal.GetLastWin32Error(); CloseHandle(job); throw new Win32Exception(error);
        }
        return job;
    }
    public static uint Launch(IntPtr job,string desktop,string application,string arguments,string directory) {
        var startup=new StartupInfo {cb=(uint)Marshal.SizeOf<StartupInfo>(),desktop="WinSta0\\"+desktop};
        ProcessInfo process;
        // Suspended creation lets us bind the exact process to the cleanup job
        // before it can launch children. No console window is ever created.
        if(!CreateProcessW(application,new StringBuilder("\""+application+"\" "+arguments),IntPtr.Zero,IntPtr.Zero,false,0x08000004,IntPtr.Zero,directory,ref startup,out process)) throw new Win32Exception(Marshal.GetLastWin32Error());
        try {
            if(!AssignProcessToJobObject(job,process.process)) {var error=Marshal.GetLastWin32Error(); TerminateProcess(process.process,1); throw new Win32Exception(error);}
            if(ResumeThread(process.thread)==0xFFFFFFFF) {var error=Marshal.GetLastWin32Error(); TerminateProcess(process.process,1); throw new Win32Exception(error);}
            return process.pid;
        } finally {CloseHandle(process.thread);CloseHandle(process.process);}
    }
}
