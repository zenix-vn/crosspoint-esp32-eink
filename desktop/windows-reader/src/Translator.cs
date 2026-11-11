using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace CrossPointReader;

// English -> Vietnamese book translation through the free Google Translate endpoint,
// producing a Vietnamese copy and/or a bilingual copy (each English paragraph followed
// by its translation). Ported from epub_translator_gui.py.

internal sealed class RateLimitedException(string message) : Exception(message);

internal sealed class GoogleHttpException(HttpStatusCode code, TimeSpan? retryAfter)
    : Exception($"HTTP {(int)code}")
{
    public HttpStatusCode Code { get; } = code;
    public TimeSpan? RetryAfter { get; } = retryAfter;
}

// Paces requests (delay + random jitter between batches), backs off on HTTP 429 with a
// cooldown shared by every worker, and splits a batch when Google merges or splits lines.
internal sealed class GoogleTranslateClient
{
    private const string Endpoint = "https://translate.googleapis.com/translate_a/single?client=gtx&sl=en&tl=vi&dt=t";

    private static readonly string[] UserAgents =
    [
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36",
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:127.0) Gecko/20100101 Firefox/127.0",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
    ];

    private static readonly HttpClient Client = new(new SocketsHttpHandler
    {
        UseCookies = false,
        AutomaticDecompression = DecompressionMethods.All,
        PooledConnectionLifetime = TimeSpan.FromMinutes(5),
    })
    { Timeout = TimeSpan.FromSeconds(25) };

    private readonly double _delay;
    private readonly double _jitter;
    private readonly int _maxRetries;
    private readonly Action<string, string>? _log;
    private readonly Action<string>? _status;
    private readonly ManualResetEventSlim? _running; // reset = paused
    private readonly CancellationToken _stop;

    private readonly object _gate = new();
    private DateTime _nextAllowed = DateTime.MinValue;
    private DateTime _cooldownUntil = DateTime.MinValue;

    public GoogleTranslateClient(double delay = 2.0, double jitter = 0.8, int maxRetries = 6,
        Action<string, string>? log = null, Action<string>? status = null,
        ManualResetEventSlim? running = null, CancellationToken stop = default)
    {
        _delay = Math.Max(0, delay);
        _jitter = Math.Max(0, jitter);
        _maxRetries = Math.Max(1, maxRetries);
        _log = log;
        _status = status;
        _running = running;
        _stop = stop;
    }

    // Blocks while paused; throws once a stop was asked for.
    private void WaitIfPaused()
    {
        _stop.ThrowIfCancellationRequested();
        if (_running == null || _running.IsSet) return;
        try { _running.Wait(_stop); }
        catch (OperationCanceledException) { throw new OperationCanceledException("Đã dừng theo yêu cầu", _stop); }
    }

    private async Task SleepAsync(double seconds, string? reason = null)
    {
        var end = DateTime.UtcNow.AddSeconds(seconds);
        while (true)
        {
            WaitIfPaused();
            var left = end - DateTime.UtcNow;
            if (left <= TimeSpan.Zero) return;
            if (reason != null) _status?.Invoke($"{reason} (còn {(int)left.TotalSeconds + 1}s…)");
            await Task.Delay(TimeSpan.FromMilliseconds(Math.Clamp(left.TotalMilliseconds, 50, 500)), _stop);
        }
    }

    private async Task PaceAsync()
    {
        TimeSpan wait;
        lock (_gate)
        {
            var now = DateTime.UtcNow;
            var target = _nextAllowed > _cooldownUntil ? _nextAllowed : _cooldownUntil;
            wait = target > now ? target - now : TimeSpan.Zero;
            var step = _delay + Random.Shared.NextDouble() * _jitter;
            _nextAllowed = (target > now ? target : now).AddSeconds(step);
        }
        if (wait > TimeSpan.Zero) await SleepAsync(wait.TotalSeconds);
        WaitIfPaused();
    }

    private void Cooldown(double seconds)
    {
        lock (_gate)
        {
            var until = DateTime.UtcNow.AddSeconds(seconds);
            if (until > _cooldownUntil) _cooldownUntil = until;
        }
    }

    private async Task<string> RequestAsync(string text, CancellationToken token)
    {
        using var req = new HttpRequestMessage(HttpMethod.Post, Endpoint)
        {
            Content = new FormUrlEncodedContent([new KeyValuePair<string, string>("q", text)]),
        };
        req.Headers.TryAddWithoutValidation("User-Agent", UserAgents[Random.Shared.Next(UserAgents.Length)]);
        using var res = await Client.SendAsync(req, token);
        if (!res.IsSuccessStatusCode)
        {
            var retry = res.Headers.RetryAfter?.Delta;
            throw new GoogleHttpException(res.StatusCode, retry);
        }
        var root = JsonNode.Parse(await res.Content.ReadAsStringAsync(token));
        if (root is not JsonArray arr || arr.Count == 0 || arr[0] is not JsonArray parts)
            throw new InvalidDataException("Phản hồi Google Dịch rỗng hoặc sai định dạng");
        var sb = new StringBuilder();
        foreach (var part in parts)
            if (part is JsonArray p && p.Count > 0 && p[0] is JsonValue v && v.TryGetValue<string>(out var s)) sb.Append(s);
        return sb.ToString();
    }

    // One light request: "Hello world" -> Vietnamese.
    public async Task<string> TestAsync() => (await RequestAsync("Hello world", CancellationToken.None)).Trim();

    // Translates lines one-to-one; throws on persistent failure instead of returning English.
    public async Task<List<string>> TranslateLinesAsync(List<string> lines)
    {
        if (lines.Count == 0) return [];
        var joined = string.Join("\n", lines);
        Exception? last = null;
        for (var attempt = 0; attempt < _maxRetries; attempt++)
        {
            await PaceAsync();
            try
            {
                var translated = await RequestAsync(joined, _stop);
                var outLines = translated.Split('\n').Select(s => s.Trim()).ToList();
                if (outLines.Count == lines.Count) return outLines;
                if (lines.Count > 1)
                {
                    // Google merged or split lines: halve the batch until every paragraph lines up.
                    _log?.Invoke($"Số dòng trả về lệch ({outLines.Count}/{lines.Count}), tự tách nhỏ batch để giữ khớp từng đoạn.", "warn");
                    var mid = lines.Count / 2;
                    var first = await TranslateLinesAsync(lines.GetRange(0, mid));
                    first.AddRange(await TranslateLinesAsync(lines.GetRange(mid, lines.Count - mid)));
                    return first;
                }
                return [string.Join(" ", outLines).Trim()];
            }
            catch (OperationCanceledException) when (_stop.IsCancellationRequested)
            {
                throw;
            }
            catch (GoogleHttpException e) when (e.Code == HttpStatusCode.TooManyRequests)
            {
                last = e;
                var baseWait = e.RetryAfter?.TotalSeconds ?? Math.Min(120, 12 * Math.Pow(2, attempt));
                var wait = baseWait + 1 + Random.Shared.NextDouble() * 4;
                Cooldown(wait);
                _log?.Invoke($"Gặp HTTP 429 (Google giới hạn tốc độ) ở lần thử {attempt + 1}/{_maxRetries}. Tạm nghỉ {wait:0}s…", "warn");
                await SleepAsync(wait, "Tạm nghỉ chống chặn 429");
            }
            catch (Exception e)
            {
                last = e;
                var wait = Math.Min(30, 3 * (attempt + 1));
                _log?.Invoke(e is GoogleHttpException h
                    ? $"Lỗi HTTP {(int)h.Code}, thử lại sau {wait}s…"
                    : $"Lỗi kết nối ({e.Message}), thử lại sau {wait}s…", "warn");
                await SleepAsync(wait);
            }
        }
        if (last is GoogleHttpException { Code: HttpStatusCode.TooManyRequests })
            throw new RateLimitedException("Google Dịch tiếp tục trả về HTTP 429 sau nhiều lần chờ.");
        throw new InvalidOperationException($"Không dịch được batch sau {_maxRetries} lần thử: {last?.Message}");
    }
}

internal sealed class TranslateOptions
{
    public string BookId { get; set; } = "";
    public bool MakeVi { get; set; } = true;
    public bool MakeBi { get; set; } = true;
    public bool Resume { get; set; } = true;
    public string Preset { get; set; } = "safe";
    public int Workers { get; set; } = 1;
    public double Delay { get; set; } = 2.0;
    public double Jitter { get; set; } = 0.8;
    public int BatchChars { get; set; } = 3800;
    public int Retries { get; set; } = 6;
    public bool SaveCopy { get; set; }
    public bool CopyOriginal { get; set; }
    public string OutDir { get; set; } = "";

    public void Clamp()
    {
        Workers = Math.Clamp(Workers, 1, 4);
        Delay = Math.Clamp(Delay, 0, 15);
        Jitter = Math.Clamp(Jitter, 0, 5);
        BatchChars = Math.Clamp(BatchChars, 500, 4500);
        Retries = Math.Clamp(Retries, 1, 12);
    }
}

internal sealed record TranslateLog(int Seq, long At, string Level, string Text);

internal sealed record TranslateOutput(string Kind, string BookId, string Title, string? File);

// One translation at a time; the page polls Status() for progress and new log lines.
internal sealed class Translator
{
    private static readonly Regex Block = new(@"(<(p|blockquote)\b[^>]*>)(.*?)(</\2>)",
        RegexOptions.IgnoreCase | RegexOptions.Singleline | RegexOptions.Compiled);
    private static readonly Regex Tag = new("<[^>]+>", RegexOptions.Compiled);
    private static readonly Regex Space = new(@"\s+", RegexOptions.Compiled);
    private static readonly Regex EmptyAnchor = new(@"<a\b[^>]*></a>", RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private static readonly Regex IdAttr = new(@"\s+id\s*=\s*(""[^""]*""|'[^']*')", RegexOptions.IgnoreCase | RegexOptions.Compiled);

    private readonly Library _library;
    private readonly AppSettings _settings;
    private readonly object _gate = new();
    private readonly List<TranslateLog> _log = new();
    private int _seq;

    private CancellationTokenSource? _cts;
    private ManualResetEventSlim _running = new(true);
    private Task? _task;

    // Live state, read by Status().
    private string _bookId = "", _title = "", _status = "Sẵn sàng.";
    private int _chapters, _chaptersDone, _paras, _failed;
    private long _started, _ended;
    private bool? _success;
    private string _summary = "";
    private List<TranslateOutput> _outputs = new();

    public Translator(AppSettings settings, Library library)
    {
        _settings = settings;
        _library = library;
    }

    public bool Busy => _task is { IsCompleted: false };

    public static string CacheFile(BookRecord book) =>
        Path.Combine(AppPaths.Root, "Cache", "translate", (book.Hash.Length >= 16 ? book.Hash[..16] : book.Id) + ".json");

    public static int CachedCount(BookRecord book)
    {
        try
        {
            var f = CacheFile(book);
            return File.Exists(f) ? (JsonNode.Parse(File.ReadAllText(f)) as JsonObject)?.Count ?? 0 : 0;
        }
        catch { return 0; }
    }

    private void Log(string text, string level = "info")
    {
        lock (_gate)
        {
            _log.Add(new TranslateLog(++_seq, Library.Now, level, text));
            if (_log.Count > 600) _log.RemoveRange(0, _log.Count - 600);
        }
    }

    private void SetStatus(string s) { lock (_gate) _status = s; }

    public object Status(int after)
    {
        lock (_gate)
        {
            return new
            {
                running = Busy,
                paused = Busy && !_running.IsSet,
                bookId = _bookId,
                title = _title,
                status = _status,
                chapters = _chapters,
                chaptersDone = _chaptersDone,
                paras = _paras,
                failed = _failed,
                started = _started,
                elapsed = _started == 0 ? 0 : ((_ended > 0 ? _ended : Library.Now) - _started) / 1000,
                success = _success,
                summary = _summary,
                outputs = _outputs,
                seq = _seq,
                log = _log.Where(l => l.Seq > after).ToList(),
            };
        }
    }

    public void Pause(bool pause)
    {
        if (!Busy) return;
        if (pause)
        {
            _running.Reset();
            SetStatus("Đang tạm dừng (bấm Tiếp tục để chạy tiếp)…");
            Log("Đã tạm dừng sau yêu cầu hiện tại.", "warn");
        }
        else
        {
            _running.Set();
            Log("Tiếp tục dịch…");
        }
    }

    public void Stop()
    {
        if (!Busy) return;
        _cts?.Cancel();
        _running.Set();
        SetStatus("Đang dừng…");
        Log("Đã yêu cầu dừng. Phần đã dịch được giữ lại để dịch tiếp lần sau.", "warn");
    }

    public void ClearLog() { lock (_gate) _log.Clear(); }

    public void Start(TranslateOptions o)
    {
        if (Busy) throw new UserFacingException("Đang dịch một cuốn sách khác.");
        o.Clamp();
        var book = _library.Get(o.BookId) ?? throw new UserFacingException("Không có sách này trong thư viện", 404);
        if (!o.MakeVi && !o.MakeBi) throw new UserFacingException("Hãy chọn ít nhất một bản xuất: tiếng Việt hoặc song ngữ.");
        if (o.SaveCopy && string.IsNullOrWhiteSpace(o.OutDir)) throw new UserFacingException("Hãy chọn thư mục lưu bản sao.");

        _settings.Translate = JsonSerializer.SerializeToNode(o, JsonOpts.Default) as JsonObject;
        _settings.Save();

        lock (_gate)
        {
            _bookId = book.Id; _title = book.Title; _status = "Đang chuẩn bị…";
            _chapters = _chaptersDone = _paras = _failed = 0;
            _started = Library.Now; _ended = 0; _success = null; _summary = ""; _outputs = new();
        }
        _cts = new CancellationTokenSource();
        _running = new ManualResetEventSlim(true);
        var token = _cts.Token;
        _task = Task.Run(async () =>
        {
            try
            {
                await Run(book, o, token);
            }
            catch (OperationCanceledException)
            {
                Finish(false, $"Đã dừng giữa chừng ({_chaptersDone}/{_chapters} chương). Phần đã dịch được lưu để dịch tiếp lần sau.");
            }
            catch (Exception e)
            {
                Log("Lỗi nghiêm trọng: " + e.Message, "error");
                Finish(false, "Lỗi: " + e.Message);
            }
        });
    }

    private void Finish(bool ok, string summary)
    {
        lock (_gate)
        {
            _success = ok;
            _summary = summary;
            _status = summary;
            _ended = Library.Now;
        }
        Log(summary, ok ? "success" : "warn");
    }

    private static bool IsHtml(string name) =>
        name.EndsWith(".html", StringComparison.OrdinalIgnoreCase) ||
        name.EndsWith(".xhtml", StringComparison.OrdinalIgnoreCase) ||
        name.EndsWith(".htm", StringComparison.OrdinalIgnoreCase);

    private static string CleanText(string inner) =>
        Space.Replace(WebUtility.HtmlDecode(Tag.Replace(inner, "")), " ").Trim();

    private static string Md5(string s) => Convert.ToHexString(MD5.HashData(Encoding.UTF8.GetBytes(s))).ToLowerInvariant();

    private async Task Run(BookRecord book, TranslateOptions o, CancellationToken token)
    {
        var src = await File.ReadAllBytesAsync(_library.BookFile(book.Id), token);

        // Entries in their original order; chapters as text, everything else as bytes.
        var entries = new List<(string Name, byte[] Data)>();
        using (var zip = new ZipArchive(new MemoryStream(src), ZipArchiveMode.Read))
        {
            foreach (var e in zip.Entries)
            {
                if (e.FullName.EndsWith('/')) continue;
                using var s = e.Open();
                using var ms = new MemoryStream();
                await s.CopyToAsync(ms, token);
                entries.Add((e.FullName, ms.ToArray()));
            }
        }
        var chapters = entries.Where(e => IsHtml(e.Name)).ToList();

        var cacheFile = CacheFile(book);
        var cache = new Dictionary<string, string>();
        if (o.Resume && File.Exists(cacheFile))
        {
            try
            {
                cache = JsonSerializer.Deserialize<Dictionary<string, string>>(await File.ReadAllTextAsync(cacheFile, token)) ?? new();
                if (cache.Count > 0) Log($"Tìm thấy {cache.Count} đoạn đã dịch từ lần trước. Sẽ dịch tiếp phần còn lại.", "success");
            }
            catch { cache = new(); }
        }
        var cacheGate = new object();
        void SaveCache()
        {
            lock (cacheGate)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(cacheFile)!);
                File.WriteAllText(cacheFile + ".tmp", JsonSerializer.Serialize(cache));
                File.Move(cacheFile + ".tmp", cacheFile, true);
            }
        }

        var client = new GoogleTranslateClient(o.Delay, o.Jitter, o.Retries, (m, l) => Log(m, l), SetStatus, _running, token);
        lock (_gate) _chapters = chapters.Count;
        Log($"Bắt đầu dịch “{book.Title}” ({chapters.Count} chương) — {o.Workers} luồng, nghỉ {o.Delay}s ± {o.Jitter}s.");

        var vi = new Dictionary<string, byte[]>();
        var bi = new Dictionary<string, byte[]>();
        await Parallel.ForEachAsync(chapters, new ParallelOptions { MaxDegreeOfParallelism = o.Workers, CancellationToken = token }, async (ch, ct) =>
        {
            var (viHtml, biHtml, ok, fail) = await TranslateChapter(ch.Name, Encoding.UTF8.GetString(ch.Data), client, cache, cacheGate, o, ct);
            if (o.Resume && ok > 0) SaveCache();
            lock (_gate)
            {
                vi[ch.Name] = Encoding.UTF8.GetBytes(viHtml);
                bi[ch.Name] = Encoding.UTF8.GetBytes(biHtml);
                _chaptersDone++;
                _paras += ok;
                _failed += fail;
                _status = $"Đang dịch: {Path.GetFileName(ch.Name)} ({_chaptersDone}/{_chapters})";
            }
        });
        token.ThrowIfCancellationRequested();

        var opfName = entries.FirstOrDefault(e => e.Name.EndsWith(".opf", StringComparison.OrdinalIgnoreCase)).Name;
        var ncxName = entries.FirstOrDefault(e => e.Name.EndsWith(".ncx", StringComparison.OrdinalIgnoreCase)).Name;
        string Text(string? n) => n == null ? "" : Encoding.UTF8.GetString(entries.First(e => e.Name == n).Data);
        var opf = Text(opfName);
        var ncx = Text(ncxName);

        var baseName = SafeFileName(book.Title);
        var author = book.Author.Length > 0 ? SafeFileName(book.Author) : "";
        if (author.Length > 40) author = author[..40].Trim(' ', '.');
        var suffix = author.Length > 0 && !baseName.Contains(author, StringComparison.OrdinalIgnoreCase) ? $" - {author}" : "";

        var outputs = new List<TranslateOutput>();
        if (o.SaveCopy) Directory.CreateDirectory(o.OutDir);
        if (o.SaveCopy && o.CopyOriginal)
        {
            var en = Path.Combine(o.OutDir, $"{baseName} (Bản Tiếng Anh){suffix}.epub");
            await File.WriteAllBytesAsync(en, src, token);
            Log("Đã chép bản gốc tiếng Anh: " + Path.GetFileName(en));
        }

        foreach (var (kind, map, label) in new[] { ("vi", vi, "Bản Tiếng Việt"), ("bi", bi, "Song Ngữ Anh - Việt") })
        {
            if ((kind == "vi" && !o.MakeVi) || (kind == "bi" && !o.MakeBi)) continue;
            var title = $"{book.Title} ({label})";
            var data = BuildEpub(entries, map, opfName, opf.Length > 0 ? UpdateOpf(opf, title) : null,
                ncxName, ncx.Length > 0 ? UpdateNcx(ncx, book.Title, title) : null);
            var fileName = $"{baseName} ({label}){suffix}.epub";

            // A new run replaces the earlier translation of the same book.
            var sourceUrl = $"translate:{book.Id}:{kind}";
            var old = _library.BySource(sourceUrl);
            if (old != null) _library.Remove(old.Id);
            var added = _library.Add(data, fileName, "translate", sourceUrl, File.Exists(_library.CoverFile(book.Id)) ? File.ReadAllBytes(_library.CoverFile(book.Id)) : null);

            string? copy = null;
            if (o.SaveCopy)
            {
                copy = Path.Combine(o.OutDir, fileName);
                await File.WriteAllBytesAsync(copy, data, token);
            }
            outputs.Add(new TranslateOutput(kind, added.Id, added.Title, copy));
            Log($"Đã thêm vào thư viện: {added.Title} ({data.Length / 1048576.0:0.00} MB)" + (copy != null ? $" — bản sao: {copy}" : ""), "success");
        }
        lock (_gate) _outputs = outputs;

        if (_failed == 0)
        {
            try { File.Delete(cacheFile); } catch { /* kept for next time */ }
            Finish(true, $"Hoàn tất! Đã dịch {_paras} đoạn trong {_chapters} chương.");
        }
        else
        {
            Finish(false, $"Xong nhưng còn {_failed} đoạn chưa dịch được (Google chặn hoặc lỗi mạng). Đợi vài phút rồi bấm Dịch lại: chỉ những đoạn lỗi được dịch bù.");
        }
    }

    private async Task<(string Vi, string Bi, int Ok, int Fail)> TranslateChapter(string name, string html,
        GoogleTranslateClient client, Dictionary<string, string> cache, object cacheGate, TranslateOptions o, CancellationToken token)
    {
        var matches = Block.Matches(html);
        if (matches.Count == 0) return (html, html, 0, 0);

        var blocks = new List<(int Index, string Text, string Key)>();
        for (var i = 0; i < matches.Count; i++)
        {
            var text = CleanText(matches[i].Groups[3].Value);
            if (text.Length > 0 && text.Any(char.IsLetter)) blocks.Add((i, text, Md5($"{name}:{i}:{text}")));
        }
        if (blocks.Count == 0) return (html, html, 0, 0);

        var results = new string?[blocks.Count];
        var pending = new List<int>();
        lock (cacheGate)
        {
            for (var k = 0; k < blocks.Count; k++)
                if (cache.TryGetValue(blocks[k].Key, out var hit)) results[k] = hit; else pending.Add(k);
        }
        int ok = blocks.Count - pending.Count, fail = 0;

        var batch = new List<int>();
        var chars = 0;
        async Task Flush()
        {
            if (batch.Count == 0) return;
            try
            {
                var res = await client.TranslateLinesAsync(batch.Select(k => blocks[k].Text).ToList());
                lock (cacheGate)
                {
                    for (var j = 0; j < batch.Count && j < res.Count; j++)
                    {
                        results[batch[j]] = res[j];
                        cache[blocks[batch[j]].Key] = res[j];
                    }
                }
                ok += batch.Count;
            }
            catch (OperationCanceledException) { throw; }
            catch (Exception e)
            {
                fail += batch.Count;
                Log($"[{Path.GetFileName(name)}] {batch.Count} đoạn lỗi: {e.Message}", "error");
            }
            batch = new();
            chars = 0;
        }
        foreach (var k in pending)
        {
            token.ThrowIfCancellationRequested();
            var line = blocks[k].Text;
            if (batch.Count > 0 && (chars + line.Length > o.BatchChars || batch.Count >= 35)) await Flush();
            batch.Add(k);
            chars += line.Length + 1;
        }
        await Flush();

        var byIndex = new Dictionary<int, string>();
        for (var k = 0; k < blocks.Count; k++) if (!string.IsNullOrEmpty(results[k])) byIndex[blocks[k].Index] = results[k]!;

        var vi = new StringBuilder(html.Length + html.Length / 2);
        var bi = new StringBuilder(html.Length * 2);
        var last = 0;
        for (var i = 0; i < matches.Count; i++)
        {
            var m = matches[i];
            vi.Append(html, last, m.Index - last);
            bi.Append(html, last, m.Index - last);
            if (byIndex.TryGetValue(i, out var tr))
            {
                var open = m.Groups[1].Value;
                var close = m.Groups[4].Value;
                var escaped = Escape(tr);
                // Empty anchors are link targets (footnotes, TOC); keep them in the Vietnamese copy.
                var anchors = string.Concat(EmptyAnchor.Matches(m.Groups[3].Value).Select(a => a.Value));
                vi.Append(open).Append(anchors).Append(escaped).Append(close);
                // The translation follows the English paragraph, in italics. Opacity instead of a
                // fixed grey keeps it readable on dark reading themes; the copy drops the id.
                bi.Append(m.Value).Append('\n')
                  .Append(IdAttr.Replace(open, ""))
                  .Append("<span style=\"font-style:italic;opacity:.78\">").Append(escaped).Append("</span>")
                  .Append(close);
            }
            else
            {
                vi.Append(m.Value);
                bi.Append(m.Value);
            }
            last = m.Index + m.Length;
        }
        vi.Append(html, last, html.Length - last);
        bi.Append(html, last, html.Length - last);
        return (vi.ToString(), bi.ToString(), ok, fail);
    }

    private static byte[] BuildEpub(List<(string Name, byte[] Data)> entries, Dictionary<string, byte[]> chapters,
        string? opfName, string? opf, string? ncxName, string? ncx)
    {
        using var ms = new MemoryStream();
        using (var zip = new ZipArchive(ms, ZipArchiveMode.Create, true))
        {
            // "mimetype" first and uncompressed, as the EPUB spec asks.
            var mime = entries.FirstOrDefault(e => e.Name == "mimetype");
            using (var s = zip.CreateEntry("mimetype", CompressionLevel.NoCompression).Open())
                s.Write(mime.Data ?? Encoding.ASCII.GetBytes("application/epub+zip"));
            foreach (var (name, data) in entries)
            {
                if (name == "mimetype") continue;
                var bytes = chapters.TryGetValue(name, out var ch) ? ch
                    : name == opfName && opf != null ? Encoding.UTF8.GetBytes(opf)
                    : name == ncxName && ncx != null ? Encoding.UTF8.GetBytes(ncx)
                    : data;
                using var s = zip.CreateEntry(name, CompressionLevel.Optimal).Open();
                s.Write(bytes);
            }
        }
        return ms.ToArray();
    }

    public static string UpdateOpf(string opf, string newTitle)
    {
        var lang = new Regex(@"(<dc:language\b[^>]*>)(.*?)(</dc:language>)", RegexOptions.IgnoreCase | RegexOptions.Singleline);
        var title = new Regex(@"(<dc:title\b[^>]*>)(.*?)(</dc:title>)", RegexOptions.IgnoreCase | RegexOptions.Singleline);
        opf = lang.Replace(opf, m => m.Groups[1].Value + "vi" + m.Groups[3].Value, 1);
        return title.Replace(opf, m => m.Groups[1].Value + Escape(newTitle) + m.Groups[3].Value, 1);
    }

    public static string UpdateNcx(string ncx, string originalTitle, string newTitle)
    {
        ncx = Regex.Replace(ncx, @"<text>CHAPTER\s+(\d+)</text>", "<text>Chương $1</text>", RegexOptions.IgnoreCase);
        if (originalTitle.Length > 0)
        {
            var to = $"<text>{Escape(newTitle)}</text>";
            ncx = ncx.Replace($"<text>{Escape(originalTitle)}</text>", to).Replace($"<text>{originalTitle}</text>", to);
        }
        return ncx.Replace("<text>Prologue</text>", "<text>Mở đầu</text>").Replace("<text>Epilogue</text>", "<text>Kết</text>");
    }

    // Like Python's html.escape: only the five markup characters, so Vietnamese letters stay as text.
    public static string Escape(string s) => s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;")
        .Replace("\"", "&quot;").Replace("'", "&#x27;");

    public static string SafeFileName(string name)
    {
        name = Regex.Replace(name, @"[<>:""/\\|?*\x00-\x1f]", " - ");
        name = Regex.Replace(name, @"\s+", " ").Trim(' ', '.');
        if (name.Length > 100) name = name[..100].Trim(' ', '.');
        return name.Length > 0 ? name : "Untitled";
    }
}
