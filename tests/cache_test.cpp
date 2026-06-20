#include "cache.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::string request_path(const char* path) {
    const int client = socket(AF_INET, SOCK_STREAM, 0);
    assert(client >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(kIconPort);
    assert(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    const std::string request = std::string("GET ") + path + " HTTP/1.0\r\n\r\n";
    assert(send(client, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()));
    char response[256]{};
    const ssize_t count = recv(client, response, sizeof(response) - 1, 0);
    close(client);
    assert(count > 0);
    return std::string(response, static_cast<std::size_t>(count));
}

} // namespace

int main() {
    const UrlParts http = cache_parse_url("http://127.0.0.1:8080/icon.png");
    assert(http.valid && !http.secure && http.port == 8080 && http.path == "/icon.png");
    const UrlParts https = cache_parse_url("https://example.com/icon.png");
    assert(https.valid && https.secure && https.port == 443);
    assert(!cache_parse_url("example.com/icon.png").valid);
    assert(!cache_parse_url("http://host:abc/icon.png").valid);
    assert(!cache_parse_url("http://host:70000/icon.png").valid);
    assert(cache_file_path("../../bad:id", "http://host/icon.png").find("../") == std::string::npos);

    assert(cache_icon_server_start());
    assert(request_path("/icon_missing_12345678.png").rfind("HTTP/1.0 404", 0) == 0);
    assert(request_path("/../secret.png").rfind("HTTP/1.0 403", 0) == 0);
    cache_icon_server_stop();
    return 0;
}
