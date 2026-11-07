#pragma once
// Public half of the BLLED Plus release signing key (ECDSA P-256). Updates from GitHub are only
// installed if their .sig file was made with the matching private key, which only the release
// workflow has (GitHub secret BLLED_SIGNING_KEY).
static const char releaseSigningKey[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEDmKc9R7J6BFRAXLaEPKcvySQSV6c\n"
    "ZCwWqRi4IZ3jyVf/dOMDgBNCdm35Z8hAC883Jvbuf6HX9Je6Ak7I9aCqzg==\n"
    "-----END PUBLIC KEY-----\n";
