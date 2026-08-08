# simple-lib-wireguard

A WireGuard client for PS3 homebrew. An app links this library, opts in at runtime, and its network
traffic runs inside the tunnel. The console as a whole is unaffected.

Status: working. It connects to a real VPN server, carries UDP and TCP both ways, looks up host
names through the tunnel, keeps itself alive and replaces its keys on schedule. Verified against
ProtonVPN on real hardware: name lookups, a web page fetched over TCP, and ten minute runs across
repeated key changes.

## What is here

| File | What it does |
|---|---|
| `wg-bytes.h` | the only place bytes become integers |
| `blake2s.c` | BLAKE2s hash, keyed MAC and HMAC (RFC 7693) |
| `chacha20.c` | ChaCha20 stream cipher (RFC 8439) |
| `poly1305.c` | Poly1305 authenticator (RFC 8439) |
| `chacha20-poly1305.c` | the AEAD built from those two |
| `x25519.c` | X25519 key exchange (RFC 7748) |
| `wg-random.c` | key material, from the console's own random number source |
| `wg-config.c` | the ordinary `.conf` a VPN provider hands out |
| `wg-handshake.c` | the Noise handshake, both starting one and answering one |
| `wg-session.c` | encrypting and decrypting packets once keys are agreed |
| `wg-replay.c` | refusing a packet counter that has already been seen |
| `wg-transport.c` | the single UDP socket to the server |
| `wg-tunnel.c` | the state machine: connect, keep alive, rekey, answer pings |
| `wg-ip.c` | building and reading the IP packets that go inside the tunnel |
| `wg-dns.c` | name lookups, so they cannot go out in the clear |
| `wg-tcp.c` | streams: starting, numbering, confirming, resending, stopping |
| `wg-net.c` | what an app calls: sockets, streams and name lookups through the tunnel |
| `wg-selftest.c` | the published test vectors, run on the console |

## What an app calls

`wg-net.h` is the whole surface. There is no handle to pass around: one app, one VPN.

```c
startWgNetwork("/dev_hdd0/tmp/swarm/wireguard.conf");
...
serviceWgNetwork(500);                       // once per turn of the app's loop
resolveWgHost("example.com", &address, 3000);
WgSocket socket = openWgUdp(0);
sendWgTo(socket, &endpoint, data, length);
recvWgFrom(socket, &from, buffer, sizeof buffer);
```

TCP is the same shape:

```c
WgTcpSocket stream = connectWgTcp(address, 80, 5000);
sendWgTcp(stream, request, length, 5000);
recvWgTcp(stream, buffer, sizeof buffer);
closeWgTcp(stream);
```

`connectWgTcp` waits for the other end to answer. For many peers at once, open them and carry on:

```c
WgTcpSocket stream = openWgTcp(address, 6881);   // returns at once
...
serviceWgNetwork(20);
if (!isWgTcpConnecting(stream) && !isWgTcpFailed(stream)) sendWgTcp(stream, request, length, 5000);
```

The hello is sent again by `serviceWgNetwork` if it goes unanswered, and the stream marks itself
failed after about fifteen seconds. Sixteen peers that never answer cost fifteen seconds in total
rather than fifteen each.

Every one of those fails while the tunnel is down, and there is no way to ask for a packet to be
sent outside it. That is the kill switch, and it works because the library has no other path to
offer. An app that wants the ordinary network connection does not use this library.

## Limits worth knowing

- Thirty two UDP sockets and sixteen TCP streams at once. A stream takes 384 KB of buffer while it
  is open, asked for when it opens and given back when it closes, so an app that holds two streams
  pays for two rather than for sixteen.
- Segments carry up to 1360 bytes, which is what this console and ProtonVPN were measured to take.
  `measureWgPacketLimit` lowers it for a server or a link that carries less; nothing raises it.
- Nothing runs on its own: the app must call `serviceWgNetwork` regularly or the tunnel stalls.
- Arriving UDP is taken as it comes: its checksum is not checked, so damage between the VPN server
  and the far end would be accepted rather than caught. TCP segments are checked and damaged ones
  are dropped, which makes the other end send them again.

## Three things that are not optional

Each of these was found the same way: the tunnel appeared to work, then quietly misbehaved.

**Answer handshakes the server starts.** Either peer can begin one, and a client that only ever
starts its own looks dead. Without this the server rebuilt the session every 19 seconds.

**Never be silent for more than ten seconds.** Whoever spoke last. The server gives up fifteen
seconds after it last sent with nothing coming back, and rebuilds the session. Answering only when
they spoke last leaves the other half open, and the provider's twenty five second keepalive is too
slow to cover it: measured, the server rebuilt the session after nine of the ten exchanges in a ten
minute run. The configured interval may only shorten this, never lengthen it.

**Keep the replaced keys for a while after a key change.** Packets already on their way still carry
the old ones. Dropping them cost one failed request at every key change.

## Byte order

The PS3 is big-endian and most crypto bugs on it come from that. This library has no endian
handling to get wrong: every primitive is specified on octets, and `wg-bytes.h` is the only code
that converts between bytes and wider integers. Nothing casts a byte buffer to a `uint32_t *`.

## Verification

`runWgSelfTest()` checks every primitive against its published vector and returns the number of
failures. Sources are named in comments beside each vector: RFC 8439 for ChaCha20, Poly1305 and the
AEAD, RFC 7693 for BLAKE2s, RFC 7748 for X25519.

It also runs the stream layer through the two cases an ordinary download never produces: a server
that sends the last of a page and closes in the same packet, and a peer that sends a byte into a
full receive ring. Both were handled wrongly until they were checked here.

Everything in the library is covered. The keyed BLAKE2s, HMAC and XChaCha20 that WireGuard also
needs are deliberately absent: they arrive with the handshake and the cookie handling, each with
the vector that proves it. Nothing ships here without one.

## Speed

Measured on the console, encrypting and decrypting packet-sized blocks: **32 MB/s each way**. That
is well above anything the console's own network port can carry, so the crypto is not what limits a
transfer and does not need optimising. X25519 and Poly1305 both work in small pieces, which keeps
them easy to check against their test vectors, and neither shows up: a handshake happens once every
two minutes, and Poly1305 is already inside that 32 MB/s.

What did limit transfers was the stream code, and three things fix it.

**Several packets in flight at once.** Sending one and waiting for it to be confirmed moves one
packet per round trip, which against a 68 ms round trip is 17 KB/s however fast the link is. Data
waiting to go out sits in a 128 KB ring, and as much of it as the other end and the link will take
is on its way at any moment.

**A window big enough to ask for.** Speed in the other direction is the window divided by the round
trip, and TCP's window field is only 16 bits, so 64 KB is the most that can be asked for without
RFC 7323 scaling. With scaling and a 256 KB ring, one stream can take 3.7 MB/s at that round trip.

**Holding data that arrives early.** Anything out of order used to be thrown away and asked for
again, which is harmless when one packet is in flight and ruinous when a hundred are. Arriving data
is written into the receive ring wherever in the stream it belongs, and the runs that have arrived
are tracked until the gaps in front of them fill. There is room for sixty four such runs: with eight,
measured on four streams at once, the list stayed full and every stream stopped, because a run that
cannot be remembered is sent again and then dropped again.

Around those sit the ordinary things a stream needs. The resend timer is the measured round trip
rather than a fixed second. Three repeated acknowledgements resend at once instead of waiting for
that timer. One acknowledgement covers two arriving segments. The amount in flight grows on success
and halves on loss, so a link that is already full gets a share rather than more traffic.

Measured end to end over WiFi through ProtonVPN, fetching a 100 MB file: **1.8 to 2.2 MB/s** on one
stream, varying that much between runs on the same build. Many streams at once used to come to less
than one did, which is what the early-arrival tracking above fixed: a torrent download spread over
about twenty eight streams now runs at 1.3 to 1.6 MB/s.

## Credits

The WireGuard protocol is Jason A. Donenfeld's. This is written from the protocol paper and the RFCs
named in the table above; no code was taken from the original implementation.
