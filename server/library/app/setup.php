<?php
// Sets the password of the admin account (creating it). Prints a new random
// password unless one is given:
//   php setup.php <user> [password]

declare(strict_types=1);

require_once __DIR__ . '/lib.php';

if (PHP_SAPI !== 'cli') exit;
$user = $argv[1] ?? 'admin';
$password = $argv[2] ?? rtrim(strtr(base64_encode(random_bytes(12)), '+/', 'xy'), '=');
foreach ([LIB_DATA, BOOKS_DIR, PUBLIC_COVERS_DIR, CUSTOM_DECKS_DIR, USERS_DIR] as $dir) {
    if (!is_dir($dir)) mkdir($dir, 0750, true);
}
setLogin($user, $password);
db();
echo "user: $user\npassword: $password\n";
