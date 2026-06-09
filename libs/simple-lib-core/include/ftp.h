#pragma once

#include <stdint.h>

// Standard FTP control port; the usual argument to startFtpServer.
#define FTP_DEFAULT_PORT 21

// Result of startFtpServer.
typedef enum {
   FTP_OK = 0,
   FTP_ALREADY_RUNNING,
   FTP_PORT_IN_USE,
   FTP_NETWORK_UNAVAILABLE,
   FTP_THREAD_CREATE_FAILED
} FtpResult;

// Running state of the (single) FTP server.
typedef enum {
   FTP_STOPPED = 0,
   FTP_STARTED
} FtpState;

// Returns 1 if nothing currently holds the port (it can be bound), else 0.
// Used to detect another FTP server already running before offering to start.
int isFtpPortAvailable(uint16_t port);

// Reports whether this app's FTP server is currently running.
FtpState isFtpServerRunning(void);

// Starts the FTP server on the given port. The server is a singleton, so the
// caller keeps no handle; query isFtpServerRunning and call stopFtpServer.
FtpResult startFtpServer(uint16_t port);

// Stops the server and waits for sessions to drain. Safe to call when stopped.
void stopFtpServer(void);
