using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace CrossPointReader;

internal static class Program
{
    // One running copy per data folder: opening a book (double click on an
    // .epub) while the app runs hands the file to that window.
    private static readonly string InstanceKey = "CrossPointReader-" +
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(AppPaths.Root.ToLowerInvariant())))[..16];

    public static string PipeName => InstanceKey + "-open";

    [STAThread]
    private static void Main(string[] args)
    {
        using var mutex = new Mutex(true, InstanceKey, out var first);
        if (!first)
        {
            HandOver(args.FirstOrDefault() ?? "");
            return;
        }
        Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm(args));
    }

    private static void HandOver(string file)
    {
        try
        {
            AllowSetForegroundWindow(-1); // ASFW_ANY: let the running window come to the front
            using var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.Out);
            pipe.Connect(3000);
            using var writer = new StreamWriter(pipe, new UTF8Encoding(false));
            writer.Write(file.Length > 0 ? Path.GetFullPath(file) : "");
        }
        catch
        {
            // The first copy is still starting or closing; nothing more to do.
        }
    }

    [DllImport("user32.dll")]
    private static extern bool AllowSetForegroundWindow(int processId);
}
