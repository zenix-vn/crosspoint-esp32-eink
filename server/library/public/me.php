<?php
// Who is signed in, for the landing page (/), which cannot see the session
// cookie (its path is /library/): {"signedIn": false} or the user's name,
// initial, role and reader count. Read-only; never cached.

declare(strict_types=1);

require_once __DIR__ . '/../../library-app/lib.php';

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');
startSession(true);
$u = currentUser();
if (!$u) {
    echo json_encode(['signedIn' => false]);
    exit;
}
$label = userLabel($u);
echo json_encode([
    'signedIn' => true,
    'name' => $label,
    'initial' => mb_strtoupper(mb_substr($label, 0, 1)),
    'hue' => crc32((string)($u['email'] ?? $u['login'] ?? $u['id'])) % 360,
    'admin' => isAdmin($u),
    'devices' => count(devicesOf((int)$u['id'])),
], JSON_UNESCAPED_UNICODE);
