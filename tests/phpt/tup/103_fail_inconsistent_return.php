@kphp_should_fail
<?php
require_once 'polyfill/tuple-php-polyfill.php';

function getMe() {
    if(1)
        return tuple(1, 'str');
    return tuple(1, 'str', 'more');
}

function demo() {
    $t = getMe();
}

demo();