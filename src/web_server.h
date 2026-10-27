#ifndef WEB_SERVER_H
#define WEB_SERVER_H

// Initialize web server
void initWebServer();

// Call from the main loop: refreshes the cached event / command history that
// the dashboard reads, so no HTTP request is made from the async handlers.
void webServerLoop();

#endif // WEB_SERVER_H
