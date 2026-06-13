#pragma once
#include <string>

// Initialise (read users.json into a static map). Returns false if file missing.
bool user_auth_init();

// Returns true iff `password` matches the bcrypt hash stored for `username`.
// Uses POSIX crypt_r() against the $2b$/$2a$ hash from users.json.
bool user_auth_verify(const std::string& username, const std::string& password);

// Sign a JWT for the given user (HS256 with web_jwt_secret).
// Returns empty string on failure. Token is valid for `ttl_hours` from now.
std::string user_auth_sign_token(const std::string& username, int ttl_hours = 24);

// Set a new bcrypt-hashed password for `username` and persist to users.json.
// Returns the empty string on success, or an error message on failure.
std::string user_auth_set_password(const std::string& username,
                                   const std::string& new_password);

// Web JWT secret — separate from Python web's JWT_SECRET so the two auth
// chains are truly independent. Loaded from CTG_WEB_JWT_SECRET env var,
// or read/created at $CTG_BASE_DIR/.account_configs/.web_jwt_secret.
const std::string& web_jwt_secret();
