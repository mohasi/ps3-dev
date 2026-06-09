#pragma once

#include <stdint.h>

typedef struct FtpServer FtpServer;

int isFtpPortAvailable(uint16_t port);
FtpServer *startFtpServer(uint16_t port);
void stopFtpServer(FtpServer *server);
