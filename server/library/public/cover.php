<?php
// Old cover links (cover.php?id=N): covers are static files now, see coverName().

declare(strict_types=1);

require_once __DIR__ . '/../../library-app/lib.php';

requireLibraryAccess();
$id = (int)($_GET['id'] ?? 0);
if ($id <= 0 || !is_file(coverFile($id))) {
    http_response_code(404);
    exit;
}
header('Location: ' . coverUrl($id), true, 301);
