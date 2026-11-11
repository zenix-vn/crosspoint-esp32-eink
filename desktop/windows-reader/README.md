# Zenix CrossPoint Reader for Windows

An EPUB reader for Windows that uses the CrossPoint library
(crosspoint.zenix.vn). It ships as one self-contained `CrossPointReader.exe`
and needs no .NET install. It uses the Edge WebView2 runtime, which comes with
Windows 10 and 11.

## Features

- **Library**: add EPUB or TXT files with the button, by drag and drop, or with
  *Open with*. Covers, progress, "continue reading", search, shelves and sort.
  TXT files are turned into EPUB.
- **Reader** (epub.js): one page, two pages or vertical scroll. Four page
  colours, fonts, font size, line spacing, margins and justification. Contents,
  bookmarks and full-text search. Your place is saved, and full screen is F11.
- **Discover (OPDS)**: sign in with your library account (HTTP Basic on
  `/library/opds.php`). Browse collections and categories, search, see book
  details, and download straight into the library.
- **Receive books over Wi-Fi**: the PC acts as a reader on `/inbox/api.php`.
  It has its own 12-hex id and 32-hex secret. Type the link key from the
  library's **Devices** page, and books you *send to device* on the web download
  by themselves (every 5 minutes while the app is open). Word lists and other
  files go to `Downloads\CrossPoint`.

Data lives in `%LOCALAPPDATA%\CrossPointReader`. The password and device secret
are encrypted with DPAPI.

## Build

```
dotnet publish -c Release -o publish      # -> publish/CrossPointReader.exe (~46 MB)
```

During development, `dotnet run` works. Set `CROSSPOINT_DEVTOOLS=1` to get the
WebView DevTools (F12).

## Layout

| Path | What |
| --- | --- |
| `src/MainForm.cs` | window and WebView2 host. Serves `https://app.crosspoint.internal/` from embedded files and `/api/*` |
| `src/ApiRouter.cs` | the API the page calls |
| `src/ServerClients.cs` | OPDS client and inbox client |
| `src/Library.cs`, `src/EpubTools.cs` | local library, EPUB metadata and covers, TXT to EPUB |
| `wwwroot/` | UI (`app.js`, `reader.js`, CSS), with epub.js 0.3.93 and JSZip 3.10.1 in `vendor/` |
| `tools/make-icon.ps1` | redraws `app.ico` |
