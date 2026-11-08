<?php
// Downloads a word list as .tsv.

declare(strict_types=1);

require_once __DIR__ . '/../../library-app/vocab.php';

$user = requireBasicAuth();
$deck = deckByName((string)($_GET['d'] ?? ''), $user);
if (!$deck) {
    http_response_code(404);
    exit('Not found');
}
sendFile($deck['file'], $deck['name'] . '.tsv', 'text/tab-separated-values; charset=utf-8');
