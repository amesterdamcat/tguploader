#pragma once
#include <string>

// Verify an HS256-signed JWT against the C++ web's independent JWT secret.
// (Secret is owned by user_auth.cpp — call user_auth_init() at startup.)
// Returns true iff signature is valid AND the token is not expired.
bool jwt_verify_hs256(const std::string& token);

// Like jwt_verify_hs256 but also returns the "sub" claim. Empty on failure.
std::string jwt_extract_sub(const std::string& token);
