/*
 * Multithreaded HTTP Proxy Server for Linux
 * Reads configuration from settings.txt
 * Build: g++ -O2 -pthread -o proxy_server proxy_server_linux.cpp
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cerrno>
#include <ctime>
#include <string>
#include <fstream>
#include <sstream>
#include <atomic>

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
struct Settings {
    std::string listen_address = "0.0.0.0";
    int         listen_port    = 8080;
    int         max_threads    = 100;
    int         buffer_size    = 65536;
    int         connect_timeout = 10;
    int         recv_timeout   = 30;
    bool        logging        = true;
    std::string log_file;
};

static Settings           g_settings;
static std::atomic<int>   g_active_threads(0);
static FILE*              g_log_fp = nullptr;
static pthread_mutex_t    g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static void log_msg(const char* fmt, ...) {
    if (!g_settings.logging) return;

    pthread_mutex_lock(&g_log_mutex);

    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char timestamp[64];
    snprintf(timestamp, sizeof(timestamp),
             "%04d-%02d-%02d %02d:%02d:%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

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
    }

    va_end(ap);
    pthread_mutex_unlock(&g_log_mutex);
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

        if      (key == "listen_port")     g_settings.listen_port     = atoi(val.c_str());
        else if (key == "listen_address")  g_settings.listen_address  = val;
        else if (key == "max_threads")     g_settings.max_threads     = atoi(val.c_str());
        else if (key == "buffer_size")     g_settings.buffer_size     = atoi(val.c_str());
        else if (key == "connect_timeout") g_settings.connect_timeout = atoi(val.c_str());
        else if (key == "recv_timeout")    g_settings.recv_timeout    = atoi(val.c_str());
        else if (key == "logging")         g_settings.logging         = (val == "1");
        else if (key == "log_file")        g_settings.log_file        = val;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Close a socket safely
// ---------------------------------------------------------------------------
static void close_socket(int& s) {
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
        close(s);
        s = -1;
    }
}

// ---------------------------------------------------------------------------
// Set socket timeouts
// ---------------------------------------------------------------------------
static void set_socket_timeouts(int s, int recv_sec, int send_sec) {
    struct timeval recv_tv = { recv_sec, 0 };
    struct timeval send_tv = { send_sec, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv));
}

// ---------------------------------------------------------------------------
// Connect to upstream host with timeout
// ---------------------------------------------------------------------------
static int connect_to_host(const char* host, int port) {
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        log_msg("DNS resolution failed for %s", host);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }

    // Non-blocking connect with timeout
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (ret < 0) {
        if (errno != EINPROGRESS) {
            close_socket(sock);
            return -1;
        }
        // Wait for connect with timeout
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval tv;
        tv.tv_sec  = g_settings.connect_timeout;
        tv.tv_usec = 0;

        ret = select(sock + 1, nullptr, &wset, nullptr, &tv);
        if (ret <= 0) {
            log_msg("Connection to %s:%d timed out", host, port);
            close_socket(sock);
            return -1;
        }

        // Check for connect error
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0) {
            log_msg("Connection to %s:%d failed (error %d)", host, port, so_error);
            close_socket(sock);
            return -1;
        }
    }

    // Back to blocking
    fcntl(sock, F_SETFL, flags);

    set_socket_timeouts(sock, g_settings.recv_timeout, g_settings.recv_timeout);
    return sock;
}

// ---------------------------------------------------------------------------
// Parse request line
// ---------------------------------------------------------------------------
struct RequestInfo {
    std::string method;
    std::string host;
    int         port = 80;
    std::string path;
    std::string full_header;
    bool        is_connect = false;
};

static bool parse_request(const std::string& header, RequestInfo& ri) {
    size_t line_end = header.find("\r\n");
    if (line_end == std::string::npos) return false;

    std::string request_line = header.substr(0, line_end);

    std::istringstream iss(request_line);
    std::string version;
    iss >> ri.method >> ri.path >> version;

    if (ri.method.empty() || ri.path.empty()) return false;

    // CONNECT method (HTTPS tunneling)
    if (ri.method == "CONNECT") {
        ri.is_connect = true;
        size_t colon = ri.path.rfind(':');
        if (colon != std::string::npos) {
            ri.host = ri.path.substr(0, colon);
            ri.port = atoi(ri.path.substr(colon + 1).c_str());
        } else {
            ri.host = ri.path;
            ri.port = 443;
        }
        ri.full_header = header;
        return true;
    }

    // Absolute URL: http://host[:port]/path
    if (ri.path.find("http://") == 0) {
        std::string url = ri.path.substr(7);
        size_t slash = url.find('/');
        std::string host_port;
        std::string rel_path = "/";

        if (slash != std::string::npos) {
            host_port = url.substr(0, slash);
            rel_path  = url.substr(slash);
        } else {
            host_port = url;
        }

        size_t colon = host_port.rfind(':');
        if (colon != std::string::npos) {
            ri.host = host_port.substr(0, colon);
            ri.port = atoi(host_port.substr(colon + 1).c_str());
        } else {
            ri.host = host_port;
            ri.port = 80;
        }

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
    size_t host_end   = header.find("\r\n", host_start);
    std::string host_val = header.substr(host_start, host_end - host_start);
    host_val = trim(host_val);

    size_t colon = host_val.rfind(':');
    if (colon != std::string::npos) {
        ri.host = host_val.substr(0, colon);
        ri.port = atoi(host_val.substr(colon + 1).c_str());
    } else {
        ri.host = host_val;
        ri.port = 80;
    }

    ri.full_header = header;
    return true;
}

// ---------------------------------------------------------------------------
// Relay data between two sockets (bidirectional tunnel for CONNECT)
// ---------------------------------------------------------------------------
static void relay_tunnel(int client, int remote) {
    char* buf = new char[g_settings.buffer_size];
    int maxfd = (client > remote ? client : remote) + 1;
    fd_set rset;

    while (true) {
        FD_ZERO(&rset);
        FD_SET(client, &rset);
        FD_SET(remote, &rset);

        struct timeval tv;
        tv.tv_sec  = g_settings.recv_timeout;
        tv.tv_usec = 0;

        int ret = select(maxfd, &rset, nullptr, nullptr, &tv);
        if (ret <= 0) break;

        if (FD_ISSET(client, &rset)) {
            ssize_t n = recv(client, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            ssize_t sent = send(remote, buf, n, 0);
            if (sent <= 0) break;
        }

        if (FD_ISSET(remote, &rset)) {
            ssize_t n = recv(remote, buf, g_settings.buffer_size, 0);
            if (n <= 0) break;
            ssize_t sent = send(client, buf, n, 0);
            if (sent <= 0) break;
        }
    }

    delete[] buf;
}

// ---------------------------------------------------------------------------
// Client handler thread
// ---------------------------------------------------------------------------
struct ClientContext {
    int         client_socket;
    sockaddr_in client_addr;
};

static void* client_thread(void* param) {
    g_active_threads++;

    ClientContext* ctx = (ClientContext*)param;
    int client = ctx->client_socket;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ctx->client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(ctx->client_addr.sin_port);
    delete ctx;

    set_socket_timeouts(client, g_settings.recv_timeout, g_settings.recv_timeout);

    char* buf = new char[g_settings.buffer_size];
    int remote = -1;

    // Read the HTTP request header
    std::string header;
    while (true) {
        ssize_t n = recv(client, buf, g_settings.buffer_size - 1, 0);
        if (n <= 0) {
            log_msg("[%s:%d] Client disconnected before sending request", client_ip, client_port);
            goto cleanup;
        }
        header.append(buf, n);

        if (header.find("\r\n\r\n") != std::string::npos) break;

        if (header.size() > 65536) {
            log_msg("[%s:%d] Request header too large", client_ip, client_port);
            const char* resp = "HTTP/1.1 431 Request Header Fields Too Large\r\n\r\n";
            send(client, resp, strlen(resp), 0);
            goto cleanup;
        }
    }

    {
        RequestInfo ri;
        if (!parse_request(header, ri)) {
            log_msg("[%s:%d] Malformed request", client_ip, client_port);
            const char* resp = "HTTP/1.1 400 Bad Request\r\n\r\n";
            send(client, resp, strlen(resp), 0);
            goto cleanup;
        }

        log_msg("[%s:%d] %s %s:%d%s",
                client_ip, client_port,
                ri.method.c_str(), ri.host.c_str(), ri.port,
                ri.is_connect ? "" : ri.path.c_str());

        if (ri.port <= 0 || ri.port > 65535) {
            const char* resp = "HTTP/1.1 400 Bad Request\r\n\r\n";
            send(client, resp, strlen(resp), 0);
            goto cleanup;
        }

        remote = connect_to_host(ri.host.c_str(), ri.port);
        if (remote < 0) {
            log_msg("[%s:%d] Cannot connect to %s:%d", client_ip, client_port, ri.host.c_str(), ri.port);
            const char* resp = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
            send(client, resp, strlen(resp), 0);
            goto cleanup;
        }

        if (ri.is_connect) {
            const char* resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
            send(client, resp, strlen(resp), 0);
            relay_tunnel(client, remote);
        } else {
            const std::string& fwd = ri.full_header;
            size_t total_sent = 0;
            while (total_sent < fwd.size()) {
                ssize_t s = send(remote, fwd.c_str() + total_sent, fwd.size() - total_sent, 0);
                if (s <= 0) goto cleanup;
                total_sent += s;
            }

            size_t hdr_end = header.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
                size_t body_start = hdr_end + 4;
                if (body_start < header.size()) {
                    const char* body = header.c_str() + body_start;
                    size_t body_len = header.size() - body_start;
                    ssize_t s = send(remote, body, body_len, 0);
                    if (s <= 0) goto cleanup;
                }
            }

            while (true) {
                ssize_t n = recv(remote, buf, g_settings.buffer_size, 0);
                if (n <= 0) break;
                ssize_t s = send(client, buf, n, 0);
                if (s <= 0) break;
            }
        }
    }

cleanup:
    delete[] buf;
    close_socket(remote);
    close_socket(client);
    g_active_threads--;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    const char* settings_path = "settings.txt";
    if (argc > 1) settings_path = argv[1];

    // Ignore SIGPIPE (broken pipe on send to closed socket)
    signal(SIGPIPE, SIG_IGN);

    // Load settings
    load_settings(settings_path);

    // Open log file
    if (!g_settings.log_file.empty()) {
        g_log_fp = fopen(g_settings.log_file.c_str(), "a");
        if (!g_log_fp) {
            fprintf(stderr, "Warning: Cannot open log file %s\n", g_settings.log_file.c_str());
        }
    }

    log_msg("=== Proxy Server Starting ===");
    log_msg("Listen: %s:%d  MaxThreads: %d  Buffer: %d",
            g_settings.listen_address.c_str(), g_settings.listen_port,
            g_settings.max_threads, g_settings.buffer_size);

    // Create listening socket
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        perror("socket() failed");
        return 1;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)g_settings.listen_port);
    inet_pton(AF_INET, g_settings.listen_address.c_str(), &addr.sin_addr);

    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind() failed");
        close(listen_sock);
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) < 0) {
        perror("listen() failed");
        close(listen_sock);
        return 1;
    }

    log_msg("Listening on %s:%d ...", g_settings.listen_address.c_str(), g_settings.listen_port);

    // Accept loop
    while (true) {
        sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            log_msg("accept() failed: %s", strerror(errno));
            continue;
        }

        // Check thread limit
        if (g_active_threads >= g_settings.max_threads) {
            log_msg("Thread limit reached (%d), rejecting connection", g_settings.max_threads);
            const char* resp = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
            send(client_sock, resp, strlen(resp), 0);
            close(client_sock);
            continue;
        }

        ClientContext* ctx = new ClientContext;
        ctx->client_socket = client_sock;
        ctx->client_addr   = client_addr;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&tid, &attr, client_thread, ctx) != 0) {
            log_msg("pthread_create failed: %s", strerror(errno));
            delete ctx;
            close(client_sock);
        }

        pthread_attr_destroy(&attr);
    }

    // Cleanup (unreachable in normal operation)
    close(listen_sock);
    if (g_log_fp) fclose(g_log_fp);
    pthread_mutex_destroy(&g_log_mutex);
    return 0;
}
