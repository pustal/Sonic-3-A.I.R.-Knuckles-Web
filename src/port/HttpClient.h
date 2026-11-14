/**
 * @file HttpClient.h
 * @brief A plain HTTPS GET for the port's own needs (the update check), with
 *        the TV's libcurl loaded at run time (see CurlShim.cpp).
 */
#ifndef S3W_HTTP_CLIENT_H
#define S3W_HTTP_CLIENT_H

#include <string>

namespace s3w
{

/**
 * Fetches @p url (following redirects, 20 s at most, at most 1 MB).
 * @param body  Receives the response body on success.
 * @param error Receives the reason on failure (no libcurl, network, HTTP status).
 * @return True when the server answered 200.
 */
bool HttpGet(const std::string &url, std::string *body, std::string *error);

} // namespace s3w

#endif // S3W_HTTP_CLIENT_H
