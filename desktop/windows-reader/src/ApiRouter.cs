using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Web;

namespace CrossPointReader;

internal sealed record ApiResponse(int Status, string ContentType, byte[] Body)
{
    public static ApiResponse Json(object? value, int status = 200) =>
        new(status, "application/json; charset=utf-8", JsonSerializer.SerializeToUtf8Bytes(value, JsonOpts.Default));

    public static ApiResponse Error(string message, int status = 400) => Json(new { error = message }, status);

    public static ApiResponse Ok() => Json(new { ok = true });
}

// Everything the page asks of the app goes through https://<origin>/api/...
internal sealed class ApiRouter
{
    private readonly AppSettings _settings;
    private readonly Library _library;
    private readonly OpdsClient _opds;
    private readonly InboxClient _inbox;
    private readonly LibraryApi _api;
    private readonly Updater _updater;
    private readonly Translator _translator;
    private readonly MainForm _window;

    public ApiRouter(MainForm window, AppSettings settings, Library library)
    {
        _window = window;
        _settings = settings;
        _library = library;
        _opds = new OpdsClient(settings, library);
        _inbox = new InboxClient(settings, library);
        _api = new LibraryApi(settings, library);
        _updater = new Updater(settings, window);
        _translator = new Translator(settings, library);
    }

    public Library Library => _library;

    public async Task<ApiResponse> Handle(string method, Uri uri, byte[]? body)
    {
        try
        {
            return await Route(method, uri.AbsolutePath.TrimEnd('/'), HttpUtility.ParseQueryString(uri.Query), body);
        }
        catch (UserFacingException e)
        {
            return ApiResponse.Error(e.Message, e.Status);
        }
        catch (InvalidDataException e)
        {
            return ApiResponse.Error(e.Message);
        }
        catch (Exception e)
        {
            return ApiResponse.Error("Lỗi: " + e.Message, 500);
        }
    }

    private async Task<ApiResponse> Route(string method, string path, System.Collections.Specialized.NameValueCollection q, byte[]? body)
    {
        var parts = path.Split('/', StringSplitOptions.RemoveEmptyEntries); // api, area, ...
        var area = parts.Length > 1 ? parts[1] : "";
        var post = method == "POST";
        JsonObject Body() => body is { Length: > 0 } ? JsonNode.Parse(body) as JsonObject ?? new() : new();
        string Str(JsonObject o, string k) => o[k]?.GetValue<string>() ?? "";

        switch (area)
        {
            case "state":
                return ApiResponse.Json(State());

            case "settings" when post:
            {
                var b = Body();
                if (b["theme"] is JsonValue t) _settings.Theme = t.GetValue<string>();
                if (b["autoSync"] is JsonValue a) _settings.AutoSync = a.GetValue<bool>();
                if (b["autoUpdate"] is JsonValue au) _settings.AutoUpdate = au.GetValue<bool>();
                if (b["deviceName"] is JsonValue d) _settings.DeviceName = d.GetValue<string>().Trim();
                if (b["reader"] is JsonObject r) _settings.Reader = (JsonObject)r.DeepClone();
                _settings.Save();
                return ApiResponse.Json(State());
            }

            case "library":
                return await LibraryRoute(method, parts, q, body, Body);

            case "opds":
            {
                var action = parts.Length > 2 ? parts[2] : "";
                switch (action)
                {
                    case "login" when post:
                    {
                        var b = Body();
                        await _opds.Login(Str(b, "serverUrl").Length > 0 ? Str(b, "serverUrl") : _settings.CatalogUrl, Str(b, "email"), Str(b, "password"));
                        return ApiResponse.Json(State());
                    }
                    case "logout" when post:
                        _settings.Email = "";
                        _settings.PasswordData = "";
                        _settings.Save();
                        return ApiResponse.Json(State());
                    case "feed":
                        return ApiResponse.Json(await _opds.Feed(q["url"]));
                    case "image":
                    {
                        var (data, type) = await _opds.Image(q["url"] ?? throw new UserFacingException("Thiếu url"));
                        return new ApiResponse(200, type, data);
                    }
                    case "download" when post:
                    {
                        var b = Body();
                        var book = await _opds.Download(Str(b, "url"), Str(b, "type"), Str(b, "title"), Str(b, "author"), Str(b, "cover"));
                        return ApiResponse.Json(book);
                    }
                }
                break;
            }

            case "inbox":
            {
                var action = parts.Length > 2 ? parts[2] : "";
                switch (action)
                {
                    case "status":
                        return ApiResponse.Json(await _inbox.Status());
                    case "link" when post:
                        return ApiResponse.Json(await _inbox.Link(Str(Body(), "key")));
                    case "sync" when post:
                        return ApiResponse.Json(await _inbox.Sync());
                    case "reset" when post:
                        _settings.ResetDevice();
                        return ApiResponse.Json(State());
                }
                break;
            }

            case "account" when parts.Length > 2 && parts[2] == "devices":
                return ApiResponse.Json(await _api.Devices());

            case "dict":
                return ApiResponse.Json(await _api.Lookup(q["q"] ?? ""));

            case "words":
            {
                var action = parts.Length > 2 ? parts[2] : "";
                if (!post) return ApiResponse.Json(await _api.Words());
                var b = Body();
                return action switch
                {
                    "save" => ApiResponse.Json(await _api.SaveWord(b)),
                    "delete" => ApiResponse.Json(await _api.DeleteWord(Str(b, "word"))),
                    "send" => ApiResponse.Json(await _api.SendDeck(Str(b, "to"))),
                    _ => ApiResponse.Error("Không tìm thấy API", 404),
                };
            }

            case "translate":
                return await TranslateRoute(post, parts.Length > 2 ? parts[2] : "", q, Body);

            case "update":
            {
                var action = parts.Length > 2 ? parts[2] : "";
                if (action == "check") return ApiResponse.Json(await _updater.Check());
                if (action == "install" && post) return ApiResponse.Json(await _updater.Install());
                break;
            }

            case "window" when post:
            {
                var action = parts.Length > 2 ? parts[2] : "";
                if (action == "snapshot") return new ApiResponse(200, "image/png", await _window.Snapshot());
                var b = Body();
                if (action == "theme") _window.ApplyTheme(b["dark"]?.GetValue<bool>() ?? false);
                if (action == "fullscreen") _window.SetFullScreen(b["on"]?.GetValue<bool>() ?? false);
                return ApiResponse.Ok();
            }

            case "open-url" when post:
            {
                var url = Str(Body(), "url");
                if (Uri.TryCreate(url, UriKind.Absolute, out var u) && (u.Scheme == "https" || u.Scheme == "http")) MainForm.OpenExternal(u.ToString());
                return ApiResponse.Ok();
            }

            case "open-folder" when post:
                Process.Start(new ProcessStartInfo("explorer.exe", $"\"{AppPaths.Library}\"") { UseShellExecute = true });
                return ApiResponse.Ok();
        }
        return ApiResponse.Error("Không tìm thấy API", 404);
    }

    private async Task<ApiResponse> LibraryRoute(string method, string[] parts, System.Collections.Specialized.NameValueCollection q, byte[]? body, Func<JsonObject> json)
    {
        var post = method == "POST";
        if (parts.Length == 2) return ApiResponse.Json(_library.All().OrderByDescending(b => b.LastOpened ?? b.Added));

        if (parts[2] == "upload" && post)
        {
            if (body is not { Length: > 0 }) throw new UserFacingException("Tệp rỗng");
            var name = q["name"] ?? "book.epub";
            var data = body;
            var book = await Task.Run(() => _library.Add(data, name, "local"));
            return ApiResponse.Json(book);
        }

        var id = parts[2];
        var record = _library.Get(id);
        if (record == null) return ApiResponse.Error("Không có sách này trong thư viện", 404);
        var action = parts.Length > 3 ? parts[3] : "";

        switch (action)
        {
            case "":
                return ApiResponse.Json(record);
            case "file":
                return new ApiResponse(200, "application/epub+zip", await File.ReadAllBytesAsync(_library.BookFile(id)));
            case "cover":
                return File.Exists(_library.CoverFile(id))
                    ? new ApiResponse(200, "image/jpeg", await File.ReadAllBytesAsync(_library.CoverFile(id)))
                    : ApiResponse.Error("Không có ảnh bìa", 404);
            case "locations" when post:
                await File.WriteAllBytesAsync(_library.LocationsFile(id), body ?? Array.Empty<byte>());
                return ApiResponse.Ok();
            case "locations":
                return File.Exists(_library.LocationsFile(id))
                    ? new ApiResponse(200, "application/json", await File.ReadAllBytesAsync(_library.LocationsFile(id)))
                    : ApiResponse.Json(null);
            case "state" when post:
            {
                var b = json();
                var updated = _library.Update(id, r =>
                {
                    if (b["cfi"] is JsonValue c) r.Cfi = c.GetValue<string>();
                    if (b["progress"] is JsonValue p) r.Progress = Math.Clamp(p.GetValue<double>(), 0, 1);
                    if (b["chapter"] is JsonValue ch) r.Chapter = ch.GetValue<string>();
                    if (b["opened"] is JsonValue) r.LastOpened = Library.Now;
                    if (b["bookmarks"] is JsonArray marks) r.Bookmarks = marks.Deserialize<List<Bookmark>>(JsonOpts.Default) ?? new();
                });
                return ApiResponse.Json(updated);
            }
            case "send" when post:
                return ApiResponse.Json(await _api.SendBook(id, json()["to"]?.GetValue<string>() ?? ""));
            case "delete" when post:
                _library.Remove(id);
                return ApiResponse.Ok();
            case "reveal" when post:
                Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{_library.BookFile(id)}\"") { UseShellExecute = true });
                return ApiResponse.Ok();
        }
        return ApiResponse.Error("Không tìm thấy API", 404);
    }

    private async Task<ApiResponse> TranslateRoute(bool post, string action, System.Collections.Specialized.NameValueCollection q, Func<JsonObject> json)
    {
        switch (action)
        {
            case "status":
                return ApiResponse.Json(_translator.Status(int.TryParse(q["after"], out var a) ? a : 0));
            case "options":
            {
                var saved = _settings.Translate?.Deserialize<TranslateOptions>(JsonOpts.Default) ?? new TranslateOptions();
                if (saved.OutDir.Length == 0) saved.OutDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads", "Translated_Books");
                return ApiResponse.Json(saved);
            }
            case "info":
            {
                var book = _library.Get(q["id"] ?? "");
                return book == null ? ApiResponse.Error("Không có sách này", 404) : ApiResponse.Json(new { cached = Translator.CachedCount(book) });
            }
            case "start" when post:
                _translator.Start(json().Deserialize<TranslateOptions>(JsonOpts.Default) ?? new TranslateOptions());
                return ApiResponse.Ok();
            case "pause" when post:
                _translator.Pause(json()["pause"]?.GetValue<bool>() ?? true);
                return ApiResponse.Ok();
            case "stop" when post:
                _translator.Stop();
                return ApiResponse.Ok();
            case "clear-log" when post:
                _translator.ClearLog();
                return ApiResponse.Ok();
            case "test" when post:
                try
                {
                    return ApiResponse.Json(new { ok = true, text = await new GoogleTranslateClient().TestAsync() });
                }
                catch (GoogleHttpException e) when (e.Code == System.Net.HttpStatusCode.TooManyRequests)
                {
                    return ApiResponse.Json(new { ok = false, limited = true, error = "Google đang chặn tạm thời (HTTP 429). Đợi vài phút hoặc đổi mạng." });
                }
                catch (Exception e)
                {
                    return ApiResponse.Json(new { ok = false, error = e is GoogleHttpException h ? $"Google trả về lỗi HTTP {(int)h.Code}" : "Không kết nối được Google Dịch: " + e.Message });
                }
            case "pick-folder" when post:
                return ApiResponse.Json(new { path = await _window.PickFolder(json()["path"]?.GetValue<string>()) });
            case "open-folder" when post:
            {
                var path = json()["path"]?.GetValue<string>() ?? "";
                if (path.Length == 0) break;
                if (File.Exists(path)) Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"") { UseShellExecute = true });
                else { Directory.CreateDirectory(path); Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true }); }
                return ApiResponse.Ok();
            }
        }
        return ApiResponse.Error("Không tìm thấy API", 404);
    }

    private object State() => new
    {
        version = Http.Version,
        serverUrl = _settings.ServerBase,
        opdsUrl = _settings.CatalogUrl,
        email = _settings.Email,
        loggedIn = _settings.LoggedIn,
        theme = _settings.Theme,
        autoSync = _settings.AutoSync,
        autoUpdate = _settings.AutoUpdate,
        reader = _settings.Reader,
        deviceId = _settings.DeviceId,
        deviceName = _settings.EffectiveDeviceName,
        hasKey = _settings.InboxKey.Length > 0,
        received = _settings.Received.Take(20),
        dataDir = AppPaths.Root,
    };

    public static byte[] Utf8(string s) => Encoding.UTF8.GetBytes(s);
}
