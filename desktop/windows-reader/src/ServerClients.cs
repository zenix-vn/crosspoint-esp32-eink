using System.Net;
using System.Net.Http.Headers;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using System.Xml.Linq;

namespace CrossPointReader;

internal sealed class UserFacingException(string message, int status = 400) : Exception(message)
{
    public int Status { get; } = status;
}

internal static class Http
{
    public static readonly string Version =
        Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "1.0.0";

    public static readonly HttpClient Client = new(new SocketsHttpHandler
    {
        UseCookies = false,
        AutomaticDecompression = DecompressionMethods.All,
        PooledConnectionLifetime = TimeSpan.FromMinutes(5),
    })
    {
        Timeout = TimeSpan.FromMinutes(5),
        DefaultRequestHeaders = { { "User-Agent", $"CrossPointReader-Windows/{Version}" } },
    };

    public static async Task<HttpResponseMessage> Send(HttpRequestMessage req)
    {
        try
        {
            return await Client.SendAsync(req, HttpCompletionOption.ResponseHeadersRead);
        }
        catch (HttpRequestException e)
        {
            throw new UserFacingException("Không kết nối được máy chủ. Kiểm tra mạng Wi-Fi/Internet. (" + e.Message + ")", 502);
        }
        catch (TaskCanceledException)
        {
            throw new UserFacingException("Máy chủ không phản hồi (hết thời gian chờ).", 504);
        }
    }
}

// The library's OPDS 1.2 catalog: /library/opds.php with HTTP Basic credentials.
internal sealed class OpdsClient(AppSettings settings, Library library)
{
    private static readonly XNamespace Atom = "http://www.w3.org/2005/Atom";
    private static readonly XNamespace DcTerms = "http://purl.org/dc/terms/";
    private static readonly XNamespace Dc = "http://purl.org/dc/elements/1.1/";

    public string RootUrl => settings.CatalogUrl;

    private HttpRequestMessage Request(string url, string? email = null, string? password = null)
    {
        var req = new HttpRequestMessage(HttpMethod.Get, url);
        email ??= settings.Email;
        password ??= settings.Password;
        // Credentials only ever go to the configured server.
        if (email.Length > 0 && new Uri(url).Host.Equals(new Uri(settings.ServerBase).Host, StringComparison.OrdinalIgnoreCase))
        {
            var token = Convert.ToBase64String(Encoding.UTF8.GetBytes(email + ":" + password));
            req.Headers.Authorization = new AuthenticationHeaderValue("Basic", token);
        }
        return req;
    }

    private static void CheckStatus(HttpResponseMessage resp)
    {
        if (resp.IsSuccessStatusCode) return;
        throw resp.StatusCode switch
        {
            HttpStatusCode.Unauthorized => new UserFacingException("Tên đăng nhập hoặc mật khẩu không đúng.", 401),
            HttpStatusCode.Forbidden => new UserFacingException("Tài khoản này chưa được mở quyền xem thư viện chung.", 403),
            HttpStatusCode.NotFound => new UserFacingException("Không tìm thấy trên máy chủ (404).", 404),
            _ => new UserFacingException($"Máy chủ trả lỗi {(int)resp.StatusCode}.", 502),
        };
    }

    public async Task Login(string serverUrl, string email, string password)
    {
        var text = serverUrl.Trim();
        if (!text.Contains("://")) text = "https://" + text;
        if (!Uri.TryCreate(text, UriKind.Absolute, out var server) || (server.Scheme != "https" && server.Scheme != "http"))
            throw new UserFacingException("Địa chỉ OPDS không hợp lệ.");
        if (email.Trim().Length == 0) throw new UserFacingException("Hãy nhập tên đăng nhập.");

        // Either the catalog itself (…/library/opds.php) or the site root.
        var path = server.GetLeftPart(UriPartial.Path).TrimEnd('/');
        string root, catalog;
        if (path.EndsWith(".php", StringComparison.OrdinalIgnoreCase) || path.Contains("opds", StringComparison.OrdinalIgnoreCase))
        {
            catalog = server.ToString();
            var cut = path.IndexOf("/library/", StringComparison.OrdinalIgnoreCase);
            root = cut > 0 ? path[..cut] : server.GetLeftPart(UriPartial.Authority);
        }
        else
        {
            root = path.EndsWith("/library", StringComparison.OrdinalIgnoreCase) ? path[..^"/library".Length] : path;
            catalog = root + "/library/opds.php";
        }

        var (oldRoot, oldCatalog) = (settings.ServerUrl, settings.OpdsUrl);
        settings.ServerUrl = root;
        settings.OpdsUrl = catalog;
        try
        {
            using var resp = await Http.Send(Request(RootUrl, email.Trim(), password));
            CheckStatus(resp);
            var body = await resp.Content.ReadAsStringAsync();
            if (!body.Contains("<feed", StringComparison.Ordinal)) throw new UserFacingException("Địa chỉ này không phải danh mục OPDS.");
        }
        catch
        {
            (settings.ServerUrl, settings.OpdsUrl) = (oldRoot, oldCatalog); // a failed login changes nothing
            throw;
        }
        settings.Email = email.Trim();
        settings.Password = password;
        settings.Save();
    }

    public async Task<JsonObject> Feed(string? url)
    {
        if (!settings.LoggedIn) throw new UserFacingException("Hãy đăng nhập trước.", 401);
        url = string.IsNullOrEmpty(url) ? RootUrl : url;
        using var resp = await Http.Send(Request(url));
        CheckStatus(resp);
        var xml = XDocument.Parse(await resp.Content.ReadAsStringAsync());
        var feed = xml.Root ?? throw new UserFacingException("Danh mục OPDS rỗng.", 502);
        var baseUri = new Uri(url);

        string? Link(XElement parent, Func<string, string, bool> match)
        {
            var link = parent.Elements(Atom + "link").FirstOrDefault(l => match(l.Attribute("rel")?.Value ?? "", l.Attribute("type")?.Value ?? ""));
            var href = link?.Attribute("href")?.Value;
            return href == null ? null : Resolve(baseUri, href);
        }

        var entries = new JsonArray();
        foreach (var e in feed.Elements(Atom + "entry"))
        {
            var title = e.Element(Atom + "title")?.Value.Trim() ?? "";
            var acquisitions = new JsonArray();
            foreach (var l in e.Elements(Atom + "link"))
            {
                var rel = l.Attribute("rel")?.Value ?? "";
                var href = l.Attribute("href")?.Value;
                if (href != null && rel.StartsWith("http://opds-spec.org/acquisition"))
                    acquisitions.Add(new JsonObject { ["url"] = Resolve(baseUri, href), ["type"] = l.Attribute("type")?.Value ?? "" });
            }
            var summary = (e.Element(Atom + "summary") ?? e.Element(Atom + "content"))?.Value.Trim() ?? "";
            if (acquisitions.Count == 0)
            {
                var nav = Link(e, (rel, type) => type.Contains("atom+xml") || rel == "subsection");
                if (nav == null) continue;
                entries.Add(new JsonObject { ["kind"] = "nav", ["title"] = title, ["summary"] = summary, ["url"] = nav });
                continue;
            }
            var cover = Link(e, (rel, _) => rel == "http://opds-spec.org/image") ?? Link(e, (rel, _) => rel.Contains("opds-spec.org/image"));
            var thumb = Link(e, (rel, _) => rel == "http://opds-spec.org/image/thumbnail") ?? cover;
            var mainUrl = acquisitions[0]!["url"]!.GetValue<string>();
            var have = library.BySource(mainUrl);
            entries.Add(new JsonObject
            {
                ["kind"] = "book",
                ["id"] = e.Element(Atom + "id")?.Value ?? mainUrl,
                ["title"] = title,
                ["author"] = string.Join(", ", e.Elements(Atom + "author").Select(a => a.Element(Atom + "name")?.Value.Trim()).Where(s => !string.IsNullOrEmpty(s))),
                ["language"] = (e.Element(DcTerms + "language") ?? e.Element(Dc + "language"))?.Value ?? "",
                ["summary"] = summary,
                ["cover"] = cover,
                ["thumb"] = thumb,
                ["acquisitions"] = acquisitions,
                ["libraryId"] = have?.Id,
            });
        }

        var search = Link(feed, (rel, _) => rel == "search");
        if (search != null) search = search.Replace("%7BsearchTerms%7D", "{searchTerms}", StringComparison.OrdinalIgnoreCase);
        return new JsonObject
        {
            ["url"] = url,
            ["title"] = feed.Element(Atom + "title")?.Value ?? "",
            ["next"] = Link(feed, (rel, _) => rel == "next"),
            ["search"] = search,
            ["entries"] = entries,
        };
    }

    public async Task<(byte[] Data, string ContentType)> Image(string url)
    {
        Directory.CreateDirectory(AppPaths.ImageCache);
        var file = Path.Combine(AppPaths.ImageCache, Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(url))) + ".img");
        if (File.Exists(file)) return (await File.ReadAllBytesAsync(file), "image/jpeg");
        using var resp = await Http.Send(Request(url));
        CheckStatus(resp);
        var data = await resp.Content.ReadAsByteArrayAsync();
        await File.WriteAllBytesAsync(file, data);
        return (data, resp.Content.Headers.ContentType?.MediaType ?? "image/jpeg");
    }

    public async Task<BookRecord> Download(string url, string type, string? title, string? author, string? coverUrl)
    {
        var have = library.BySource(url);
        if (have != null) return have;
        using var resp = await Http.Send(Request(url));
        CheckStatus(resp);
        var data = await resp.Content.ReadAsByteArrayAsync();
        var name = resp.Content.Headers.ContentDisposition?.FileNameStar ?? resp.Content.Headers.ContentDisposition?.FileName?.Trim('"');
        var ext = type.Contains("text/plain") ? ".txt" : ".epub";
        if (string.IsNullOrEmpty(name)) name = (title ?? "book") + ext;
        if (!name.EndsWith(".epub", StringComparison.OrdinalIgnoreCase) && !name.EndsWith(".txt", StringComparison.OrdinalIgnoreCase)) name += ext;
        byte[]? cover = null;
        if (!string.IsNullOrEmpty(coverUrl))
        {
            try { cover = (await Image(coverUrl)).Data; } catch { /* the book matters, not its cover */ }
        }
        return library.Add(data, name, "opds", url, cover, title, author);
    }

    private static string Resolve(Uri baseUri, string href) =>
        new Uri(baseUri, href.Replace("{searchTerms}", "__SEARCH__")).ToString().Replace("__SEARCH__", "{searchTerms}");
}

// The book inbox (/inbox/api.php): this PC is a reader like the X4. It
// registers with its own id and secret and joins an account with the link key
// made on the library's Devices page.
internal sealed class InboxClient(AppSettings settings, Library library)
{
    private string Url => settings.ServerBase + "/inbox/api.php";

    private HttpRequestMessage Request(HttpMethod method, string query, HttpContent? content = null)
    {
        settings.EnsureDevice();
        var req = new HttpRequestMessage(method, Url + query) { Content = content };
        req.Headers.Add("X-Device-Id", settings.DeviceId);
        req.Headers.Add("X-Device-Secret", settings.DeviceSecret);
        req.Headers.Add("X-Device-Name", Uri.EscapeDataString(settings.EffectiveDeviceName));
        req.Headers.Add("X-Device-Model", "Windows PC");
        req.Headers.Add("X-Device-Firmware", "windows-" + Http.Version);
        if (settings.InboxKey.Length > 0) req.Headers.Add("X-Inbox-Key", settings.InboxKey);
        return req;
    }

    private async Task<JsonObject> Json(HttpRequestMessage req)
    {
        using var resp = await Http.Send(req);
        var text = await resp.Content.ReadAsStringAsync();
        JsonObject? body = null;
        try { body = JsonNode.Parse(text) as JsonObject; } catch { /* not JSON */ }
        if (resp.StatusCode == HttpStatusCode.Forbidden)
        {
            var err = body?["error"]?.GetValue<string>() ?? "";
            throw new UserFacingException(err == "device refused"
                ? "Máy chủ từ chối thiết bị này. Hãy xóa thiết bị trên web rồi bấm \"Đặt lại thiết bị\"."
                : "Thiết bị chưa được liên kết.", 403);
        }
        if (resp.StatusCode == HttpStatusCode.TooManyRequests) throw new UserFacingException("Nhập sai mã quá nhiều lần, hãy thử lại sau một giờ.", 429);
        if (!resp.IsSuccessStatusCode || body == null) throw new UserFacingException($"Hộp thư trả lỗi {(int)resp.StatusCode}.", 502);
        return body;
    }

    public async Task<JsonObject> Status()
    {
        var list = await Json(Request(HttpMethod.Get, "?action=list"));
        var pending = list["pending"]?.GetValue<bool>() ?? false;
        return new JsonObject
        {
            ["linked"] = !pending,
            ["badKey"] = list["bad_key"]?.GetValue<bool>() ?? false,
            ["waiting"] = list["books"]?.DeepClone() ?? new JsonArray(),
        };
    }

    public async Task<JsonObject> Link(string key)
    {
        settings.InboxKey = key.Trim();
        settings.Save();
        var status = await Status();
        if (status["badKey"]?.GetValue<bool>() == true) throw new UserFacingException("Mã liên kết không đúng hoặc đã hết hạn.");
        if (status["linked"]?.GetValue<bool>() != true) throw new UserFacingException("Chưa liên kết được, hãy kiểm tra lại mã.");
        return status;
    }

    public async Task<JsonObject> Sync()
    {
        var list = await Json(Request(HttpMethod.Get, "?action=list"));
        if (list["pending"]?.GetValue<bool>() == true) throw new UserFacingException("Thiết bị chưa được liên kết.", 403);
        var received = new JsonArray();
        var failed = 0;
        foreach (var node in list["books"]?.AsArray() ?? new JsonArray())
        {
            var id = node?["id"]?.GetValue<string>();
            var name = node?["name"]?.GetValue<string>() ?? id;
            if (string.IsNullOrEmpty(id) || string.IsNullOrEmpty(name)) continue;
            var item = new ReceivedItem { Name = name, At = Library.Now };
            try
            {
                using var resp = await Http.Send(Request(HttpMethod.Get, "?action=get&id=" + Uri.EscapeDataString(id)));
                if (!resp.IsSuccessStatusCode) throw new UserFacingException("tải thất bại");
                var data = await resp.Content.ReadAsByteArrayAsync();
                var ext = Path.GetExtension(name).ToLowerInvariant();
                if (ext is ".epub" or ".txt")
                {
                    item.BookId = library.Add(data, name, "inbox").Id;
                }
                else
                {
                    // Word lists, PDFs: kept as files, this app does not open them.
                    Directory.CreateDirectory(AppPaths.Downloads);
                    await File.WriteAllBytesAsync(FreePath(AppPaths.Downloads, name), data);
                    item.Status = "file";
                }
                // Deleting it there is the receipt, as on the reader.
                var form = new FormUrlEncodedContent(new Dictionary<string, string> { ["action"] = "delete", ["id"] = id });
                using var _ = await Http.Send(Request(HttpMethod.Post, "", form));
            }
            catch (Exception e)
            {
                item.Status = "error";
                item.Name += " — " + e.Message;
                failed++;
            }
            settings.Received.Insert(0, item);
            received.Add(new JsonObject { ["name"] = item.Name, ["bookId"] = item.BookId, ["status"] = item.Status });
        }
        if (settings.Received.Count > 50) settings.Received.RemoveRange(50, settings.Received.Count - 50);
        settings.Save();
        return new JsonObject { ["received"] = received, ["failed"] = failed };
    }

    private static string FreePath(string dir, string name)
    {
        var path = Path.Combine(dir, name);
        var stem = Path.GetFileNameWithoutExtension(name);
        var ext = Path.GetExtension(name);
        for (var n = 2; File.Exists(path); n++) path = Path.Combine(dir, $"{stem} ({n}){ext}");
        return path;
    }
}
