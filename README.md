# Multithreaded HTTP Proxy Server

A lightweight, multithreaded HTTP/HTTPS proxy server written in C++ with both Windows and Linux implementations. Configuration is read from a `settings.txt` file.

## Files

| File | Description |
|------|-------------|
| `ProxyServer.cpp` | Windows implementation (Winsock2, Win32 threads) |
| `proxy_server_linux.cpp` | Linux implementation (POSIX sockets, pthreads) |
| `settings.txt` | Configuration file (shared format for both platforms) |

## Features

- **HTTP Proxying** — Rewrites absolute URLs to relative, forwards requests to upstream servers, and relays responses back to the client.
- **HTTPS Tunneling** — Handles the `CONNECT` method for transparent HTTPS tunneling (bidirectional relay).
- **Multithreaded** — One thread per client connection for concurrent request handling.
- **Configurable** — All settings read from `settings.txt` (or a custom path via command-line argument).
- **Thread Limiting** — Rejects new connections with HTTP 503 when the thread limit is reached.
- **Timestamped Logging** — Logs to stdout and optionally to a log file. Thread-safe logging with mutex/critical section.
- **Connection Timeouts** — Non-blocking connect with configurable timeout for upstream connections.
- **Socket Timeouts** — Configurable receive/send timeouts on all sockets.
- **Large Header Protection** — Rejects requests with headers exceeding 64KB (HTTP 431).

## Architecture

```
                          ┌──────────────┐
   Browser ──────────────►│  Proxy Server │
   (HTTP proxy config)    │  :8080        │
                          └──────┬───────┘
                                 │
                    ┌────────────┼────────────┐
                    │            │            │
               Thread 1     Thread 2     Thread N
                    │            │            │
              ┌─────▼─────┐ ┌───▼───┐  ┌────▼────┐
              │ HTTP Fwd   │ │ HTTPS │  │ HTTP Fwd│
              │ Rewrite URL│ │CONNECT│  │         │
              │ Relay resp │ │Tunnel │  │         │
              └─────┬─────┘ └───┬───┘  └────┬────┘
                    │           │            │
                    ▼           ▼            ▼
              Upstream     Upstream     Upstream
              Server       Server       Server
```

### HTTP Request Flow

1. Client sends an HTTP request with an absolute URL (e.g., `GET http://example.com/path HTTP/1.1`).
2. Proxy parses the request, extracts the target host and port.
3. Proxy rewrites the request line to use a relative path (`GET /path HTTP/1.1`).
4. Proxy connects to the upstream server and forwards the request.
5. Proxy relays the response back to the client.

### HTTPS Tunnel Flow (CONNECT)

1. Client sends `CONNECT host:port HTTP/1.1`.
2. Proxy connects to the target host:port.
3. Proxy responds with `HTTP/1.1 200 Connection Established`.
4. Proxy enters bidirectional relay mode — all bytes are forwarded transparently in both directions.

## Configuration

### settings.txt

```ini
# Proxy Server Settings
# Lines starting with '#' are comments

# Port the proxy server listens on
listen_port=8080

# Address to bind to (0.0.0.0 = all interfaces, 127.0.0.1 = localhost only)
listen_address=0.0.0.0

# Maximum number of concurrent client connections
max_threads=100

# Receive/send buffer size in bytes
buffer_size=65536

# Connection timeout in seconds (for upstream connections)
connect_timeout=10

# Receive timeout in seconds
recv_timeout=30

# Enable logging (1 = on, 0 = off)
logging=1

# Log file path (empty = stdout only)
log_file=proxy.log
```

### Settings Reference

| Setting | Default | Description |
|---------|---------|-------------|
| `listen_port` | `8080` | TCP port the proxy listens on |
| `listen_address` | `0.0.0.0` | Bind address (`0.0.0.0` = all interfaces, `127.0.0.1` = localhost only) |
| `max_threads` | `100` | Maximum concurrent client connections. Excess connections receive HTTP 503. |
| `buffer_size` | `65536` | Read/write buffer size in bytes for socket I/O |
| `connect_timeout` | `10` | Timeout in seconds for connecting to upstream servers |
| `recv_timeout` | `30` | Receive/send timeout in seconds on all sockets |
| `logging` | `1` | Enable (`1`) or disable (`0`) logging |
| `log_file` | *(empty)* | Path to log file. If empty, logs go to stdout only. |

## Building

### Windows (MSVC)

```cmd
cl /EHsc /O2 ProxyServer.cpp /link ws2_32.lib
```

### Windows (MinGW g++)

```cmd
g++ -O2 -o ProxyServer.exe ProxyServer.cpp -lws2_32
```

### Linux (g++)

```bash
g++ -std=c++11 -O2 -pthread -o proxy_server proxy_server_linux.cpp
```

**Minimum C++ standard:** C++11

## Running

### Windows

```cmd
ProxyServer.exe                    # uses settings.txt in current directory
ProxyServer.exe myconfig.txt       # custom settings file
```

### Linux

```bash
./proxy_server                      # uses settings.txt in current directory
./proxy_server myconfig.txt         # custom settings file
```

## Browser Configuration

Configure your browser or application to use the proxy:

- **HTTP Proxy:** `<proxy_host>:<listen_port>` (e.g., `127.0.0.1:8080`)
- **HTTPS Proxy:** Same address — the proxy handles `CONNECT` tunneling.

### Example (Firefox)

1. Settings → Network Settings → Manual proxy configuration
2. HTTP Proxy: `127.0.0.1`, Port: `8080`
3. Check "Also use this proxy for HTTPS"

### Example (curl)

```bash
curl -x http://127.0.0.1:8080 http://example.com
curl -x http://127.0.0.1:8080 https://example.com
```

### Example (Environment Variable)

```bash
export http_proxy=http://127.0.0.1:8080
export https_proxy=http://127.0.0.1:8080
```

## Platform Differences

| Aspect | Windows (`ProxyServer.cpp`) | Linux (`proxy_server_linux.cpp`) |
|--------|------------------------------|----------------------------------|
| Sockets | Winsock2 (`SOCKET`, `closesocket`, `SD_BOTH`) | POSIX (`int` fd, `close`, `SHUT_RDWR`) |
| Threading | `CreateThread` / `CloseHandle` | `pthread_create` with `PTHREAD_CREATE_DETACHED` |
| Thread count | `InterlockedIncrement` / `InterlockedDecrement` | `std::atomic<int>` |
| Log mutex | `CRITICAL_SECTION` | `pthread_mutex_t` |
| Non-blocking | `ioctlsocket(FIONBIO)` | `fcntl(O_NONBLOCK)` |
| Socket timeout | `DWORD` milliseconds | `struct timeval` seconds |
| select() nfds | `0` (ignored on Windows) | `max_fd + 1` (required on Linux) |
| Log file open | `_fsopen(..., _SH_DENYNO)` (shared access) | `fopen()` (no locking issues on Linux) |
| Log file flush | `fflush` + `_commit` (forces metadata update) | `fflush` (sufficient on Linux) |
| Signal handling | N/A | `signal(SIGPIPE, SIG_IGN)` |
| Initialization | `WSAStartup` / `WSACleanup` | None needed |

## Log Output

Logs are timestamped and include client IP:port, method, target host:port, and path:

```
[2026-05-11 14:32:01] === Proxy Server Starting ===
[2026-05-11 14:32:01] Listen: 0.0.0.0:8080  MaxThreads: 100  Buffer: 65536
[2026-05-11 14:32:01] Listening on 0.0.0.0:8080 ...
[2026-05-11 14:32:05] [192.168.1.10:52341] GET example.com:80/index.html
[2026-05-11 14:32:06] [192.168.1.10:52342] CONNECT www.google.com:443
[2026-05-11 14:32:10] [192.168.1.10:52343] GET api.example.com:80/v1/data
```

## Error Responses

| Code | Condition |
|------|-----------|
| `400 Bad Request` | Malformed request or invalid port |
| `431 Request Header Fields Too Large` | Header exceeds 64KB |
| `502 Bad Gateway` | Cannot connect to upstream server |
| `503 Service Unavailable` | Thread limit reached |

## Firewall Notes

If running the proxy on a remote Linux machine and clients cannot connect, check that the firewall allows inbound traffic on the listen port:

```bash
# firewalld (CentOS/RHEL)
sudo firewall-cmd --add-port=8080/tcp --permanent
sudo firewall-cmd --reload

# iptables
sudo iptables -I INPUT -p tcp --dport 8080 -j ACCEPT

# ufw (Ubuntu/Debian)
sudo ufw allow 8080/tcp
```

A pcap showing only SYN packets with no SYN-ACK responses is a strong indicator that a firewall is silently dropping inbound packets.
