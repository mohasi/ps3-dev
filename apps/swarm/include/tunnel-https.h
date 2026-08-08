#pragma once

// The bytes under TLS, carried by the WireGuard tunnel instead of the console's own network.
//
// simple-lib-https drives a socket itself, which would leave the tunnel and hand the site our real
// address. Binding this puts every https request on the tunnel: the name is looked up through it,
// the connection is one of ours, and the certificate checking above is unchanged.
//
// Call it once, after the tunnel is up and before any request. There is no matching unbind: an app
// that has taken this route stays on it.
void useTunnelForHttps(void);
