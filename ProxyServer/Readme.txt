proxy_server.exe              # uses settings.txt in current directory
proxy_server.exe myconf.txt   # custom settings file


g++ -O2 -pthread -o proxy_server proxy_server_linux.cpp

./proxy_server                # uses settings.txt in current directory
./proxy_server myconf.txt     # custom settings file