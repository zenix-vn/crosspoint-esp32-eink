using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

namespace CrossPointReader;

// The library's JSON API (/library/api.php): the account's readers, sending a
// book to one, the dictionary and the saved words. Same Basic credentials as OPDS.
internal sealed class LibraryApi(AppSettings settings, Library library)
{
    private readonly Dictionary<string, JsonObject> _lookupCache = new();

    private string Url => settings.ServerBase + "/library/api.php";

    private HttpRequestMessage Request(HttpMethod method, string action, string query = "", HttpContent? content = null)
    {
        if (!settings.LoggedIn) throw new UserFacingException("Hãy đăng nhập thư viện ở mục Khám phá trước.", 401);
        var req = new HttpRequestMessage(method, $"{Url}?a={action}{query}") { Content = content };
        var token = Convert.ToBase64String(Encoding.UTF8.GetBytes(settings.Email + ":" + settings.Password));
        req.Headers.Authorization = new AuthenticationHeaderValue("Basic", token);
        return req;
    }

    private static async Task<JsonObject> Json(HttpRequestMessage req)
    {
        using var resp = await Http.Send(req);
        var text = await resp.Content.ReadAsStringAsync();
        JsonObject? body = null;
        try { body = JsonNode.Parse(text) as JsonObject; } catch { /* not JSON */ }
        if (resp.StatusCode == HttpStatusCode.Unauthorized) throw new UserFacingException("Tên đăng nhập hoặc mật khẩu thư viện không còn đúng. Hãy đăng nhập lại.", 401);
        if (body == null)
        {
            throw resp.StatusCode == HttpStatusCode.NotFound
                ? new UserFacingException("Máy chủ chưa hỗ trợ tính năng này.", 404)
                : new UserFacingException($"Máy chủ trả lỗi {(int)resp.StatusCode}.", 502);
        }
        if (!resp.IsSuccessStatusCode) throw new UserFacingException(body["error"]?.GetValue<string>() ?? $"Lỗi {(int)resp.StatusCode}", (int)resp.StatusCode);
        return body;
    }

    // The account's readers, without this PC (it is one of them once linked).
    public async Task<JsonObject> Devices()
    {
        var me = await Json(Request(HttpMethod.Get, "me"));
        var list = new JsonArray();
        foreach (var d in me["devices"]?.AsArray() ?? new JsonArray())
        {
            if (d?["id"]?.GetValue<string>() == settings.DeviceId) continue;
            list.Add(d!.DeepClone());
        }
        return new JsonObject { ["name"] = me["name"]?.DeepClone(), ["devices"] = list };
    }

    public async Task<JsonObject> SendBook(string bookId, string to)
    {
        var book = library.Get(bookId) ?? throw new UserFacingException("Không có sách này trong thư viện.", 404);
        var name = book.Title + (book.Author.Length > 0 ? " - " + book.Author : "") + ".epub";
        var form = new MultipartFormDataContent
        {
            { new StringContent(to), "to" },
            { new StringContent(name), "name" },
        };
        var file = new ByteArrayContent(await File.ReadAllBytesAsync(library.BookFile(bookId)));
        file.Headers.ContentType = new MediaTypeHeaderValue("application/epub+zip");
        form.Add(file, "file", "book.epub");
        return await Json(Request(HttpMethod.Post, "send", content: form));
    }

    public async Task<JsonObject> Lookup(string word)
    {
        var key = word.Trim().ToLowerInvariant();
        if (_lookupCache.TryGetValue(key, out var hit)) return (JsonObject)hit.DeepClone();
        var result = await Json(Request(HttpMethod.Get, "lookup", "&q=" + Uri.EscapeDataString(word.Trim())));
        if (_lookupCache.Count > 500) _lookupCache.Clear();
        _lookupCache[key] = (JsonObject)result.DeepClone();
        return result;
    }

    public Task<JsonObject> Words() => Json(Request(HttpMethod.Get, "words"));

    public async Task<JsonObject> SaveWord(JsonObject w)
    {
        _lookupCache.Clear(); // "saved" marks change
        var form = new FormUrlEncodedContent(new Dictionary<string, string>
        {
            ["word"] = w["word"]?.GetValue<string>() ?? "",
            ["ipa"] = w["ipa"]?.GetValue<string>() ?? "",
            ["meaning"] = w["meaning"]?.GetValue<string>() ?? "",
            ["example"] = w["example"]?.GetValue<string>() ?? "",
        });
        return await Json(Request(HttpMethod.Post, "saveword", content: form));
    }

    public async Task<JsonObject> DeleteWord(string word)
    {
        _lookupCache.Clear();
        var form = new FormUrlEncodedContent(new Dictionary<string, string> { ["word"] = word });
        return await Json(Request(HttpMethod.Post, "deleteword", content: form));
    }

    public Task<JsonObject> SendDeck(string to) =>
        Json(Request(HttpMethod.Post, "senddeck", content: new FormUrlEncodedContent(new Dictionary<string, string> { ["to"] = to })));
}

// Updates from the server's Windows feed (/windows/latest.json), published by
// publish-windows.ps1 like the firmware's OTA feed:
//   {"version":"1.0.5","url":"CrossPointReader-Setup-1.0.5.msi","sha256":"…","size":…,"notes":"…","date":"…"}
internal sealed class Updater(AppSettings settings, MainForm window)
{
    private string FeedUrl => settings.ServerBase + "/windows/latest.json";

    private async Task<JsonObject?> Manifest()
    {
        using var resp = await Http.Send(new HttpRequestMessage(HttpMethod.Get, FeedUrl + "?t=" + DateTime.UtcNow.Ticks));
        if (resp.StatusCode == HttpStatusCode.NotFound) return null;
        if (!resp.IsSuccessStatusCode) throw new UserFacingException($"Không kiểm tra được bản cập nhật (lỗi {(int)resp.StatusCode}).", 502);
        return JsonNode.Parse(await resp.Content.ReadAsStringAsync()) as JsonObject;
    }

    private static Version Parse(string? v) => Version.TryParse((v ?? "").Split('-', '+')[0], out var p) ? p : new Version(0, 0);

    public async Task<JsonObject> Check()
    {
        var m = await Manifest();
        var latest = m?["version"]?.GetValue<string>();
        var available = latest != null && Parse(latest) > Parse(Http.Version);
        return new JsonObject
        {
            ["current"] = Http.Version,
            ["latest"] = latest,
            ["available"] = available,
            ["notes"] = m?["notes"]?.GetValue<string>() ?? "",
            ["size"] = m?["size"]?.GetValue<long>() ?? 0,
            ["date"] = m?["date"]?.GetValue<string>() ?? "",
        };
    }

    // Downloads the installer, checks it, then hands over to Windows Installer
    // and closes the app so its files can be replaced. The app restarts after.
    public async Task<JsonObject> Install()
    {
        var m = await Manifest() ?? throw new UserFacingException("Không tìm thấy bản cập nhật.", 404);
        var version = m["version"]?.GetValue<string>() ?? throw new UserFacingException("Thông tin cập nhật không hợp lệ.", 502);
        if (Parse(version) <= Parse(Http.Version)) throw new UserFacingException("Bạn đang dùng bản mới nhất.");
        var url = new Uri(new Uri(FeedUrl), m["url"]?.GetValue<string>() ?? throw new UserFacingException("Thiếu đường dẫn tải.", 502));
        using var resp = await Http.Send(new HttpRequestMessage(HttpMethod.Get, url));
        if (!resp.IsSuccessStatusCode) throw new UserFacingException($"Tải bản cập nhật thất bại (lỗi {(int)resp.StatusCode}).", 502);
        var data = await resp.Content.ReadAsByteArrayAsync();
        var expected = m["sha256"]?.GetValue<string>();
        if (!string.IsNullOrEmpty(expected) && !Convert.ToHexString(SHA256.HashData(data)).Equals(expected, StringComparison.OrdinalIgnoreCase))
            throw new UserFacingException("Tệp cập nhật bị lỗi khi tải, hãy thử lại.", 502);

        var msi = Path.Combine(Path.GetTempPath(), $"CrossPointReader-Setup-{version}.msi");
        await File.WriteAllBytesAsync(msi, data);
        var installed = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Zenix CrossPoint Reader", "CrossPointReader.exe");
        var relaunch = File.Exists(installed) ? installed : Environment.ProcessPath ?? installed;
        // Windows Installer asks for permission itself, shows its progress, then the app opens again.
        var script = $"/c msiexec /i \"{msi}\" /passive /norestart && start \"\" \"{relaunch}\"";
        Process.Start(new ProcessStartInfo("cmd.exe", script) { UseShellExecute = false, CreateNoWindow = true });
        window.CloseForUpdate();
        return new JsonObject { ["ok"] = true, ["version"] = version };
    }
}
