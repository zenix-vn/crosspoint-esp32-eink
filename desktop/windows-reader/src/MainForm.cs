using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace CrossPointReader;

internal sealed class MainForm : Form
{
    // A host name that never reaches the network: every request to it is
    // answered by OnWebResource, from the exe's embedded files or the API.
    public const string Origin = "https://app.crosspoint.internal/";

    private readonly WebView2 _web;
    private readonly AppSettings _settings;
    private readonly ApiRouter _router;
    private readonly string[] _args;
    private CoreWebView2Environment? _env;
    private FormWindowState _restoreState;
    private FormBorderStyle _restoreBorder;
    private bool _fullScreen;

    public MainForm(string[] args)
    {
        _args = args;
        _settings = AppSettings.Load();
        _settings.EnsureDevice();
        _router = new ApiRouter(this, _settings, new Library());

        Text = "Zenix CrossPoint Reader";
        // Sizes in logical units, scaled to the screen's DPI and kept on the screen.
        var area = Screen.FromPoint(Cursor.Position).WorkingArea;
        Size = new Size(Math.Min(LogicalToDeviceUnits(1360), (int)(area.Width * 0.9)),
                        Math.Min(LogicalToDeviceUnits(880), (int)(area.Height * 0.9)));
        MinimumSize = new Size(Math.Min(LogicalToDeviceUnits(900), area.Width), Math.Min(LogicalToDeviceUnits(600), area.Height));
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.FromArgb(246, 247, 251);
        using (var icon = Assembly.GetExecutingAssembly().GetManifestResourceStream("app.ico"))
        {
            if (icon != null) Icon = new Icon(icon);
        }

        _web = new WebView2 { Dock = DockStyle.Fill, DefaultBackgroundColor = BackColor };
        Controls.Add(_web);
        Load += async (_, _) => await InitAsync();
    }

    private async Task InitAsync()
    {
        try
        {
            Directory.CreateDirectory(AppPaths.WebView);
            _env = await CoreWebView2Environment.CreateAsync(null, AppPaths.WebView);
            await _web.EnsureCoreWebView2Async(_env);
        }
        catch (WebView2RuntimeNotFoundException)
        {
            if (MessageBox.Show(this, "Máy chưa có Microsoft Edge WebView2 Runtime. Mở trang tải về?", Text,
                    MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes)
                OpenExternal("https://developer.microsoft.com/microsoft-edge/webview2/");
            Close();
            return;
        }

        var core = _web.CoreWebView2;
        var devTools = Environment.GetEnvironmentVariable("CROSSPOINT_DEVTOOLS") == "1";
        core.Settings.AreDevToolsEnabled = devTools;
        core.Settings.AreDefaultContextMenusEnabled = devTools;
        core.Settings.IsStatusBarEnabled = false;
        core.Settings.IsZoomControlEnabled = false;
        core.Settings.IsGeneralAutofillEnabled = false;
        core.Settings.IsPasswordAutosaveEnabled = false;
        core.AddWebResourceRequestedFilter(Origin + "*", CoreWebView2WebResourceContext.All);
        core.WebResourceRequested += OnWebResource;
        core.NewWindowRequested += (_, e) =>
        {
            e.Handled = true;
            OpenExternal(e.Uri);
        };
        core.NavigationStarting += (_, e) =>
        {
            if (e.Uri.StartsWith(Origin, StringComparison.OrdinalIgnoreCase)) return;
            e.Cancel = true;
            if (e.Uri.StartsWith("http")) OpenExternal(e.Uri);
        };
        core.ContainsFullScreenElementChanged += (_, _) => SetFullScreen(core.ContainsFullScreenElement);

        // "Open with" an EPUB: import it and go straight to reading.
        var start = Origin + "index.html";
        var id = await ImportArg(_args.FirstOrDefault() ?? "");
        if (id != null) start += "#/read/" + id;
        core.Navigate(start);
        _ = Task.Run(ListenForOtherCopies);
    }

    private async Task<string?> ImportArg(string file)
    {
        if (!File.Exists(file) || !(file.EndsWith(".epub", StringComparison.OrdinalIgnoreCase) || file.EndsWith(".txt", StringComparison.OrdinalIgnoreCase)))
            return null;
        try
        {
            var book = await Task.Run(() => _router.Library.Add(File.ReadAllBytes(file), Path.GetFileName(file), "local"));
            return book.Id;
        }
        catch (Exception e)
        {
            MessageBox.Show(this, "Không mở được tệp: " + e.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return null;
        }
    }

    // A second launch (double click on a book) sends its file here.
    private async Task ListenForOtherCopies()
    {
        while (!IsDisposed)
        {
            try
            {
                using var pipe = new System.IO.Pipes.NamedPipeServerStream(Program.PipeName, System.IO.Pipes.PipeDirection.In, 1,
                    System.IO.Pipes.PipeTransmissionMode.Byte, System.IO.Pipes.PipeOptions.Asynchronous);
                await pipe.WaitForConnectionAsync();
                using var reader = new StreamReader(pipe);
                var file = await reader.ReadToEndAsync();
                BeginInvoke(async () =>
                {
                    if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
                    Activate();
                    var id = await ImportArg(file);
                    if (id != null && _web.CoreWebView2 != null)
                        await _web.CoreWebView2.ExecuteScriptAsync($"refreshBooks().then(() => {{ location.hash = '#/read/{id}'; }})");
                });
            }
            catch
            {
                await Task.Delay(1000);
            }
        }
    }

    private async void OnWebResource(object? sender, CoreWebView2WebResourceRequestedEventArgs e)
    {
        if (_env == null) return;
        var deferral = e.GetDeferral();
        try
        {
            var uri = new Uri(e.Request.Uri);
            var method = e.Request.Method;
            byte[]? body = null;
            if (e.Request.Content != null)
            {
                using var ms = new MemoryStream();
                await e.Request.Content.CopyToAsync(ms);
                body = ms.ToArray();
            }

            ApiResponse resp = uri.AbsolutePath.StartsWith("/api/", StringComparison.Ordinal)
                ? await Task.Run(() => _router.Handle(method, uri, body))
                : ServeStatic(uri.AbsolutePath);

            var headers = $"Content-Type: {resp.ContentType}\r\nCache-Control: no-store\r\nContent-Length: {resp.Body.Length}";
            e.Response = _env.CreateWebResourceResponse(new MemoryStream(resp.Body), resp.Status, resp.Status < 400 ? "OK" : "Error", headers);
        }
        catch (Exception ex)
        {
            e.Response = _env.CreateWebResourceResponse(new MemoryStream(ApiRouter.Utf8(ex.Message)), 500, "Error", "Content-Type: text/plain; charset=utf-8");
        }
        finally
        {
            deferral.Complete();
        }
    }

    private static readonly Dictionary<string, string> Types = new(StringComparer.OrdinalIgnoreCase)
    {
        [".html"] = "text/html; charset=utf-8", [".js"] = "text/javascript; charset=utf-8",
        [".css"] = "text/css; charset=utf-8", [".svg"] = "image/svg+xml", [".png"] = "image/png",
        [".ico"] = "image/x-icon", [".json"] = "application/json", [".woff2"] = "font/woff2",
    };

    // Resource names keep MSBuild's "\" for subfolders ("www/vendor\epub.min.js").
    private static readonly Dictionary<string, string> StaticFiles = Assembly.GetExecutingAssembly()
        .GetManifestResourceNames()
        .Where(n => n.StartsWith("www/", StringComparison.Ordinal))
        .ToDictionary(n => n[3..].Replace('\\', '/'), n => n, StringComparer.OrdinalIgnoreCase);

    private static ApiResponse ServeStatic(string path)
    {
        if (path == "/" || path.Length == 0) path = "/index.html";
        if (!StaticFiles.TryGetValue(Uri.UnescapeDataString(path), out var name))
            return new ApiResponse(404, "text/plain", ApiRouter.Utf8("Not found"));
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(name);
        if (stream == null) return new ApiResponse(404, "text/plain", ApiRouter.Utf8("Not found"));
        using var ms = new MemoryStream();
        stream.CopyTo(ms);
        return new ApiResponse(200, Types.GetValueOrDefault(Path.GetExtension(path), "application/octet-stream"), ms.ToArray());
    }

    public void ApplyTheme(bool dark)
    {
        if (InvokeRequired) { BeginInvoke(() => ApplyTheme(dark)); return; }
        var value = dark ? 1 : 0;
        _ = DwmSetWindowAttribute(Handle, 20, ref value, sizeof(int)); // DWMWA_USE_IMMERSIVE_DARK_MODE
        BackColor = dark ? Color.FromArgb(15, 17, 21) : Color.FromArgb(246, 247, 251);
        _web.DefaultBackgroundColor = BackColor;
    }

    // A picture of what the page shows now; the reader turns it like a sheet
    // of paper while the next page renders underneath.
    public Task<byte[]> Snapshot()
    {
        var done = new TaskCompletionSource<byte[]>();
        BeginInvoke(async () =>
        {
            try
            {
                using var ms = new MemoryStream();
                await _web.CoreWebView2.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png, ms);
                done.SetResult(ms.ToArray());
            }
            catch (Exception e)
            {
                done.SetException(e);
            }
        });
        return done.Task;
    }

    // The Windows folder picker, shown over the app window.
    public Task<string?> PickFolder(string? initial)
    {
        var done = new TaskCompletionSource<string?>();
        BeginInvoke(() =>
        {
            using var dlg = new FolderBrowserDialog
            {
                Description = "Chọn thư mục lưu bản dịch",
                UseDescriptionForTitle = true,
                InitialDirectory = Directory.Exists(initial) ? initial! : Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
            };
            done.SetResult(dlg.ShowDialog(this) == DialogResult.OK ? dlg.SelectedPath : null);
        });
        return done.Task;
    }

    // Closes the window shortly after the update has started, once the page has its reply.
    public void CloseForUpdate() => BeginInvoke(async () => { await Task.Delay(700); Close(); });

    public void SetFullScreen(bool on)
    {
        if (InvokeRequired) { BeginInvoke(() => SetFullScreen(on)); return; }
        if (on == _fullScreen) return;
        _fullScreen = on;
        if (on)
        {
            _restoreState = WindowState;
            _restoreBorder = FormBorderStyle;
            WindowState = FormWindowState.Normal;
            FormBorderStyle = FormBorderStyle.None;
            WindowState = FormWindowState.Maximized;
        }
        else
        {
            FormBorderStyle = _restoreBorder;
            WindowState = _restoreState;
        }
    }

    public static void OpenExternal(string url)
    {
        try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch { /* no browser */ }
    }

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);
}
