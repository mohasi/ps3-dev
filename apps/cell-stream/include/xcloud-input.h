#pragma once

// xcloud-input - sending what the player is doing back to the machine.
//
// Presses travel on their own channel, out of order and never repeated: one that arrives late is
// already wrong, and sending it again would be worse than letting it go.

// Tells the machine what kind of controller it is dealing with. Sent once, as soon as the channel
// for it is open.
void sendXcloudInputStart(int channel);

// Reads the controller as it stands and sends it. Called every time round the loop: the machine
// expects the whole state each time, not just what changed.
void sendXcloudGamepad(int channel);
