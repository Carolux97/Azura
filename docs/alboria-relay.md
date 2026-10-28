# Alboria relay: co-op over the internet

How Azura plays System Link with players on other networks through Alboria, our own online service:
what the runtime and the server say to each other. The server's code is private; this
document is the contract the public client code follows.

## Contents

1. [Summary](#1-summary)
2. [How a game is played](#2-how-a-game-is-played)
3. [Identity and bans](#3-identity-and-bans)
4. [Control API (HTTPS)](#4-control-api-https)
5. [Relay protocol (UDP)](#5-relay-protocol-udp)
6. [The runtime side](#6-the-runtime-side)
7. [Limits](#7-limits)
8. [What the server keeps](#8-what-the-server-keeps)
9. [Versions](#9-versions)

## 1. Summary

The game keeps playing System Link as on a local network. The local network is a **room** on the
relay instead: the runtime sends the game's System Link packets, broadcasts included, to the relay,
and the relay passes them only to the other members of the same room. Every player connects out to
the relay, so nobody opens ports and nobody sees anyone else's IP address.

There are two channels:

- **Control** (HTTPS, `https://api.alboria.org/v1/`): the runtime signs in with the installation's
  key, creates a room or joins one with its code, and receives a **ticket** for the relay.
- **Relay** (UDP, the address in the ticket): the runtime carries the game's traffic, encrypted and
  authenticated with the ticket's key.

## 2. How a game is played

Everything happens inside the game, with `network_mode = "online"` (the default).

1. The host opens the party options and chooses **Joinable** or **Invite only**. The runtime signs
   in (section 3), creates a room and receives a ticket with the room's code, such as `K7QM-2XPA`;
   underneath, the game creates a System Link party. **System Link** stays the real local network
   and **Local** stays local.
2. In Manage/Invite, LB shows or hides the room's code next to the players heading. It is hidden
   each time the screen opens.
3. The host gives the code to friends. Each one, in a party on Alboria, presses RB (Join by code)
   in Manage/Invite and types it. The runtime joins the room, then opens the game's System Link
   browser, which finds the host's party in the room and joins it.
4. The runtime's local network is the room: each member has a virtual address `10.77.0.x` (the
   host `10.77.0.1`), which is the address the game sees in its XNADDR.
5. The room closes when the host leaves it, or when nobody sends anything for 2 minutes.

## 3. Identity and bans

Each installation has an Ed25519 key, made the first time the game runs and kept in `identity.toml`
(the key's 32-byte seed). The public key is the player's identity for the service.

- Signing in proves the player holds the key: the server sends a random challenge and the runtime
  signs it (section 4.1).
- A ban is on a public key, an IP address, or both. A banned player cannot sign in, create a room
  or join one, and is removed from the rooms they are in.
- A player who deletes `identity.toml` gets a new key. Bans that survive that need accounts, which
  come later.

## 4. Control API (HTTPS)

JSON over HTTPS. Binary values (keys, challenges, signatures) are standard base64 with padding.
Errors answer with a status code and `{"error": "<code>", "message": "<text>"}`.

### 4.1 Status and sign in

`POST /v1/status` with `{}` answers `{ "status": "ok" }` while the service is up. The runtime asks
at start-up and whenever the party options open. While it has no answer, choosing Joinable or
Invite only shows a message saying Alboria cannot be reached.

`POST /v1/auth/challenge`

```json
{ "public_key": "<32 bytes>" }
```

Answer: `{ "challenge": "<32 bytes>", "expires_in": 60 }`.

`POST /v1/auth/session`

```json
{
  "public_key": "<32 bytes>",
  "challenge": "<32 bytes>",
  "signature": "<64 bytes>",
  "gamertag": "Carolina",
  "client": "azura/0.5.0"
}
```

`signature` is the Ed25519 signature of the bytes `alboria-auth-v1` (15 bytes) followed by the
challenge. A challenge is used once and expires after 60 seconds.

Answer: `{ "token": "<opaque>", "expires_in": 3600 }`. Later requests send it as
`Authorization: Bearer <token>`.

Errors: `400 bad_request`, `401 bad_signature`, `401 challenge_expired`, `403 banned`,
`426 client_too_old`.

### 4.2 Host a game

`POST /v1/rooms`

```json
{ "title_id": "4D5308AB", "max_members": 4 }
```

`max_members` is 2 to 10. Answer: a ticket (section 4.4).

### 4.3 Join a game

`POST /v1/rooms/join`

```json
{ "code": "K7QM-2XPA" }
```

The code is case insensitive and the dash is optional. Answer: a ticket.

Errors: `404 room_not_found`, `409 room_full`, `403 banned`, `409 wrong_title` (the room is for
another game; the request then has to carry `title_id` too).

### 4.4 The ticket

```json
{
  "code": "K7QM-2XPA",
  "relay": "relay.alboria.org:27450",
  "connection_id": 2864434397,
  "key": "<32 bytes>",
  "virtual_ip": "10.77.0.2",
  "host": false,
  "expires_in": 300
}
```

- `connection_id`: identifies this member on the relay, in every packet.
- `key`: the member's key for the relay (section 5.2). It only travels inside HTTPS and is never
  written to logs or files.
- `expires_in`: seconds to send the first packet to the relay. Once connected, the member stays
  while it keeps sending.

The runtime keeps the ticket in memory only.

## 5. Relay protocol (UDP)

### 5.1 Packets

Every packet, in both directions:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | Magic `0xA1 0xB0` |
| 2 | 1 | Protocol version, 1 |
| 3 | 1 | Type |
| 4 | 4 | Connection ID (big endian) |
| 8 | 8 | Counter (big endian) |
| 16 | n | Encrypted body |
| 16 + n | 16 | Poly1305 tag |

The first 16 bytes are the header. It is not encrypted but it is authenticated (it is the AEAD's
associated data). The largest packet is 1452 bytes, so the body is at most 1420 bytes.

### 5.2 Encryption

ChaCha20-Poly1305 (RFC 8439) with the ticket's key. The 12-byte nonce is the direction byte (`0x00`
from the member to the relay, `0x01` from the relay to the member), 3 zero bytes, and the 8 bytes of
the counter. Each side starts its counter at 1 and adds 1 for every packet it sends, so a nonce is never
used twice with the same key.

A receiver drops a packet that does not decrypt, and one whose counter it has already accepted or
that is more than 1024 behind the highest one accepted (replay protection).

### 5.3 Types

| Type | Name | Direction | Body |
| --- | --- | --- | --- |
| 1 | `HELLO` | to relay | empty |
| 2 | `WELCOME` | from relay | virtual IP (4), keepalive interval in seconds (2) |
| 3 | `DATA` | both | address (4), source port (2), destination port (2), game data |
| 4 | `PING` | both | empty |
| 5 | `PONG` | both | empty |
| 6 | `BYE` | to relay | empty |
| 7 | `CLOSED` | from relay | reason (1): 1 room closed, 2 banned, 3 timed out, 4 replaced |

Addresses are IPv4 addresses as big-endian numbers, ports are big endian.

- The member sends `HELLO` until it gets `WELCOME` (every second, up to 10 times). The relay takes
  the address the packet came from as the member's address, and updates it whenever an
  authenticated packet comes from a new one (a router that changes the player's port).
- `DATA` to the relay: the address is the destination, a member's virtual address or a broadcast
  (`255.255.255.255` or `10.77.255.255`), which goes to every other member. `DATA` from the relay:
  the address is the sender's virtual address. The ports are the ones the game uses (port 1000 is
  1000 here, with no offset).
- A member with nothing to send sends `PING` every keepalive interval (5 seconds); the relay answers
  `PONG`. A member silent for 30 seconds is removed.
- `BYE` leaves the room at once. When the host leaves, the room closes and the others get `CLOSED`.
- A second `HELLO` with the same connection ID from another address replaces the first one; the old
  address gets `CLOSED` with reason 4.

## 6. The runtime side

With `network_mode = "online"`, the runtime connects to the relay once it has a ticket, and while
it is in a room the game sees:

- `XNetGetEthernetLinkStatus` reports a link, so System Link is offered as on a local network.
- Its XNADDR has its virtual address and the machine ID of `identity.toml`.
- Its datagram sockets (UDP and VDP) send through the relay: `sendto` to a member's virtual address
  or to a broadcast becomes `DATA`.
- What arrives from the relay reaches the game's socket through a socket on the loopback interface
  for each remote member, so `select`, blocking reads and `FIONREAD` keep working as with any other
  socket; `recvfrom` gives the game the member's virtual address and port as the sender.

Stream (TCP) sockets are not carried. Whether Gears 3 uses them for System Link has not been
checked yet.

## 7. Limits

- Up to 10 members per room.
- Per member: 1000 packets and 256 KB per second; above that, packets are dropped.
- Per IP address: 10 sign-ins and 5 new rooms per minute.
- Rooms close after 2 minutes with no traffic, and a code stops working when its room closes.

## 8. What the server keeps

- Bans: public key or IP address, reason and date, until removed.
- For each room, for 7 days: its code, title ID, times, and the public key and gamertag of each
  member, so that a report about a player can be checked. No IP addresses and no game traffic.
- Counts for the trial of the service: rooms per day and different keys per month, with no other
  data.

## 9. Versions

The relay's version byte and the API's `/v1/` change only when a change breaks older clients. The
server answers `426 client_too_old` to a `client` it no longer supports, and the runtime then asks
the player to update.
