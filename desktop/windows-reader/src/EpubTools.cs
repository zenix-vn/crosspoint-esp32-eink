using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO.Compression;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml.Linq;

namespace CrossPointReader;

internal sealed class EpubMeta
{
    public string Title { get; set; } = "";
    public string Author { get; set; } = "";
    public string Language { get; set; } = "";
    public string Description { get; set; } = "";
    public byte[]? Cover { get; set; }
}

internal static partial class EpubTools
{
    private static readonly XNamespace Opf = "http://www.idpf.org/2007/opf";
    private static readonly XNamespace Dc = "http://purl.org/dc/elements/1.1/";
    private static readonly XNamespace Container = "urn:oasis:names:tc:opendocument:xmlns:container";

    public static EpubMeta ReadMeta(byte[] epub)
    {
        var meta = new EpubMeta();
        using var zip = new ZipArchive(new MemoryStream(epub), ZipArchiveMode.Read);
        var containerEntry = zip.GetEntry("META-INF/container.xml") ?? throw new InvalidDataException("Không phải tệp EPUB hợp lệ");
        var container = LoadXml(containerEntry);
        var opfPath = container.Descendants(Container + "rootfile").FirstOrDefault()?.Attribute("full-path")?.Value
                      ?? throw new InvalidDataException("EPUB thiếu tệp OPF");
        var opfEntry = zip.GetEntry(opfPath) ?? throw new InvalidDataException("EPUB thiếu tệp OPF");
        var opf = LoadXml(opfEntry);
        var baseDir = opfPath.Contains('/') ? opfPath[..(opfPath.LastIndexOf('/') + 1)] : "";

        var metadata = opf.Root?.Element(Opf + "metadata");
        if (metadata != null)
        {
            meta.Title = Clean(metadata.Elements(Dc + "title").FirstOrDefault()?.Value);
            meta.Author = string.Join(", ", metadata.Elements(Dc + "creator").Select(e => Clean(e.Value)).Where(s => s.Length > 0).Distinct());
            meta.Language = Clean(metadata.Elements(Dc + "language").FirstOrDefault()?.Value);
            meta.Description = StripTags(metadata.Elements(Dc + "description").FirstOrDefault()?.Value);
        }

        var manifest = opf.Root?.Element(Opf + "manifest")?.Elements(Opf + "item").ToList() ?? new();
        XElement? cover = manifest.FirstOrDefault(i => (i.Attribute("properties")?.Value ?? "").Split(' ').Contains("cover-image"));
        if (cover == null && metadata != null)
        {
            var coverId = metadata.Elements(Opf + "meta").FirstOrDefault(m => m.Attribute("name")?.Value == "cover")?.Attribute("content")?.Value;
            if (coverId != null) cover = manifest.FirstOrDefault(i => i.Attribute("id")?.Value == coverId);
        }
        bool IsImage(XElement i) => (i.Attribute("media-type")?.Value ?? "").StartsWith("image/");
        cover ??= manifest.FirstOrDefault(i => IsImage(i) && ((i.Attribute("id")?.Value ?? "") + (i.Attribute("href")?.Value ?? "")).Contains("cover", StringComparison.OrdinalIgnoreCase));
        if (cover != null && IsImage(cover))
        {
            var href = cover.Attribute("href")?.Value;
            if (href != null)
            {
                var entry = zip.GetEntry(NormalizePath(baseDir + Uri.UnescapeDataString(href)));
                if (entry != null && entry.Length < 20_000_000)
                {
                    using var s = entry.Open();
                    using var ms = new MemoryStream();
                    s.CopyTo(ms);
                    meta.Cover = ms.ToArray();
                }
            }
        }
        return meta;
    }

    // Covers are shown at most ~300 px wide; a smaller JPEG keeps the grid light.
    public static byte[]? Thumbnail(byte[]? image, int maxHeight = 720)
    {
        if (image == null || image.Length == 0) return null;
        try
        {
            using var src = Image.FromStream(new MemoryStream(image));
            var scale = Math.Min(1.0, (double)maxHeight / src.Height);
            var w = Math.Max(1, (int)(src.Width * scale));
            var h = Math.Max(1, (int)(src.Height * scale));
            using var bmp = new Bitmap(w, h);
            using (var g = Graphics.FromImage(bmp))
            {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.Clear(Color.White);
                g.DrawImage(src, 0, 0, w, h);
            }
            var codec = ImageCodecInfo.GetImageEncoders().First(c => c.FormatID == ImageFormat.Jpeg.Guid);
            using var p = new EncoderParameters(1);
            p.Param[0] = new EncoderParameter(System.Drawing.Imaging.Encoder.Quality, 86L);
            using var outMs = new MemoryStream();
            bmp.Save(outMs, codec, p);
            return outMs.ToArray();
        }
        catch
        {
            return null;
        }
    }

    // A plain text book becomes a small EPUB so the one reader opens everything.
    public static byte[] TextToEpub(byte[] raw, string title, string author)
    {
        var text = DecodeText(raw).Replace("\r\n", "\n").Replace('\r', '\n');
        var chapters = new List<(string Title, List<string> Paras)>();
        var current = (Title: title, Paras: new List<string>());
        foreach (var rawLine in text.Split('\n'))
        {
            var line = rawLine.Trim();
            if (line.Length == 0) continue;
            if (line.Length < 80 && ChapterHeading().IsMatch(line))
            {
                if (current.Paras.Count > 0) chapters.Add(current);
                current = (line, new List<string>());
                continue;
            }
            current.Paras.Add(line);
            if (current.Paras.Count >= 400)
            {
                chapters.Add(current);
                current = ($"{title} ({chapters.Count + 1})", new List<string>());
            }
        }
        if (current.Paras.Count > 0 || chapters.Count == 0) chapters.Add(current);

        var uid = "urn:uuid:" + Guid.NewGuid();
        using var ms = new MemoryStream();
        using (var zip = new ZipArchive(ms, ZipArchiveMode.Create, true))
        {
            var mime = zip.CreateEntry("mimetype", CompressionLevel.NoCompression);
            using (var w = new StreamWriter(mime.Open(), new UTF8Encoding(false))) w.Write("application/epub+zip");
            Put(zip, "META-INF/container.xml",
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>");

            var manifest = new StringBuilder();
            var spine = new StringBuilder();
            var nav = new StringBuilder();
            for (var i = 0; i < chapters.Count; i++)
            {
                var (ct, paras) = chapters[i];
                var file = $"c{i + 1:D4}.xhtml";
                var body = new StringBuilder();
                body.Append("<h2>").Append(E(ct)).Append("</h2>");
                foreach (var p in paras) body.Append("<p>").Append(E(p)).Append("</p>");
                Put(zip, "OEBPS/" + file, Xhtml(ct, body.ToString()));
                manifest.Append($"<item id=\"c{i + 1}\" href=\"{file}\" media-type=\"application/xhtml+xml\"/>");
                spine.Append($"<itemref idref=\"c{i + 1}\"/>");
                nav.Append($"<li><a href=\"{file}\">{E(ct)}</a></li>");
            }
            Put(zip, "OEBPS/nav.xhtml", Xhtml("Mục lục", $"<nav epub:type=\"toc\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><h1>Mục lục</h1><ol>{nav}</ol></nav>"));
            Put(zip, "OEBPS/content.opf",
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" +
                "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"uid\">" +
                "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">" +
                $"<dc:identifier id=\"uid\">{uid}</dc:identifier><dc:title>{E(title)}</dc:title>" +
                (author.Length > 0 ? $"<dc:creator>{E(author)}</dc:creator>" : "") +
                $"<dc:language>vi</dc:language><meta property=\"dcterms:modified\">{DateTime.UtcNow:yyyy-MM-ddTHH:mm:ssZ}</meta></metadata>" +
                $"<manifest><item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>{manifest}</manifest>" +
                $"<spine>{spine}</spine></package>");
        }
        return ms.ToArray();
    }

    private static string Xhtml(string title, string body) =>
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE html><html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">" +
        $"<head><meta charset=\"utf-8\"/><title>{E(title)}</title></head><body>{body}</body></html>";

    private static void Put(ZipArchive zip, string name, string content)
    {
        var entry = zip.CreateEntry(name, CompressionLevel.Optimal);
        using var w = new StreamWriter(entry.Open(), new UTF8Encoding(false));
        w.Write(content);
    }

    private static string DecodeText(byte[] raw)
    {
        if (raw.Length >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) return Encoding.UTF8.GetString(raw, 3, raw.Length - 3);
        if (raw.Length >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) return Encoding.Unicode.GetString(raw, 2, raw.Length - 2);
        try
        {
            return new UTF8Encoding(false, true).GetString(raw);
        }
        catch (DecoderFallbackException)
        {
            return Encoding.GetEncoding(1258).GetString(raw);
        }
    }

    private static XDocument LoadXml(ZipArchiveEntry entry)
    {
        using var s = entry.Open();
        return XDocument.Load(s);
    }

    private static string NormalizePath(string path)
    {
        var parts = new List<string>();
        foreach (var part in path.Split('/'))
        {
            if (part == "..") { if (parts.Count > 0) parts.RemoveAt(parts.Count - 1); }
            else if (part != "." && part.Length > 0) parts.Add(part);
        }
        return string.Join('/', parts);
    }

    private static string Clean(string? s) => Regex.Replace(s ?? "", @"\s+", " ").Trim();

    private static string StripTags(string? s) => Clean(WebUtility.HtmlDecode(Regex.Replace(s ?? "", "<[^>]+>", " ")));

    private static string E(string s) => WebUtility.HtmlEncode(s);

    [GeneratedRegex(@"^(chương|chuong|chapter|hồi|hoi|phần|phan|quyển|quyen|part|book)\s+([0-9]+|[ivxlcdm]+)\b", RegexOptions.IgnoreCase)]
    private static partial Regex ChapterHeading();
}
