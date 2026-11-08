<?php
// Brings the database in line with library-data/books: adds new files with
// their metadata and cover, updates changed ones, drops deleted ones, and
// rebuilds the dictionary when a published deck changed. Run from cron:
//   */10 * * * * php /usr/local/lsws/crosspoint.zenix.vn/library-app/indexer.php

declare(strict_types=1);

require_once __DIR__ . '/vocab.php';

if (PHP_SAPI !== 'cli') exit;

$lock = fopen(LIB_DATA . '/indexer.lock', 'c');
if (!$lock || !flock($lock, LOCK_EX | LOCK_NB)) exit;  // a previous run is still going

foreach ([PUBLIC_COVERS_DIR, CUSTOM_DECKS_DIR] as $dir) {
    if (!is_dir($dir)) mkdir($dir, 0750, true);
}

$pdo = db();
$pass = time();
$known = [];
foreach ($pdo->query('SELECT id, path, size, mtime FROM books') as $row) $known[$row['path']] = $row;

$insert = $pdo->prepare(<<<SQL
    INSERT INTO books(path, title, author, language, description, category, subcategory, format, size, mtime,
                      added, cover, search, seen)
    VALUES (:path, :title, :author, :language, :description, :category, :subcategory, :format, :size, :mtime,
            :added, 0, :search, :seen)
    ON CONFLICT(path) DO UPDATE SET title = excluded.title, author = excluded.author, language = excluded.language,
        description = excluded.description, format = excluded.format, size = excluded.size, mtime = excluded.mtime,
        search = excluded.search, seen = excluded.seen
SQL);
$setCover = $pdo->prepare('UPDATE books SET cover = 1 WHERE path = ?');
$idOf = $pdo->prepare('SELECT id FROM books WHERE path = ?');

$added = 0;
$present = [];  // paths on disk; tracked here, not written per book, so a pass with no changes writes nothing
$files = new RecursiveIteratorIterator(new RecursiveDirectoryIterator(BOOKS_DIR, FilesystemIterator::SKIP_DOTS));
foreach ($files as $file) {
    if (!$file->isFile()) continue;
    $ext = strtolower($file->getExtension());
    if (!in_array($ext, BOOK_EXT, true)) continue;
    $path = substr($file->getPathname(), strlen(BOOKS_DIR) + 1);
    $size = $file->getSize();
    $mtime = $file->getMTime();
    $present[$path] = true;
    if (isset($known[$path])) {
        if ((int)$known[$path]['size'] === $size && (int)$known[$path]['mtime'] === $mtime) continue;
    }
    // Written in the last minute: the uploader may still be writing it (tar
    // sets the original modification time only once a file is complete).
    if ($mtime > time() - 60) continue;

    $folders = explode('/', $path);
    array_pop($folders);
    $category = cleanFolder($folders[0] ?? '');
    $subcategory = cleanFolder($folders[1] ?? '');
    $meta = nameMeta($path);
    $epub = $ext === 'epub' ? epubMeta($file->getPathname()) : null;
    $title = ($epub['title'] ?? '') ?: $meta['title'];
    $title = preg_replace('/\s+—\s+Mục lục$/u', '', $title) ?? $title;  // Wikisource exports of a book's contents page
    $author = ($epub['author'] ?? '') ?: $meta['author'];
    $language = $epub['language'] ?? '';
    if ($language === '' && str_contains(fold($path), 'tieng anh')) $language = 'en';
    $insert->execute([
        ':path' => $path, ':title' => $title, ':author' => $author, ':language' => $language,
        ':description' => mb_substr($epub['description'] ?? '', 0, 4000), ':category' => $category,
        ':subcategory' => $subcategory, ':format' => $ext, ':size' => $size, ':mtime' => $mtime, ':added' => time(),
        ':search' => fold("$title $author $category $subcategory"), ':seen' => $pass,
    ]);
    if (!empty($epub['cover'])) {
        $idOf->execute([$path]);
        $id = (int)$idOf->fetchColumn();
        $idOf->closeCursor();  // an open statement would pin the WAL
        if ($id && writeThumb($epub['cover'], coverFile($id))) $setCover->execute([$path]);
    }
    $added++;
}

// Files gone from disk.
// An unreadable books folder must not empty the library.
foreach ($present ? array_diff_key($known, $present) : [] as $row) {
    @unlink(coverFile((int)$row['id']));
    $pdo->prepare('DELETE FROM books WHERE id = ?')->execute([$row['id']]);
}

$dict = rebuildDictionary();
meta('indexed', (string)time());
echo date('c') . " indexed: $added new/changed" . ($dict ? ', dictionary rebuilt' : '') . "\n";
