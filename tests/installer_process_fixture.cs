// A real Windows process boundary without the renderer, user config or AppX.
using System;
using System.IO;
using System.Runtime.InteropServices;

internal static class InstallerProcessFixture
{
    private delegate IntPtr WindowProc(IntPtr window, uint message, UIntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WindowClass
    {
        public uint Style;
        public WindowProc Procedure;
        public int ClassExtra, WindowExtra;
        public IntPtr Instance, Icon, Cursor, Background;
        public string MenuName, ClassName;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct Message
    {
        public IntPtr Window;
        public uint Id;
        public UIntPtr W;
        public IntPtr L;
        public uint Time;
        public int X, Y;
        public uint Private;
    }
    private static string mode;
    private static uint prepare;
    private static WindowProc procedure = Handle;
    private static IntPtr Handle(IntPtr window, uint message, UIntPtr w, IntPtr l)
    {
        if (message == prepare && mode.StartsWith("center"))
        {
            if (mode == "center-refuse") { return new IntPtr(2); }
            if (mode.StartsWith("center-legacy")) { return IntPtr.Zero; }
            if (mode == "center-timeout") { System.Threading.Thread.Sleep(2000); }
            if (mode != "center-saved-hung") { PostQuitMessage(0); }
            return new IntPtr(1);
        }
        if (message == 0x0010)
        {
            if (!mode.EndsWith("hung")) { PostQuitMessage(0); }
            return IntPtr.Zero;
        }
        return DefWindowProc(window, message, w, l);
    }
    public static int Main(string[] args)
    {
        mode = args[0];
        prepare = RegisterWindowMessage("BAFX.PrepareForMaintenance.v1");
        WindowClass type = new WindowClass();
        type.Procedure = procedure;
        type.ClassName = mode.StartsWith("center") ? "BAFX.NativeControlCenter.Window.v1" : "BaClickFxDesktopOverlay";
        if (RegisterClass(ref type) == 0) { return 2; }
        IntPtr window = CreateWindowEx(0, type.ClassName, "maintenance fixture", 0,
            0, 0, 10, 10, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (window == IntPtr.Zero) { return 3; }
        File.WriteAllText(args[1], "ready");
        Message message;
        while (GetMessage(out message, IntPtr.Zero, 0, 0) > 0)
        {
            TranslateMessage(ref message);
            DispatchMessage(ref message);
        }
        return 0;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "RegisterClassW")]
    private static extern ushort RegisterClass(ref WindowClass type);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "CreateWindowExW")]
    private static extern IntPtr CreateWindowEx(uint ex, string type, string title, uint style, int x, int y,
        int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "DefWindowProcW")]
    private static extern IntPtr DefWindowProc(IntPtr window, uint message, UIntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "RegisterWindowMessageW")]
    private static extern uint RegisterWindowMessage(string name);
    [DllImport("user32.dll", EntryPoint = "GetMessageW")]
    private static extern int GetMessage(out Message message, IntPtr window, uint min, uint max);
    [DllImport("user32.dll")]
    private static extern bool TranslateMessage(ref Message message);
    [DllImport("user32.dll", EntryPoint = "DispatchMessageW")]
    private static extern IntPtr DispatchMessage(ref Message message);
    [DllImport("user32.dll")]
    private static extern void PostQuitMessage(int code);
}
