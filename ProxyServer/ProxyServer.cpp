/*
 * Multithreaded HTTP Proxy Server for Windows
 * Reads configuration from settings.txt
 * Build: cl /EHsc /O2 proxy_server.cpp /link ws2_32.lib
 *    or: g++ -O2 -o proxy_server.exe proxy_server.cpp -lws2_32
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <io.h>
#include <share.h>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

 // ---------------------------------------------------------------------------
 // Settings
 // ---------------------------------------------------------------------------
struct Settings {
    std::string listen_address = "0.0.0.0";
    int         listen_port = 8080;
    int         max_threads = 100;
    int         buffer_size = 65536;
    int         connect_timeout = 10;
    int         recv_timeout = 30;
    bool        logging = true;
    std::string log_file;
};

static Settings     g_settings;
static LONG         g_active_threads = 0;
static FILE* g_log_fp = nullptr;
static CRITICAL_SECTION g_log_cs;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void log_msg(const char* fmt, ...) {
    if (!g_settings.logging) return;

    EnterCriticalSection(&g_log_cs);

    SYSTEMTIME st;
    GetLocalTime(&st);

    char timestamp[64];
    _snprintf_s(timestamp, sizeof(timestamp), _TRUNCATE,
        "%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    va_list ap;
    va_start(ap, fmt);

    fprintf(stdout, "[%s] ", timestamp);
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    fflush(stdout);

    if (g_log_fp) {
        va_end(ap);
        va_start(ap, fmt);
        fprintf(g_log_fp, "[%s] ", timestamp);
        vfprintf(g_log_fp, fmt, ap);
        fprintf(g_log_fp, "\n");
        fflush(g_log_fp);
        _commit(_fileno(g_log_fp));
    }

    va_end(ap);
    LeaveCriticalSection(&g_log_cs);
}

// ---------------------------------------------------------------------------
// Trim helper
// ---------------------------------------------------------------------------
static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ---------------------------------------------------------------------------
// Load settings from file
// ---------------------------------------------------------------------------
static bool load_settings(const char* path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        fprintf(stderr, "Warning: Cannot open %s, using defaults.\n", path);
        return false;
    }

    std::string line;
    while (std::getline(ifs, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "listen_port")     g_settings.listen_port = atoi(val.c_str());
        else if (key == "listen_address")  g_settings.listen_address = val;
        else if (key == "max_threads")     g_settings.max_threads = atoi(val.c_str());
        else if (key == "buffer_size")     g_settings.buffer_size = atoi(val.c_str());
        else if (key == "connect_timeout") g_settings.connect_timeout = atoi(val.c_str());
        else if (key == "recv_timeout")    g_settings.recv_timeout = atoi(val.c_str());
        else if (key == "logging")         g_settings.logging = (val == "1");
        else if (key == "log_file")        g_settings.log_file = val;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Close a socket safely
// ---------------------------------------------------------------------------
static void close_socket(SOCKET& s) {
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
        s = INVALID_SOCKET;
    }
}

// ---------------------------------------------------------------------------
// Set socket timeouts
// ---------------------------------------------------------------------------
static void set_socket_timeouts(SOCKET s, int recv_sec, int send_sec) {
    DWORD recv_ms = recv_sec * 1000;
    DWORD send_ms = send_sec * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_ms, sizeof(recv_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_ms, sizeof(send_ms));
}

// ---------------------------------------------------------------------------
// Connect to upstream host with timeout
// ---------------------------------------------------------------------------
static SOCKET connect_to_host(const char* host, int port) {
    struct addrinfo hints = {}, * res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    _snprintf_s(port_str, sizeof(port_str), _TRUNCATE, "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        log_msg("DNS resolution failed for %s", host);
        return INVALID_SOCKET;
    }

    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }

    // Non-blocking connect with timeout
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    int ret = connect(sock, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);

    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            close_socket(sock);
            return INVALID_SOCKET;
        }
        // Wait for connect with timeout
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv;
        tv.tv_sec = g_settings.connect_timeout;
        tv.tv_usec = 0;

        ret = select(0, nullptr, &wset, nullptr, &tv);
        if (ret <= 0) {
            log_msg("Connection to %s:%d timed out", host, port);
            close_socket(sock);
            return INVALID_SOCKET;
        }

        // Check for connect error
        int so_error = 0;
        int len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
        if (so_error != 0) {
            log_msg("Connection to %s:%d failed (error %d)", host, port, so_error);
            close_socket(sock);
            return INVALID_SOCKET;
        }
    }

    // Back to blocking
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);

    set_socket_timeouts(sock, g_settings.recv_timeout, g_settings.recv_timeout);
    return sock;
}

// ---------------------------------------------------------------------------
// Parse request line: "METHOD host:port HTTP/ver" or "METHOD http://host/path HTTP/ver"
// ---------------------------------------------------------------------------
struct RequestInfo {
    std::string method;
    std::string host;
    int         port = 80;
    std::string path;
    std::string full_header;  // entire header block received
    bool        is_connect = false;
};

static bool parse_request(const std::string& header, RequestInfo& ri) {
    // Find the request line
    size_t line_end = header.find("\r\n");
    if (line_end == std::string::npos) return false;

    std::string request_line = header.substr(0, line_end);

    // Split into METHOD URL VERSION
    std::istringstream iss(request_line);
    std::string version;
    iss >> ri.method >> ri.path >> version;

    if (ri.method.empty() || ri.path.empty()) return false;

    // CONNECT method (for HTTPS tunneling)
    if (ri.method == "CONNECT") {
        ri.is_connect = true;
        // path = "host:port"
        size_t colon = ri.path.rfind(':');
        if (colon != std::string::npos) {
            ri.host = ri.path.substr(0, colon);
            ri.port = atoi(ri.path.substr(colon + 1).c_str());
        }
        else {
            ri.host = ri.path;
            ri.port = 443;
        }
        ri.full_header = header;
        return true;
    }

    // Absolute URL: http://host[:port]/path
    if (ri.path.find("http://") == 0) {
        std::string url = ri.path.substr(7); // skip "http://"
        size_t slash = url.find('/');
        std::string host_port;
        std::string rel_path = "/";

        if (slash != std::string::npos) {
            host_port = url.substr(0, slash);
            rel_path = url.substr(slash);
        }
        else {
            host_port = url;
        }

        size_t colon = host_port.rfind(':');
        if (colon != std::string::npos) {
            ri.host = host_port.substr(0, colon);
            ri.port = atoi(host_port.substr(colon + 1).c_str());
        }
        else {
            ri.host = host_port;
            ri.port = 80;
        }

        // Rewrite request line to use relative path
        std::string new_request_line = ri.method + " " + rel_path + " " + version;
        ri.full_header = new_request_line + header.substr(line_end);
        ri.path = rel_path;
        return true;
    }

    // Relative URL — extract Host header
    size_t host_pos = header.find("Host: ");
    if (host_pos == std::string::npos)
        host_pos = header.find("host: ");
    if (host_pos == std::string::npos) return false;

    size_t host_start = host_pos + 6;
    size_t host_end = header.find("\r\n", host_start);
    std::string host_val = header.substr(host_start, host_end - host_start);
    host_val = trim(host_val);

    size_t colon = host_val.rfind(':');
    if (colon != std::string::npos) {
        ri.host = host_val.substr(0, colon);
        ri.port = atoi(host_val.substr(colon + 1).c_str());
    }
    else {
        ri.host = host_val;
        ri.port = 80;
    }

    ri.full_header = header;
    return true;
}

// ---------------------------------------------------------------------------
// Relay data between two sockets (bidirectional tunnel for CONNECT)
// ---------------------------------------------------------------------------
static void relay_tunnel(SOCKET client, SOCKET remote) {
    char* buf = new char[g_settings.buffer_size];
    fd_set rset;

    while (true) {
        FD_ZERO(&rset);
        FD_SET(client, &rset);
        FD_SET(remote, &rset);

        struct timeval tv;
        tv.tv_sec = g_settings.recv_timeout;
        tv.tv_usec = 0;

        int ret = select(0, &rset, nullptr, nullptr, &tv);
        if (ret <= 0) break;

        if (FD_ISSET(client, &rset)) {
            int n = recv(client, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            int sent = send(remote, buf, n, 0);
            if (sent <= 0) break;
        }

        if (FD_ISSET(remote, &rset)) {
            int n = recv(remote, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            int sent = send(client, buf, n, 0);
            if (sent <= 0) break;
        }
    }

    delete[] buf;
}

// ---------------------------------------------------------------------------
// Client handler thread
// ---------------------------------------------------------------------------
struct ClientContext {
    SOCKET client_socket;
    sockaddr_in client_addr;
};

static DWORD WINAPI client_thread(LPVOID param) {
    InterlockedIncrement(&g_active_threads);

    ClientContext* ctx = (ClientContext*)param;
    SOCKET client = ctx->client_socket;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ctx->client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(ctx->client_addr.sin_port);
    delete ctx;

    set_socket_timeouts(client, g_settings.recv_timeout, g_settings.recv_timeout);

    char* buf = new char[g_settings.buffer_size];
    SOCKET remote = INVALID_SOCKET;

    // Read the HTTP request header
    std::string header;
    while (true) {
        int n = recv(client, buf, g_settings.buffer_size - 1, 0);
        if (n <= 0) {
            log_msg("[%s:%d] Client disconnected before sending request", client_ip, client_port);
            goto cleanup;
        }
        header.append(buf, n);

        // Check for end of headers
        if (header.find("\r\n\r\n") != std::string::npos) break;

        if (header.size() > 65536) {
            log_msg("[%s:%d] Request header too large", client_ip, client_port);
            const char* resp = "HTTP/1.1 431 Request Header Fields Too Large\r\n\r\n";
            send(client, resp, (int)strlen(resp), 0);
            goto cleanup;
        }
    }

    {
        RequestInfo ri;
        if (!parse_request(header, ri)) {
            log_msg("[%s:%d] Malformed request", client_ip, client_port);
            const char* resp = "HTTP/1.1 400 Bad Request\r\n\r\n";
            send(client, resp, (int)strlen(resp), 0);
            goto cleanup;
        }

        log_msg("[%s:%d] %s %s:%d%s",
            client_ip, client_port,
            ri.method.c_str(), ri.host.c_str(), ri.port,
            ri.is_connect ? "" : ri.path.c_str());

        // Validate port range
        if (ri.port <= 0 || ri.port > 65535) {
            const char* resp = "HTTP/1.1 400 Bad Request\r\n\r\n";
            send(client, resp, (int)strlen(resp), 0);
            goto cleanup;
        }

        // Connect to upstream
        remote = connect_to_host(ri.host.c_str(), ri.port);
        if (remote == INVALID_SOCKET) {
            log_msg("[%s:%d] Cannot connect to %s:%d", client_ip, client_port, ri.host.c_str(), ri.port);
            const char* resp = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
            send(client, resp, (int)strlen(resp), 0);
            goto cleanup;
        }

        if (ri.is_connect) {
            // HTTPS tunnel — send 200 Connection Established, then relay
            const char* resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
            send(client, resp, (int)strlen(resp), 0);
            relay_tunnel(client, remote);
        }
        else {
            // Forward the request header to upstream
            const std::string& fwd = ri.full_header;
            int total_sent = 0;
            while (total_sent < (int)fwd.size()) {
                int s = send(remote, fwd.c_str() + total_sent, (int)fwd.size() - total_sent, 0);
                if (s <= 0) goto cleanup;
                total_sent += s;
            }

            // If there's body data after headers, forward that too
            size_t hdr_end = header.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
                size_t body_start = hdr_end + 4;
                if (body_start < header.size()) {
                    const char* body = header.c_str() + body_start;
                    int body_len = (int)(header.size() - body_start);
                    int s = send(remote, body, body_len, 0);
                    if (s <= 0) goto cleanup;
                }
            }

            // Relay response from upstream back to client
            while (true) {
                int n = recv(remote, buf, g_settings.buffer_size, 0);
                if (n <= 0) break;
                int s = send(client, buf, n, 0);
                if (s <= 0) break;
            }
        }
    }

cleanup:
    delete[] buf;
    close_socket(remote);
    close_socket(client);
    InterlockedDecrement(&g_active_threads);
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    const char* settings_path = "settings.txt";
    if (argc > 1) settings_path = argv[1];

    InitializeCriticalSection(&g_log_cs);

    // Load settings
    load_settings(settings_path);

    // Open log file
    if (!g_settings.log_file.empty()) {
        g_log_fp = _fsopen(g_settings.log_file.c_str(), "a", _SH_DENYNO);
        if (!g_log_fp) {
            fprintf(stderr, "Warning: Cannot open log file %s\n", g_settings.log_file.c_str());
        }
    }

    log_msg("=== Proxy Server Starting ===");
    log_msg("Listen: %s:%d  MaxThreads: %d  Buffer: %d",
        g_settings.listen_address.c_str(), g_settings.listen_port,
        g_settings.max_threads, g_settings.buffer_size);

    // Initialize Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    // Create listening socket
    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    // Bind
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_settings.listen_port);
    inet_pton(AF_INET, g_settings.listen_address.c_str(), &addr.sin_addr);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        fprintf(stderr, "listen() failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    log_msg("Listening on %s:%d ...", g_settings.listen_address.c_str(), g_settings.listen_port);

    // Accept loop
    while (true) {
        sockaddr_in client_addr = {};
        int addr_len = sizeof(client_addr);
        SOCKET client_sock = accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET) {
            log_msg("accept() failed: %d", WSAGetLastError());
            continue;
        }

        // Check thread limit
        if (g_active_threads >= g_settings.max_threads) {
            log_msg("Thread limit reached (%d), rejecting connection", g_settings.max_threads);
            const char* resp = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
            send(client_sock, resp, (int)strlen(resp), 0);
            closesocket(client_sock);
            continue;
        }

        ClientContext* ctx = new ClientContext;
        ctx->client_socket = client_sock;
        ctx->client_addr = client_addr;

        HANDLE hThread = CreateThread(nullptr, 0, client_thread, ctx, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);  // Detach — thread manages its own lifetime
        }
        else {
            log_msg("CreateThread failed: %d", GetLastError());
            delete ctx;
            closesocket(client_sock);
        }
    }

    // Cleanup (unreachable in normal operation; here for completeness)
    closesocket(listen_sock);
    WSACleanup();
    if (g_log_fp) fclose(g_log_fp);
    DeleteCriticalSection(&g_log_cs);
    return 0;
}
