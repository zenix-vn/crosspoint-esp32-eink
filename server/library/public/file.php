<?php
// Downloads a book (login or Basic credentials, for the reader's OPDS client).

declare(strict_types=1);

require_once __DIR__ . '/../../library-app/lib.php';

$user = requireLibraryAccess();
$book = bookById((int)($_GET['id'] ?? 0), $user);
if (!$book) {
    http_response_code(404);
    exit('Not found');
}
$types = ['epub' => 'application/epub+zip', 'pdf' => 'application/pdf', 'txt' => 'text/plain; charset=utf-8',
          'mobi' => 'application/x-mobipocket-ebook', 'azw3' => 'application/vnd.amazon.ebook'];
$name = $book['title'] . ($book['author'] ? ' - ' . $book['author'] : '') . '.' . $book['format'];
sendFile(bookFile($book), inboxName($name), $types[$book['format']] ?? 'application/octet-stream');
