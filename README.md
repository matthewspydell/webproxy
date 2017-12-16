# This is a simple webproxy

#### Compilation
```bash
gcc -lcrypto webproxy.c -o webproxy
```

#### Running
```bash
./webproxy <port> <timeout>
```
If a timeout is not given it will default to 60 seconds.
You must setup your web browser to connect to localhost on the same port.

Included is a blacklist file that can be used to block websites. Each host, or IP, must be entered on a new line and follow the host format that is given in a HTTP request.

#### blacklist file example
```
www.apple.com
www.google.com
129.55.110.80
```
