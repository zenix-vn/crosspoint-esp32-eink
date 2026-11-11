using System.Security.Cryptography;
using System.Text.Json;

namespace CrossPointReader;

internal sealed class Bookmark
{
    public string Cfi { get; set; } = "";
    public string Label { get; set; } = "";
    public double Percent { get; set; }
    public long Created { get; set; }
}

internal sealed class BookRecord
{
    public string Id { get; set; } = "";
    public string Title { get; set; } = "";
    public string Author { get; set; } = "";
    public string Language { get; set; } = "";
    public string Description { get; set; } = "";
    public string Hash { get; set; } = "";
    public long FileSize { get; set; }
    public long Added { get; set; }
    public long? LastOpened { get; set; }
    public double Progress { get; set; }
    public string? Cfi { get; set; }
    public string? Chapter { get; set; }
    public string Source { get; set; } = "local";
    public string? SourceUrl { get; set; }
    public bool HasCover { get; set; }
    public List<Bookmark> Bookmarks { get; set; } = new();
}

// The books on this PC: Library/<id>/book.epub, cover.jpg, locations.json,
// with library.json as the index.
internal sealed class Library
{
    private readonly object _gate = new();
    private readonly List<BookRecord> _books;
    private string IndexFile => Path.Combine(AppPaths.Library, "library.json");

    public Library()
    {
        Directory.CreateDirectory(AppPaths.Library);
        _books = new();
        try
        {
            if (File.Exists(IndexFile))
                _books = JsonSerializer.Deserialize<List<BookRecord>>(File.ReadAllText(IndexFile), JsonOpts.Default) ?? new();
        }
        catch
        {
            _books = new();
        }
        _books.RemoveAll(b => !File.Exists(BookFile(b.Id)));
    }

    public static long Now => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();

    public string BookDir(string id) => Path.Combine(AppPaths.Library, id);
    public string BookFile(string id) => Path.Combine(BookDir(id), "book.epub");
    public string CoverFile(string id) => Path.Combine(BookDir(id), "cover.jpg");
    public string LocationsFile(string id) => Path.Combine(BookDir(id), "locations.json");

    public List<BookRecord> All()
    {
        lock (_gate) return _books.ToList();
    }

    public BookRecord? Get(string id)
    {
        lock (_gate) return _books.FirstOrDefault(b => b.Id == id);
    }

    public BookRecord? BySource(string url)
    {
        lock (_gate) return _books.FirstOrDefault(b => b.SourceUrl == url);
    }

    // Adds an EPUB or TXT file; the same file twice returns the first copy.
    public BookRecord Add(byte[] data, string fileName, string source, string? sourceUrl = null,
                          byte[]? fallbackCover = null, string? fallbackTitle = null, string? fallbackAuthor = null)
    {
        var ext = Path.GetExtension(fileName).ToLowerInvariant();
        var (nameTitle, nameAuthor) = TitleFromFileName(fileName);
        if (ext == ".txt")
        {
            data = EpubTools.TextToEpub(data, fallbackTitle ?? nameTitle, fallbackAuthor ?? nameAuthor);
        }
        else if (ext != ".epub" && !LooksLikeZip(data))
        {
            throw new InvalidDataException($"\"{fileName}\" không phải EPUB hoặc TXT");
        }

        var hash = Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
        lock (_gate)
        {
            var existing = _books.FirstOrDefault(b => b.Hash == hash);
            if (existing != null)
            {
                if (sourceUrl != null && existing.SourceUrl == null) { existing.SourceUrl = sourceUrl; SaveLocked(); }
                return existing;
            }
        }

        var meta = EpubTools.ReadMeta(data);
        var id = hash[..12];
        Directory.CreateDirectory(BookDir(id));
        File.WriteAllBytes(BookFile(id), data);
        var cover = EpubTools.Thumbnail(meta.Cover) ?? EpubTools.Thumbnail(fallbackCover);
        if (cover != null) File.WriteAllBytes(CoverFile(id), cover);

        var record = new BookRecord
        {
            Id = id,
            Title = FirstNonEmpty(meta.Title, fallbackTitle, nameTitle),
            Author = FirstNonEmpty(meta.Author, fallbackAuthor, nameAuthor),
            Language = meta.Language,
            Description = meta.Description.Length > 2000 ? meta.Description[..2000] : meta.Description,
            Hash = hash,
            FileSize = data.Length,
            Added = Now,
            Source = source,
            SourceUrl = sourceUrl,
            HasCover = cover != null,
        };
        lock (_gate)
        {
            _books.Add(record);
            SaveLocked();
        }
        return record;
    }

    public BookRecord? Update(string id, Action<BookRecord> change)
    {
        lock (_gate)
        {
            var b = _books.FirstOrDefault(x => x.Id == id);
            if (b == null) return null;
            change(b);
            SaveLocked();
            return b;
        }
    }

    public bool Remove(string id)
    {
        lock (_gate)
        {
            var removed = _books.RemoveAll(b => b.Id == id) > 0;
            if (removed) SaveLocked();
            try { Directory.Delete(BookDir(id), true); } catch { /* in use: left for next start */ }
            return removed;
        }
    }

    private void SaveLocked()
    {
        var tmp = IndexFile + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(_books, JsonOpts.Indented));
        File.Move(tmp, IndexFile, true);
    }

    private static bool LooksLikeZip(byte[] d) => d.Length > 4 && d[0] == 'P' && d[1] == 'K';

    private static string FirstNonEmpty(params string?[] values) =>
        values.FirstOrDefault(v => !string.IsNullOrWhiteSpace(v))?.Trim() ?? "Không có tiêu đề";

    // "Title - Author.epub", like the server's nameMeta().
    private static (string Title, string Author) TitleFromFileName(string fileName)
    {
        var name = Path.GetFileNameWithoutExtension(fileName).Trim();
        var parts = System.Text.RegularExpressions.Regex.Split(name, @"\s+[-–]\s+");
        return parts.Length >= 2 ? (string.Join(" - ", parts[..^1]).Trim(), parts[^1].Trim()) : (name, "");
    }
}
