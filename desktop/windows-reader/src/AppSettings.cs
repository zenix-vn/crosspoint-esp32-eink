using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CrossPointReader;

internal static class AppPaths
{
    // CROSSPOINT_DATA_DIR gives a test run its own library and settings.
    public static readonly string Root = Environment.GetEnvironmentVariable("CROSSPOINT_DATA_DIR") is { Length: > 0 } dir
        ? dir
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CrossPointReader");

    public static string Library => Path.Combine(Root, "Library");
    public static string WebView => Path.Combine(Root, "WebView2");
    public static string ImageCache => Path.Combine(Root, "Cache", "images");
    public static string SettingsFile => Path.Combine(Root, "settings.json");
    public static string Downloads => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads", "CrossPoint");
}

internal static class JsonOpts
{
    public static readonly JsonSerializerOptions Default = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        WriteIndented = false,
    };

    public static readonly JsonSerializerOptions Indented = new(Default) { WriteIndented = true };
}

internal sealed class ReceivedItem
{
    public string Name { get; set; } = "";
    public string? BookId { get; set; }
    public long At { get; set; }
    public string Status { get; set; } = "ok";
}

internal sealed class AppSettings
{
    public const string DefaultServer = "https://crosspoint.zenix.vn";

    // Site root (the inbox lives at <root>/inbox/api.php) and the OPDS catalog URL.
    public string ServerUrl { get; set; } = DefaultServer;
    public string OpdsUrl { get; set; } = "";
    // The library login: its user name or email, both work on the server.
    public string Email { get; set; } = "";
    public string PasswordData { get; set; } = "";
    public string DeviceId { get; set; } = "";
    public string DeviceSecretData { get; set; } = "";
    public string InboxKey { get; set; } = "";
    public string DeviceName { get; set; } = "";
    public bool AutoSync { get; set; } = true;
    public bool AutoUpdate { get; set; } = true;
    public string Theme { get; set; } = "system";
    public JsonObject? Reader { get; set; }
    public List<ReceivedItem> Received { get; set; } = new();

    private static readonly object Gate = new();

    public static AppSettings Load()
    {
        try
        {
            if (File.Exists(AppPaths.SettingsFile))
            {
                var s = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(AppPaths.SettingsFile), JsonOpts.Default);
                if (s != null) return s;
            }
        }
        catch
        {
            // A damaged settings file starts over with defaults.
        }
        return new AppSettings();
    }

    public void Save()
    {
        lock (Gate)
        {
            Directory.CreateDirectory(AppPaths.Root);
            var tmp = AppPaths.SettingsFile + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(this, JsonOpts.Indented));
            File.Move(tmp, AppPaths.SettingsFile, true);
        }
    }

    public string ServerBase => (string.IsNullOrWhiteSpace(ServerUrl) ? DefaultServer : ServerUrl.Trim()).TrimEnd('/');

    public string CatalogUrl => string.IsNullOrWhiteSpace(OpdsUrl) ? ServerBase + "/library/opds.php" : OpdsUrl.Trim();

    public string Password
    {
        get => Unprotect(PasswordData);
        set => PasswordData = Protect(value);
    }

    public bool LoggedIn => Email.Length > 0 && PasswordData.Length > 0;

    public string DeviceSecret => Unprotect(DeviceSecretData);

    public string EffectiveDeviceName =>
        string.IsNullOrWhiteSpace(DeviceName) ? "Windows · " + Environment.MachineName : DeviceName.Trim();

    // The inbox knows readers by a 12-hex id (a MAC on the X4) and a 32-hex
    // secret. The id has the locally administered bit set so it never equals a
    // real reader's MAC.
    public void EnsureDevice()
    {
        if (DeviceId.Length == 12 && DeviceSecret.Length == 32) return;
        var id = RandomNumberGenerator.GetBytes(6);
        id[0] = (byte)((id[0] | 0x02) & 0xFE);
        DeviceId = Convert.ToHexString(id).ToLowerInvariant();
        DeviceSecretData = Protect(Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant());
        Save();
    }

    public void ResetDevice()
    {
        DeviceId = "";
        DeviceSecretData = "";
        InboxKey = "";
        EnsureDevice();
    }

    private static string Protect(string plain)
    {
        if (string.IsNullOrEmpty(plain)) return "";
        var data = ProtectedData.Protect(Encoding.UTF8.GetBytes(plain), null, DataProtectionScope.CurrentUser);
        return Convert.ToBase64String(data);
    }

    private static string Unprotect(string data)
    {
        if (string.IsNullOrEmpty(data)) return "";
        try
        {
            return Encoding.UTF8.GetString(ProtectedData.Unprotect(Convert.FromBase64String(data), null, DataProtectionScope.CurrentUser));
        }
        catch
        {
            return "";
        }
    }
}
