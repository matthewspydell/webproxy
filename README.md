This the the README for webproxy.c

To compile the project enter,
"gcc -lcrypto webproxy.c -o webproxy"

To run the proxy enter,
"./webproxy <port> <timeout>"
If a timeout is not given it will default to 60 seconds
Port must match the port on your web browser

Included is a blacklist file that can be used to block websites.
Each host must be entered on a new line and follow the host format
that is proved in a HTTP GET request