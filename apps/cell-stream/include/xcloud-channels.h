#pragma once

// xcloud-channels - what the console says to the machine once there is a connection, and what it
// makes of what comes back.
//
// The machine sends nothing until it is asked. Four named channels are opened, a greeting goes out
// on one of them, and only after the machine answers that greeting does anything describing the
// picture arrive.

#include "sctp.h"

// Opens the channels the machine expects to find. 0 or -1.
int openXcloudChannels(void);

// Carries the conversation forward: sends the greeting once its channel is ready, and answers what
// arrives. Called with each message the connection hands over.
void advanceXcloudChannels(const SctpMessage *message);

// Called when nothing arrived, so anything waiting on a channel that has since opened can go.
void pollXcloudChannels(void);

// Sends the controller as it stands, if the machine is ready for it. Does nothing until then.
void sendXcloudControllerState(void);

// The picture size asked for, in lines: 720 or 1080. The console outputs 1080p either way, so 720
// is stretched to fill it. Chosen before a session starts; changing it later has no effect on one
// already running.
void loadXcloudStreamHeight(void);   // restores the size chosen last time; call once before the list
int getXcloudStreamHeight(void);
void setXcloudStreamHeight(int height);   // remembers it too, so the next session starts the same way
