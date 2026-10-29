# Alboria relay: co-op over the internet

How Azura plays System Link with players on other networks through Alboria, our online ecosystem
(for Gears of War 3 and the games that come after it): what the launcher, the runtime and the server
say to each other. The server's code is private; this
document is the contract the public client code follows.

## Contents

1. [Summary](#1-summary)
2. [How a game is played](#2-how-a-game-is-played)
3. [Accounts, identity and bans](#3-accounts-identity-and-bans)
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

## 3. Accounts, identity and bans

An Alboria account belongs to a player, for every game that uses Alboria. It is their **email**,
with no password: a 6-digit code sent to it proves it is theirs (section 3.1).

Each game installation is a **device** of the account. It has an Ed25519 key, kept in
`identity.toml` (the key's 32-byte seed, in the game's saves folder), and signs in with it
(section 4.1). The launcher links the device to the account the first time: when the player creates
an account or signs in to theirs from the launcher's welcome screen (section 4.6). Each game links
its own installation, so a player with two games on a computer has two devices. A player who
chooses a local account has no device: the game plays Local and System Link only, with no Alboria.

- The account shows as `Gamertag#tag`, such as `Carolux#3290`, the same in every game. The server
  gives each account a four-digit tag, 1000 to 9999, when it is created, so that no two players
  share a gamertag and tag (ignoring case). Tags of 1 to 999 are special (developers and people
  chosen by the project): only the server's administration tool gives them, and clients show them as
  they are, `Carolux#97`, never padded. Changing the gamertag keeps the tag when nobody else holds
  it with the new gamertag, and gives another four-digit one when somebody does.
- Signing in proves the device holds its key: the server sends a random challenge and the device
  signs it (section 4.1). A key no account has linked is refused with `no_account`.
- A player who loses `identity.toml` (a new computer, a reinstall that removes the saves folder)
  signs in to their account again from the launcher: the new key becomes a new device, with the same
  identity. The old one can be removed from the Account screen.
- A ban is on an account (every device it has, and every one it links later), a device's key, a
  computer (section 3.2), an IP address, or a key and an address. A banned player cannot sign in,
  ask for a code, create a room or join one, and is removed from the rooms they are in.
- So that a banned player cannot come back with a new account, banning an account also bans, with
  it: each of its devices' keys (which stay banned if they move to another account), each of their
  computers, and the last address of each device for 30 days, only for new accounts and new links:
  who already plays from that address, such as someone in the same home, keeps playing. Removing
  the account's ban removes them too.
- One mailbox makes one account: the server compares emails in lower case, without a `+suffix`,
  and for Gmail without dots, so `Carolina.M+azura@gmail.com` and `carolinam@gmail.com` are the
  same account. Emails on throwaway domains (the public disposable-email-domains list) are refused.

### 3.1 Codes

A code is 6 digits, sent by email in the player's language. It works once, for 10 minutes, and
stops working after 5 wrong guesses. Another code for the same email can be asked for after 60
seconds, and replaces the one before; an email gets at most 5 codes an hour, and an IP address may
ask for 10 an hour. A code is for one purpose: creating the account, linking a device to it, moving
it to another email (the code goes to the new one) or deleting it.

### 3.2 The computer's ID

When the launcher links a device, it sends `machine_id`: the SHA-256 of the bytes
`alboria-machine-v1` (18 bytes), a zero byte and the computer's own ID in lower case UTF-8, with no
spaces around it. The ID is the one the operating system keeps for the computer and does not change
when the game is reinstalled:

| System | ID |
| --- | --- |
| macOS | `IOPlatformUUID` of the `IOPlatformExpertDevice` in the I/O Registry |
| Linux | `/etc/machine-id` (`/var/lib/dbus/machine-id` when it is missing) |
| Windows | `MachineGuid` in `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Cryptography` |

The server only ever sees the hash, which tells it two devices are on the same computer and
nothing else. The ID is read by the launcher, never sent as it is, and never written to logs.

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

Answer: `{ "token": "<opaque>", "expires_in": 3600, "gamertag": "Carolina", "tag": 97 }`, with
the account's gamertag and tag (section 3). Later requests send the token as
`Authorization: Bearer <token>`.

`client` is the game (`azura/0.5.0`) or the launcher (`azura-launcher/0.5.0`). The key must be a
device of an account (section 3).

Errors: `400 bad_request`, `401 bad_signature`, `401 challenge_expired`, `403 banned`,
`403 no_account` (no account has linked this key), `409 gamertag_taken` (every tag of that gamertag
is held), `426 client_too_old`.

### 4.2 Host a game

`POST /v1/rooms`

```json
{ "title_id": "4D5308AB", "max_members": 4, "party_type": "joinable" }
```

`max_members` is 2 to 10. `party_type` is who may join, as the host chose it in the game's party
options: `joinable` (anyone with the code) or `invite_only` (who the host invites; for now also with
the code). It is `joinable` when missing, as the first clients send it. A room is never System Link,
which is the local network. Answer: a ticket (section 4.4).

`POST /v1/rooms/type` with `{ "code": "K7QM-2XPA", "party_type": "invite_only" }` changes the
party type of a room the player hosts; the relay then sends every member `ROOM` (section 5.3), and
who joins later gets the new type in the ticket. Answer: `{ "party_type": "invite_only" }`. Errors:
`400 bad_request`, `403 not_host`, `404 room_not_found`.

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
  "party_type": "joinable",
  "session": null,
  "expires_in": 300
}
```

- `connection_id`: identifies this member on the relay, in every packet.
- `party_type`: the room's, the same for every member: a member who joins by code shows the host's
  Joinable or Invite only in the game's party options and party panel.
- `session`: the host's Xbox Live party session once the host has published it (section 4.5), as
  base64; `null` before, and for rooms of System Link parties. Clients read every field of an
  answer without assuming its type: a field may be `null`.
- `qos`: the data the host's game answers quality of service probes with (section 4.5), as base64;
  `null` before the host publishes it, and for rooms of System Link parties.
- `stream`: the stream port's address as players reach it (section 5.5), such as
  `relay.alboria.org:27451`. A server without streams leaves it out.
- `key`: the member's key for the relay (section 5.2). It only travels inside HTTPS and is never
  written to logs or files.
- `expires_in`: seconds to send the first packet to the relay. Once connected, the member stays
  while it keeps sending.

The runtime keeps the ticket in memory only.

### 4.5 Xbox Live parties (work in progress)

With `alboria_live` (off by default while it is being built) the game believes the player is
signed in to Xbox Live, and its own Joinable and Invite only parties are Alboria's: no System Link
party underneath (section 6.1).

`POST /v1/rooms/session` with `{ "code": "K7QM-2XPA", "session": "<60 bytes>" }` stores the
session of the Live party the player hosts in the room: its XSESSION_INFO, that is the session ID
(XNKID, 8 bytes), the host's XNADDR (36) and the key (XNKEY, 16). Members who join afterwards get it
in the ticket's `session`. Answer: `{}`. Errors: `400 bad_request`, `403 not_host`,
`404 room_not_found`.

`POST /v1/rooms/qos` with `{ "code": "K7QM-2XPA", "qos": "<1 to 1024 bytes>" }` stores the data
the host's game listens for quality of service probes with (`XNetQosListen`); the game sends it again
when it changes. Members who join afterwards get it in the ticket's `qos`, and the game reads the
party's settings from it before joining. Answer: `{}`. Errors: `400 bad_request`, `403 not_host`,
`404 room_not_found`.

### 4.6 Accounts

The launcher's requests for the account (section 3). Errors that ask the player to wait answer
`429 too_many_requests` with `retry_in` (seconds) next to the error.

`POST /v1/account/code` asks for a code (section 3.1):

```json
{ "purpose": "create", "email": "carolina@example.com", "gamertag": "Carolux97", "language": "es" }
```

`purpose` is `create` (with the gamertag the account will have) or `signin` (to link this device to
the account of the email). The launcher also sends `machine_id` (section 3.2), so that a banned
computer is refused before an email goes out. `language` is one of the launcher's languages (`en`, `es`, `fr`, `de`,
`it`, `pt`, `pl`, `cs`, `hu`, `ru`, `ja`, `ko`, `zh-Hant`); any other gets English. Answer:
`{ "expires_in": 600, "resend_in": 60 }`. Errors: `400 bad_request`, `400 disposable_email`
(a throwaway domain), `400 email_suppressed` (emails to that address bounced for good or were
marked as spam, so they are no longer delivered: use another one), `403 banned`,
`404 no_account` (signin: no account has that email), `409 email_taken` (create: that email has an
account), `429 too_many_requests`, `503 mail_failed`.

`POST /v1/account/verify` uses the code from the device, which proves its key as signing in does:

```json
{
  "purpose": "create",
  "email": "carolina@example.com",
  "code": "482190",
  "public_key": "<32 bytes>",
  "challenge": "<32 bytes>",
  "signature": "<64 bytes>",
  "client": "azura-launcher/0.5.0",
  "title_id": "4D5308AB",
  "platform": "Mac",
  "machine_id": "<32 bytes>"
}
```

It creates the account and links the device to it, or links the device to the account of the
email; a key linked to another account moves to this one. `machine_id` is required (section 3.2),
and the server keeps it and the request's address with the device. Answer: as `/v1/auth/session`
(`token`, `expires_in`, `gamertag`, `tag`). Errors: those of `/v1/auth/session`, and
`401 bad_code` (with `attempts_left`), `404 no_account`, `409 email_taken`, `410 code_expired`
(expired, used up or never sent: ask for another one).

With the token of a signed-in device:

| Request | Body | Answer |
| --- | --- | --- |
| `POST /v1/account` | `{}` | `email`, `gamertag`, `tag` and `devices`: each with `public_key`, `title_id`, `platform`, `created_at`, `last_seen_at` and `current` (the device asking) |
| `POST /v1/account/devices/remove` | `{ "public_key": "…" }` | `{}`; the device is signed out at once, and signs in again only after it is linked again. `404 no_device` |
| `POST /v1/account/email/code` | `{ "email": "…", "language": "es" }` | as `/v1/account/code`; the code goes to the new email. `409 email_taken` |
| `POST /v1/account/email/verify` | `{ "email": "…", "code": "…" }` | `{ "email": "…" }` |
| `POST /v1/account/export` | `{}` | everything the server keeps about the account (section 8), as JSON |
| `POST /v1/account/delete/code` | `{ "language": "es" }` | as `/v1/account/code`; the code goes to the account's email |
| `POST /v1/account/delete` | `{ "code": "…" }` | `{}`; the account and its devices are deleted, and every device is signed out |

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
| 1 | `HELLO` | to relay | flags (1), local address (4), local port (2); empty from the first clients |
| 2 | `WELCOME` | from relay | virtual IP (4), keepalive interval in seconds (2) |
| 3 | `DATA` | both | address (4), source port (2), destination port (2), game data |
| 4 | `PING` | both | empty |
| 5 | `PONG` | both | empty |
| 6 | `BYE` | to relay | empty |
| 7 | `CLOSED` | from relay | reason (1): 1 room closed, 2 banned, 3 timed out, 4 replaced |
| 8 | `PEERS` | from relay | count (1), then 48 bytes per peer (section 5.4) |
| 9 | `PROBE` | member to member | the sender's clock (8) |
| 10 | `PROBE_ACK` | member to member | the `PROBE`'s body, echoed |
| 11 | `DIRECT` | member to member | as `DATA` to the relay |
| 12 | `ROOM` | from relay | the room's party type (1): 1 joinable, 2 invite only |
| 13 | `STREAM` | from relay | stream ID (4), the opening member's virtual address (4) and port (2), the port it is for (2) (section 5.5) |

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
- `DATA` to port 0 never reaches the game: it is the latency echo between two members through the
  relay. Its payload is a kind (1 request, 2 reply) and the sender's 8-byte clock, which the reply
  echoes.

### 5.4 Direct connections

Members that take direct connections reach each other without the relay when their routers allow
it, and fall back to it when they do not. The relay stays in charge of the room: who is in it, the
keys, and bans.

- **Asking for it.** A `HELLO`'s flags have bit 0 set (`alboria_direct`, on by default), with the
  member's address on its own network and the port of its socket there, for members behind the
  same router. The socket is the one that talks to the relay: the mapping its router opened for the
  relay is what lets the other members' packets in (UDP hole punching).
- **`PEERS`.** Whenever who is in a room or where a member is changes, the relay sends each member
  that asked the list of the others that asked: for each one its virtual address (4), its address
  and port as the relay sees them (4, 2), its local address and port (4, 2), and a 32-byte key for
  that pair of members, made by the relay and only ever sent inside this encrypted channel. The list
  replaces the last one: a member missing from it left, was banned or stopped taking direct
  connections, and its key is forgotten. A member who did not ask gets no list and is never listed.
- **Packets between members** have the header of section 5.1 with the sender's virtual address as
  the connection ID, sealed with the pair's key. The nonce's direction byte is `0x02` from the member
  with the lower virtual address and `0x03` from the other, so the two never share a nonce. Each
  member keeps a counter and a replay window per peer.
- **Finding a path.** Both members send `PROBE` to the other's public address (and local one) every
  200 ms for 5 seconds; the first that gets through is answered with `PROBE_ACK` from where it
  arrived. A `PROBE_ACK` makes the path direct: from then on the game's packets to that member go as
  `DIRECT`. Without an answer, the members keep using the relay and try again every 10 seconds.
- **Keeping it.** On a direct path a `PROBE` goes every second, which keeps the routers' mappings
  open and measures the round trip. A path with nothing from the other side for 6 seconds falls back
  to the relay. Broadcasts always go through the relay.
- **What it costs.** The members of a room learn each other's public address. Players who do not
  want that turn the setting off (Options > Connection in the game) and everything goes through the
  relay. Neither the game nor the launcher ever shows an address.

### 5.5 Streams (the game's TCP connections)

**The design.** The game plays over UDP, directly between members when their routers allow it and
through the relay otherwise (sections 5.1 to 5.4). Its few TCP connections, which carry
signalling rather than play (Gears of War 3's party beacons: asking the host for a place in its
party, and its answer), always go through the server, over real TCP. TCP gives them reliable,
ordered delivery for free, they work behind any router, and the latency they add is not felt where
they are used. This split, signalling over TCP through the server and the game peer to peer over
UDP, is the model for the next games on Alboria too.

**How it works.** The server listens on a TCP port (27451, the ticket's `stream` field). Both
members connect out to it, so no router has to let anything in:

1. A member whose game connects to another member's virtual address opens a connection to the
   stream port with the role *dial*, a stream ID of its choosing, and a first record with the
   address, the port it wants and its own port.
2. The relay tells the member it is for with `STREAM` (section 5.3).
3. That member connects to the stream port with the role *answer* (or *refuse*, when nothing of its
   game listens on that port) and the same stream ID.
4. The server pairs the two connections and tells the dialing member with a one-byte record: 1
   taken, 0 not. From then on each side's records go to the other.

A stream that is not answered within 10 seconds is refused. A member may have 32 streams open or
waiting; its streams close when it leaves the room.

**On the wire.** Each connection opens with 12 bytes in the clear: magic `A1 B0`, version 1, the
role (1 dial, 2 answer, 3 refuse), the member's connection ID (4) and the stream ID (4). Then come
records: a 2-byte length and the bytes sealed with ChaCha20-Poly1305 (the 16-byte tag at the end),
with no additional data. Each end of a stream has its own key: the first 32 bytes of
HMAC-SHA512, keyed with the member's key, of `alboria-stream-v1`, the stream ID (4) and the role (1
dials, 2 answers or refuses). The nonce is as in section 5.2, with a counter per direction starting
at 0: direction 0 for records to the server, 1 for records from it. The server opens what one end
sends and seals it again for the other, as the relay does with `DATA`. A record carries up to 16 KB.

**In the runtime.** A TCP socket of the game that connects to another member's virtual address is
connected instead to a loopback port where a thread of the runtime takes it and dials the stream.
With Xbox Live parties on Alboria, the game's TCP sockets are bound on the loopback interface, and a
listening one is registered by the port the game asked for: streams to that port are answered by
connecting to it, and the game's `accept` reports the member's virtual address and port.

## 6. The runtime side

The launcher writes `network_mode` from the player's account choice: `online` with an Alboria
account and `system_link` with a local one (Local and System Link only, no Alboria). With `online`,
the game signs in with the same key the launcher linked. A key no account has linked gets
`no_account`: hosting or joining a room fails, and the online overlay says to sign in from the
launcher.

With `network_mode = "online"`, the runtime connects to the relay once it has a ticket, and while
it is in a room the game sees:

- `XNetGetEthernetLinkStatus` reports a link, so System Link is offered as on a local network.
- Its XNADDR has its virtual address and the machine ID of `identity.toml`.
- Its datagram sockets (UDP and VDP) send through the relay: `sendto` to a member's virtual address
  or to a broadcast becomes `DATA`.
- What arrives from the relay reaches the game's socket through a socket on the loopback interface
  for each remote member, so `select`, blocking reads and `FIONREAD` keep working as with any other
  socket; `recvfrom` gives the game the member's virtual address and port as the sender.

The game's TCP connections to other members go through the server's stream port (section 5.5).

### 6.1 Xbox Live parties (work in progress, `alboria_live`)

- **Sign-in.** Player 1 is signed in to Live (`XamUserGetSigninState` 2, the Live enabled flag in
  `XamUserGetSigninInfo`, an online XUID with the top bits `0x0009` and the offline XUID's low 48
  bits when the title asks for it) and every privilege `XamUserCheckPrivilege` is asked for is
  granted. The game then offers and creates Joinable and Invite only parties by itself, and private
  Horde and Versus matches start (**verified** on 29 October 2026, one player).
- **Hosting.** When the title creates a hosted Live session that uses the peer network
  (`XSessionCreate` with the host, peer network and Live service flags), the runtime hosts a room
  (Joinable when the session has public slots, Invite only when it only has private ones), fills
  the session's XSESSION_INFO (an online peer XNKID, a key, and an XNADDR with the virtual address
  10.77.0.1) and publishes it (section 4.5). The session's traffic goes through the room as System
  Link's does.
- **Joining by code.** The ticket brings the host's session. The runtime broadcasts
  `XN_LIVE_INVITE_ACCEPTED` (`0x02000002`, user 0), and answers the title's
  `XInviteGetAcceptedInfo` (XLIVEBASE message `0x58023`) with an XINVITE_INFO: the invitee's online
  XUID, the title ID, the host's XSESSION_INFO and "from a game invite". The game then joins the
  host's party as after accepting an invite on the console. The message's second argument is its
  parameter list (16 bytes per parameter); the XINVITE_INFO goes where the second one points
  (**verified** in Gears of War 3's code at 0x82B0B5A8).
- **Finding the host's party.** The title then looks the session up with `XSessionSearchByID`,
  answered with the host's XSESSION_INFO, and probes the host with `XNetQosLookup`. The game reads
  the party's settings from the probe's data: the host's `XNetQosListen` data (flags `0x05`, enable
  and set data), which the runtime publishes with the room (section 4.5) and hands back to members in
  `XNetQosLookup`. Without it the game drops the session (**verified** at 0x8297EC18).
- **Joining.** The member asks the host's party beacon for a place over TCP (section 5.5), then
  travels to the host's session. Verified on 29 October 2026 with two installations on one
  computer, one with RAAM's Shadow and one without (section 6.2).

### 6.2 Players with different content (DLC)

Decided on 29 October 2026.

- **Players with and without a DLC play together, with the content they share.** A player with
  RAAM's Shadow and one without it can be in the same party and match; what only the DLC has (its
  maps, modes, characters) is not offered while someone in the party lacks it.
- **Why it matters on the wire.** RAAM's Shadow's update data replaces `GearGame.xxx` with a version
  that keeps the base game's net objects at the same indices (**verified**: the 86564 objects both
  versions load sit at the same index) and adds 74 at the end, while keeping the package's GUID and
  its single generation (89730 net objects without the update, 89804 with it). Unreal Engine 3
  numbers every package's objects after the previous packages' counts, so with different counts
  every package after `GearGame` is shifted: a reference one side sends resolves to the wrong object
  or to None on the other (**verified**: a member without the DLC joining a host with it got None as
  its menu HUD, the reference being 74 past the class).
- **How the runtime makes it work.** The engine already negotiates package versions through
  generations: `UPackageMap::Compute` gives each package the net object count of the older of the
  two sides' generations. At start-up the runtime gives the updated `GearGame` the base game's count
  as a first generation and its own as a second, so both sides of a mixed connection count the base
  game's objects, and the update's own ones are not sent; two players with the update count all of
  them. The table of known content updates is `kContentUpdates` in `runtime/src/script_hooks.cpp`.
- **Matchmaking (not built yet).** When a player with a DLC looks for a game, empty or filling
  lobbies of players without it come first, so that rooms fill up; lobbies where everyone has the
  DLC come after. The room will carry each member's content so that the service and the game know
  what the party shares.

## 7. Limits

- Up to 10 members per room.
- Per member: 1000 packets and 256 KB per second; above that, packets are dropped.
- Per IP address: 10 sign-ins and 5 new rooms per minute, and 10 codes an hour.
- Per email: one code a minute and 5 an hour (section 3.1).
- Rooms close after 2 minutes with no traffic, and a code stops working when its room closes.

## 8. What the server keeps

- Accounts: email, gamertag and tag, until the player deletes the account. The email is used only to
  send the codes.
- Devices of each account: public key, game (title ID), system (such as "Mac"), the hash of the
  computer's ID (section 3.2), the address it last signed in from and the dates it was linked and
  last signed in, until removed from the account. The address is kept only to ban it with its
  account.
- Bans: account, public key, computer or IP address, reason, date and end date (30 days for the
  addresses of a banned account; none for the rest), until removed or ended.
- Codes: in memory only, as a hash, for their 10 minutes.
- For each room, for 7 days: its code, title ID, times, and the public key and `Gamertag#tag` of
  each member, so that a report about a player can be checked. No IP addresses and no game traffic.
- Counts of use: rooms per day, with no other data, and the device keys seen each month, to count
  the different players, kept 13 months.

## 9. Versions

The relay's version byte and the API's `/v1/` change only when a change breaks older clients. The
server answers `426 client_too_old` to a `client` it no longer supports, and the runtime then asks
the player to update.
