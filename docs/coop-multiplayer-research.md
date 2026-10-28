# Co-op and multiplayer: research and options

How Azura could let players play Gears of War 3's campaign co-op, Horde, Beast and Versus with other
people, what the game needs for that, how other projects solved it, and the options ordered from
best to worst. Trello card 34. Written on 2026-10-27.

Every statement is marked as **verified** (read in our code, the SDK, the game's imports or another
project's code), **from a source** (a web page, named in the Sources at the end, not checked by us),
or **inferred** (our own reasoning, not checked).

## Contents

1. [Summary](#1-summary)
2. [What the game needs](#2-what-the-game-needs)
3. [What the runtime does today](#3-what-the-runtime-does-today)
4. [How other projects did it](#4-how-other-projects-did-it)
5. [Options, from best to worst](#5-options-from-best-to-worst)
6. [Player identity](#6-player-identity)
7. [Legal and security](#7-legal-and-security)
8. [Peer to peer with a session directory](#8-peer-to-peer-with-a-session-directory)
9. [Local co-op on one computer](#9-local-co-op-on-one-computer)
10. [Latency overlay](#10-latency-overlay)
11. [Recommendation, first milestone and cards](#11-recommendation-first-milestone-and-cards)
12. [Sources](#12-sources)

## 1. Summary

Gears of War 3 has two ways to play with other consoles: **System Link**, the LAN mode, and **Xbox
Live**. System Link covers every mode, campaign co-op included (from a source), and needs no online
service: the consoles find each other with broadcasts on the local network and talk directly. Xbox
Live adds matchmaking playlists, invitations, friends, presence, leaderboards and the ranked modes,
all of which need a service that no longer exists for us.

Two projects have already made Xbox 360 games playable online without Microsoft's service, in the
two ways the options below build on:

- **Xenia Canary Netplay** (an Xenia fork, BSD-3): emulates the Xbox Live session services against a
  web service of its own (Xenia WebServices, MIT), and the players connect directly to each other,
  with UPnP to open the ports. It lists Gears of War 3 as working (from a source, which does not say in
  which mode).
- **GoldenEye Recomp** (built on the ReXGlue SDK, like Azura): uses the game's System Link and adds a
  small server, which anyone can run, that lists the matches and relays the LAN packets between the
  players, so no player has to open ports (from its README).

**Recommendation:** start with System Link. First on a real LAN, then over the internet with a
relay of our own in the GoldenEye way (session codes, no accounts), and leave Live emulation
(public Versus matchmaking and playlists) for a separate project with its own legal review. The
first milestone is two computers playing a campaign chapter in co-op over System Link on the same
network. Before any of it, each installation needs a player identity (XUID) of its own: today every
Azura player has the same one.

## 2. What the game needs

### 2.1 What it imports (verified)

From the game's imports in the generated code (`runtime/generated/default`):

- **Sockets**, the Xbox's own Winsock: `NetDll_socket`, `bind`, `connect`, `listen`, `accept`,
  `send`, `recv`, `sendto`, `recvfrom`, `select`, `ioctlsocket`, `setsockopt`, `getsockopt`,
  `getsockname`, `inet_addr`, `WSAStartup`, `WSACleanup`, `WSACreateEvent`, `WSAGetLastError`.
- **XNet**, the secure network layer: `XNetStartup`, `XNetCleanup`, `XNetGetTitleXnAddr`,
  `XNetXnAddrToInAddr`, `XNetInAddrToXnAddr`, `XNetRegisterKey`, `XNetUnregisterKey`,
  `XNetUnregisterInAddr`, `XNetConnect`, `XNetServerToInAddr`, `XNetSetSystemLinkPort`,
  `XNetGetEthernetLinkStatus`, `XNetRandom`, `XNetDnsLookup`, `XNetDnsRelease`, and the quality of
  service functions `XNetQosListen`, `XNetQosLookup`, `XNetQosRelease`, `XNetQosGetListenStats`.
- **Live sessions**, through XAM messages: `XamSessionCreateHandle`, `XamSessionRefObjByHandle`,
  `XMsgStartIORequest`, `XMsgStartIORequestEx`, `XMsgInProcessCall`. The `XSession*` functions of the
  Xbox SDK (create, join, start, end, search, arbitration, statistics) are a static library in the
  game that sends these messages to XAM's "XGI" application (`xgi_app.cpp` in the SDK).
- **Users and Live features**: `XamUserGetSigninState`, `XamUserGetSigninInfo`, `XamUserGetXUID`,
  `XamUserGetName`, `XamUserCheckPrivilege`, `XamUserAreUsersFriends`,
  `XamUserCreateStatsEnumerator`, `XamUserReadProfileSettings`, `XNetLogonGetTitleID`, the
  `XamShow*UI` system screens (sign-in, friends, players, game invite, gamer card, messages), and
  voice chat (`XamVoiceCreate`, `XamVoiceSubmitPacket`, `XamVoiceHeadsetPresent`).

### 2.2 How a session works on the console

This is how the Xbox 360 SDK's networking works in general (from Microsoft's documentation of the
time and the Xenia sources; the exact calls Gears 3 makes in each menu have not been logged yet):

1. The host creates a session (`XSessionCreate`). For System Link, the console itself fills the
   session's `XSESSION_INFO`: the host's address (`XNADDR`), the session's ID (`XNKID`) and a key
   exchange key.
2. On System Link the host answers broadcasts on the local network with that information; on Live,
   the session is registered with the Live service and found with `XSessionSearch`.
3. A client registers the session's key (`XNetRegisterKey`), turns the host's `XNADDR` into an
   address it can send to (`XNetXnAddrToInAddr`), and joins (`XSessionJoinRemote`).
4. Game traffic goes over UDP in "VDP" sockets (a protocol of the Xbox: data with a small header, part
   of which can be voice). On the console all of it is encrypted and authenticated by XNet.
5. Before joining, quality of service (`XNetQosLookup`) measures the latency to the possible hosts,
   which the game uses to choose or show them.

Gears 3 is an Unreal Engine 3 game: the engine's online subsystem for Live drives these calls, and
the game's own network driver sends the gameplay traffic. That the travel URL of a co-op host
carries `?listen` (verified in our travel hook) is how Unreal makes the host's game accept clients.

### 2.2.1 How Gears 3 starts a System Link party (verified)

Found on 2026-10-27 by running the game on macOS and disassembling its scripts (`GearGame`,
`OnlineSubsystemLive`):

- **The party options open with Back** (Tab on the keyboard, Create on the DualSense) from the main
  menu's lobby (`UIScene_FrontEnd.CanOpenPartyOptions`, with no Live condition). "Create public
  party" asks for Xbox Live Gold; System Link is a party type in these options, not a menu of its own.
- **The party types** (`EGearVersusPartyType`): InviteRequired 0, FriendsOnly 1, Public 2,
  SystemLink 3, Local 4. The chosen one is profile setting 102; `GearProfileSettings.SetPartyType`
  sets `bPartyTypeIsSystemLink` when it is 3.
- **System Link is offered only with a network link.** `BuildPartyOptions` enables it when every
  player is signed in, the player is not a guest and `UIInteraction.HasLinkConnection()` is true.
  `HasLinkConnection` is native: it calls `XNetGetEthernetLinkStatus` and tests bit 0. The runtime's
  stub returns 0, so the option stayed greyed out until the stub was changed for the experiment.
- **The party session** is created by `UIScene_FrontEnd.ConditionalCreatePartySession` when the
  player can play online or `bPartyTypeIsSystemLink` is set, the party type is not Local, the player
  is not a guest and there is a link. With the link reported, the game created the System Link party
  (`XSessionCreate` through the XGI app) and showed its lobby; nothing answered for a client, because
  the session messages are only logged (section 3).
- **What a client does** has not been observed: it needs two computers.

### 2.3 What depends on Xbox Live

| Feature | System Link | Xbox Live | Notes |
| --- | --- | --- | --- |
| Campaign co-op, 2 to 4 players | yes (from a source) | yes | |
| Horde and Beast | yes (from a source) | yes | Player count over System Link not confirmed |
| Versus, private matches | yes (from a source) | yes | |
| Versus matchmaking, playlists (Standard, Casual) | no | yes | Inferred: the playlists come from Live's title storage, which is Epic's data |
| Ranked play, arbitration, leaderboards | no | yes | |
| Invitations, friends, party, presence | no | yes | |
| Achievements | local | local | The console unlocks them; the runtime already does it locally |
| Voice chat | yes | yes | Voice goes in the VDP packets (inferred for Gears 3) |

Card 74 (achievements that need co-op or Versus) depends on this: the achievements that ask for
Standard or Casual playlists need Live emulation and our own playlists (section 5.4), and
"Lambentismo" needs a way to recognise Azura developers (section 6).

## 3. What the runtime does today

Verified in the SDK (`src/kernel/xam/xam_net.cpp`, `apps/xgi_app.cpp`, `src/system/xam/user_profile.cpp`):

- **Sockets work.** `socket`, `bind`, `sendto`, `recvfrom` and the rest go to the host's sockets
  (`XSocket`), with VDP sockets handled (the game data length is the first buffer).
- **XNet is a stub.** `XNetGetTitleXnAddr` returns the loopback address with a made-up network card
  address, `XNetXnAddrToInAddr` and `XNetInAddrToXnAddr` fail, `XNetRegisterKey`, `XNetConnect`,
  `XNetQosLookup`, `getsockopt` and `getsockname` are stubs, and `XNetQosListen` fails.
- **Sessions are only logged.** Every `XSession*` message returns success without doing anything:
  `XSessionCreate` does not fill `XSESSION_INFO`, and a search finds nothing. So not even System
  Link can work today.
- **Each installation has an identity of its own** (card 111, since 2026-10-28): a random offline
  XUID for player 1 (`0xE000` followed by 48 random bits, players 2 to 4 the next three), a random
  machine ID for the `XNADDR` and a key seed, kept in `identity.toml` in the saves folder. The saves
  stay in the folder named after the old shared XUID, `0xB13EBABEBABEBABE`
  (`ContentManager::ContentFolderXuid`).
- **The travel hook strips `?listen`** from campaign URLs (`PatchTravelURL` in
  `runtime/src/script_hooks.cpp`), because keeping it hung the game after loading a chapter on macOS
  (2026-10-21). A co-op host needs it. Why it hung is not known; inferred: the game waits for a network
  or session operation that the stubs never complete.

## 4. How other projects did it

### 4.1 Xenia Canary Netplay

An Xenia fork by Adrian Cassar (repository `AdrianCassar/xenia-canary`, default branch
`netplay_canary_experimental`, BSD-3 like Xenia: verified, its LICENSE file). Our SDK comes from the
same Xenia code, so its pieces map onto our files almost one to one.

What it does (verified, its code):

- **Live emulation against a web service.** `XLiveAPI` talks over HTTP to "Xenia WebServices"
  (`AdrianCassar/Xenia-WebServices`, TypeScript, MIT: verified). The service keeps players
  (`players`, `whoami`), sessions per title (`title/{id}/sessions`, `search`, `details`, `modify`,
  `qos`, `migrate`, `arbitration`, `leaderboards`) and the ports each title uses
  (`title/{id}/ports`). A public instance runs on Heroku with a session browser (from a source).
- **XNet without its security.** `XNetGetTitleXnAddr` returns the player's public IP address, given
  by the service. `XNetXnAddrToInAddr` returns that address as it is (or the LAN address in System Link
  mode, or loopback for the player's own). Traffic is plain UDP: XNet's encryption and authentication
  are not reproduced.
- **Direct connections, no relay.** Players connect to each other's public address. UPnP
  (miniupnp) opens the ports on routers that allow it; without it the NAT is "strict" and others
  cannot join that player (verified, `XLiveAPI.cpp`). Its release notes list UPnP, host migration and
  "port chaining from backend", and nothing about a relay or NAT traversal (from a source).
- **Three modes** (`network_mode`): offline, System Link and Xbox Live. System Link does not use the
  service (from a source, release 4.0.0).
- **What the service stores about a player** (verified, `XLiveAPI.cpp`): XUID, gamertag, a machine
  ID, the public IP address and the network card's MAC address.
- **Gears of War 3** is listed among the games that work, "improved stability" in release 6.0.0 (from
  a source), and its code has comments specific to Gears 3's title ID `4D5308AB` (rich presence longer
  than 64 characters, a session property it expects): verified, `xnet.h` and `XLiveAPI.cpp`.

Reusable: the XNet, `XSession` and QoS implementation, as a guide or as code (BSD-3, keeping the
notice). Not reusable as it is: its security model (plain UDP to public addresses, no
authentication), and the dependency on someone else's public service.

### 4.2 GoldenEye Recomp

A ReXGlue port of GoldenEye 007's Xbox 360 version by SunJaycy, with online play
(`SunJaycy/GoldenEye-Recomp`, `GoldenEye-Recomp-rexglue` and `GoldenEye-Recomp-Server`). The server
is public domain (Unlicense: verified).

How it works (from its README; not run by us):

- The game already speaks System Link. A small server does two jobs: a **matchmaker** (hosts register
  their match, players ask for the list) and a **UDP relay** that forwards the game's LAN broadcast and
  unicast packets between players, with a virtual `10.0.0.x` address for each.
- Every player connects out to the server, so **no player opens ports**; only the server's one UDP
  port has to be reachable. Anyone can run it (on the same PC, with Hamachi or playit.gg, or on a
  free VPS), and the players type its address in the game's ONLINE menu.
- The server forwards the game data without reading it.

Reusable: the design, almost as it is, for Gears 3's System Link. It needs no Live emulation and no
accounts.

### 4.3 Virtual LAN tools

XLink Kai tunnels System Link traffic between players over the internet and supports the Xenia
netplay fork (from a source). Hamachi, ZeroTier or Tailscale do the same for any LAN game. With
System Link working, these let friends play over the internet with no code and no server of ours.

### 4.4 Projects named in the card

- No Xbox 360 recompilation called "Crash Racing" was found, and "BBPort" is a PS4 port of
  Bloodborne (card 34's notes). Neither is a reference for Xbox 360 networking.
- Other ReXGlue ports (Forza Horizon, Forza Motorsport 4, Dante's Inferno, Burnout Revenge, Banjo
  Nuts and Bolts, Skate 3 and more) were found on GitHub; none of their descriptions mention online
  play. Not checked further.

### 4.5 Steam networking

Steam's peer to peer networking (Steam Datagram Relay) is free, traverses NAT and hides the players'
IP addresses (from a source), but it needs a Steam App ID: an application published on Steam. Azura
cannot get one for a game it has no rights to, and using Valve's test application (480, Spacewar)
for a public build is described as not allowed (from a source). It also needs every player to run
Steam. **Not viable.** Valve's open source GameNetworkingSockets library (BSD-3) gives the same
encrypted, reliable UDP transport without Steam; for peer to peer it needs our own signaling and
STUN/TURN servers (inferred from its documentation, not checked in its code).

Epic Online Services also offers free lobbies, peer to peer and relays. It needs a product
registered with Epic, for a game whose rights are Microsoft's: not checked, and it would need the
legal review of section 7.

## 5. Options, from best to worst

Effort is in Fibonacci points as on the board, as an estimate.

### 5.1 System Link on a LAN (the base of everything)

Implement in the runtime what System Link needs: a unique XUID per installation, `XNADDR` with the
LAN address and a stable machine identity, `XNetXnAddrToInAddr` and its reverse, key registration,
`XSessionCreate` filling `XSESSION_INFO`, broadcasts, `getsockname`/`getsockopt`, quality of service
on the LAN, and `?listen` kept when the player hosts.

- **Pros:** no server, no accounts, no personal data; every mode the game has on System Link; the
  later options build on it.
- **Cons:** on its own, only players on the same network.
- **Risks:** the `?listen` hang has to be understood; the game may expect XNet behaviour the stubs do
  not show yet (to be found with logs).
- **Effort:** 8 (runtime), plus testing on two computers.
- **Breaks:** nothing while nobody hosts; the XUID change has to keep the saves where they are.

### 5.2 System Link over the internet with a relay of our own (recommended next)

The GoldenEye design: a small Azura server lists the matches and relays the System Link packets. The
runtime sends its System Link traffic to the server instead of the LAN. Players join with a session
code or from a list.

- **Pros:** no player opens ports; IP addresses are not shown to other players; the server never runs
  the game and stays small; anyone can run their own (open source, like GoldenEye's); no Live
  emulation.
- **Cons:** the relay's bandwidth grows with the players (a few tens of kilobits per player, inferred
  for a game of this age); every packet goes through the server, which adds its latency.
- **Risks:** someone has to run the public instance (a free or cheap VPS); see section 7 for
  authentication.
- **Effort:** 8 (server and runtime transport), plus 3 for the launcher's settings.
- **Breaks:** nothing; System Link on a LAN keeps working.

### 5.3 Peer to peer with a session directory

The option from card 34's notes, evaluated in section 8. The same directory as 5.2, but the game
traffic goes directly between players, with STUN to open the NATs and a relay only when that fails.

- **Pros:** the least bandwidth on our side; the lowest latency between players.
- **Cons:** NAT traversal is the hard part; players see each other's IP; the host's advantage.
- **Effort:** 13 on top of 5.2 (split it).

### 5.4 Xbox Live emulation (Xenia netplay style)

Implement the Live session services against a web service, as Xenia netplay does (its code as a
guide or adapted, its service or ours).

- **Pros:** the game's own online menus, matchmaking and playlists, invitations, ranked play and
  leaderboards; what Versus achievements need.
- **Cons:** by far the largest work; the playlists and title files the game downloads from Live are
  Epic's data, which we cannot publish, so we would have to write our own; the security model of
  4.1 (plain UDP to public addresses) has to be replaced.
- **Risks:** legal (section 7); a public service with accounts.
- **Effort:** 13 several times over: a project of its own.

### 5.5 Only document a virtual LAN tool

Implement 5.1 and tell players to use Tailscale, ZeroTier, Hamachi or XLink Kai to play over the
internet.

- **Pros:** no server and almost no code beyond 5.1.
- **Cons:** each player installs and sets up another tool; we do not control the experience.
- Worth doing in any case as soon as 5.1 works, while 5.2 is built.

### 5.6 Steam peer to peer

Not viable (section 4.5).

## 6. Player identity

Whatever the option, each player needs an identity: a unique XUID for the game, a name, and maybe
something more. The options, from the lightest:

| | No accounts | Keys without a server | Our own accounts | An existing service (Discord, GitHub) |
| --- | --- | --- | --- | --- |
| How | A name chosen in the launcher, a random XUID per installation | Each launcher makes a key pair and signs what it sends; the developers' public keys ship in the kit | Sign-up and login on our server | OAuth sign-in on our server |
| Matchmaking and playlists | yes (no skill memory) | yes | yes | yes |
| Persistent levels and medals | local only | local only | yes | yes |
| Recognise Azura developers (Lambentismo) | no (anyone can claim it) | **yes** | yes | yes |
| Bans | no | by key (a new key escapes it) | yes | yes |
| Friends and invitations | session codes shared elsewhere | session codes, or keys as friend IDs | yes | yes (Discord friends) |
| Hosting | none | none | server, database, backups, uptime | server and database |
| Personal data stored | none (relay sees IPs in transit) | none | email, password hash, IPs | the provider's user ID and name |
| GDPR duties | almost none | almost none | privacy notice, account deletion, breach duties, password handling | privacy notice, account deletion |
| Can be added later | base | on top of "no accounts" | yes | yes |

Recommendation: no accounts plus keys without a server. Every installation gets a random XUID and a
key pair the first time; the name is the gamertag already chosen in the launcher. Signing with the
key lets the developers be recognised for Lambentismo with no server and no personal data. Accounts
can be added later without breaking this. The XUID should look like an offline one so the game does
not take it for a real Live account (inferred: the game may check the XUID's type).

## 7. Legal and security

- **No game data or keys published.** None of the options needs them: System Link and the relay
  carry what the players' own games send. Live emulation (5.4) is where it gets close: the playlists
  and title files of Live are Epic's, so ours would have to be our own.
- **Trademarks.** The service and the launcher must not present themselves as Xbox Live or as
  anything of Microsoft's or Epic's. Public matchmaking would need its own legal review (card 34's
  notes).
- **What other players can reach.** On the console, XNet encrypted and authenticated all traffic,
  and the hypervisor contained the game. In Azura, the game's 2011 network code runs natively on the
  player's computer, with the player's permissions, and parses packets from other players. Inferred:
  a bug in that code that a malicious player could trigger is a way into the player's computer, not
  only a crash. So:
  - every packet that reaches the game should come from a member of the session, checked by the
    runtime before the game sees it (a session key given by the host or the relay, and packets
    authenticated with it);
  - the first online options should be for friends (session codes), not public matchmaking;
  - with a relay, players do not see each other's IP addresses; with direct connections they do, and
    the launcher has to say so.
- **Personal data.** A relay sees IP addresses while it forwards; it should not log them beyond what
  operating it needs. Section 6 lists what each identity option stores.

## 8. Peer to peer with a session directory

The option discussed on 2026-10-24: a player looks for open sessions with a decent connection, and if
there are none becomes the host and announces one.

- **Directory cost:** small. It keeps a list of sessions with their address and properties, answers
  searches and expires sessions whose host stops refreshing them. A free or cheap VPS is enough for
  hundreds of players (inferred).
- **NAT traversal:** STUN lets two players behind ordinary NATs reach each other (the directory tells
  each one the other's public address and they send to each other at the same time). It fails behind
  symmetric or carrier-grade NAT, frequent on mobile and some fibre providers (inferred, common
  knowledge of the field); those players need a relay, which is option 5.2's server.
- **Host's advantage:** the host plays with no latency and runs the game's authority, so it can cheat.
  The same is true with a relay (the host is still a player); only dedicated servers avoid it, which
  this game was not made for.
- **Against the relay (5.2):** peer to peer saves bandwidth and some latency, but needs the relay
  anyway for the hard NATs, shows IP addresses and is harder to secure. With the player numbers a
  fan port has, the relay's bandwidth is cheap (inferred). So: relay first, peer to peer later as an
  optimisation that falls back to it.
- **Few players:** with few sessions the search rarely finds a good one. The search should widen its
  latency limit over time and fall back to hosting without the player noticing; session codes between
  friends avoid the problem.

## 9. Local co-op on one computer

Done in card 81 (PR #74, 25 October 2026): two players in split screen, each with their own device
and profile, from the game's Options, Local co-op. Verified in the card. What is left from card 34's
"Local players" notes:

- **Players 3 and 4:** the game allows two players per console in split screen, so they only matter
  online (two local players joining an online session). With 5.1, each local player needs their own
  XUID too: today they are player 1's plus 1 to 3, which would collide between installations.
- **Button prompts per player:** still player 1's for both halves. The options are in card 34's
  notes; the first (an atlas with every family and a hook on the function that turns a button into a
  font character) is still the best one (an assessment, not checked).

## 10. Latency overlay

A mode of the performance overlay (cards 23, 24 and 91), off by default.

- **What it shows:** the round trip to the host, and to each player when hosting, with jitter and
  packet loss over the last few seconds. The host shows "Host" instead of a number.
- **Where the numbers come from:** with System Link and the relay (5.1, 5.2), from our own transport:
  the runtime sends small ping packets of its own to each peer (or the relay answers them), outside the
  game's traffic. Reading the game's own number (Unreal keeps a ping per player) is possible through a
  script hook but measures something else (the game's tick), so ours is better for the overlay
  (inferred). Quality of service before joining (`XNetQosLookup`) can use the same measurement.
- **Host or client:** the runtime knows it from the session: `XSessionCreate` with the host flag, or a
  join. That is the state the overlay reads.
- **Settings:** an entry in the Video options and a key in azura.toml, as the other overlay modes.

## 11. Recommendation, first milestone and cards

**Recommendation:** System Link first (5.1), friends over the internet with a virtual LAN tool right
after (5.5), then an Azura relay with session codes (5.2), and peer to peer (5.3) or Live emulation
(5.4) only if there are players enough to justify them. Identity: no accounts plus keys (section 6).

**First milestone:** two computers on the same network play a campaign chapter in co-op over System
Link: one hosts, the other finds the session in the game's System Link menu and joins, both play, and
both can save. Then Horde with two players.

**Cards** (titles in the board's format; effort as an estimate):

1. *Runtime: a player identity of its own for each installation* (3): random offline-type XUID per
   installation for each local player, saves kept in their current folder, and a key pair for later.
2. *Runtime: System Link on a local network* (8): `XNADDR`, address conversions, keys, session info,
   broadcasts, quality of service, `getsockname`/`getsockopt`; `?listen` kept when hosting and the
   hang understood. Milestone above, on macOS and Linux.
3. *Docs: play System Link over the internet with a virtual LAN* (1): tested with one tool, written
   for players.
4. *Server: Azura relay with session codes* (8): matchmaker and UDP relay, open source, with session
   membership checked; and the runtime's transport to it.
5. *Launcher: online play settings* (3): relay address, session code, the privacy notes of section 7.
6. *Overlay: connection latency* (3): section 10.
7. Later, each its own project: peer to peer with a directory (5.3) and Live emulation for Versus
   playlists (5.4), the latter after a legal review; card 74's Lambentismo with the developer keys.

## 12. Sources

- Xenia Canary Netplay: https://github.com/AdrianCassar/xenia-canary and its releases,
  https://github.com/AdrianCassar/xenia-canary/releases (code read: `XLiveAPI.cpp`, `xam_net.cc`,
  `xnet.h`, `LICENSE`, branch `netplay_canary_experimental`, 2026-10-27).
- Xenia WebServices: https://github.com/AdrianCassar/Xenia-WebServices
- GoldenEye Recomp server: https://github.com/SunJaycy/GoldenEye-Recomp-Server (README),
  https://github.com/SunJaycy/GoldenEye-Recomp
- Coverage of GoldenEye Recomp: https://www.tomshardware.com/video-games/retro-gaming/cancelled-xbox-360-version-of-goldeneye-007-gets-recompiled-for-pc-no-emulator-the-game-runs-as-a-real-native-executable-insists-dev
- XLink Kai and Xenia: https://www.teamxlink.co.uk/wiki/Xenia_XLink_Kai_Setup
- Gears of War 3 co-op and System Link: https://www.co-optimus.com/article/6862/gears-of-war-3-co-op-faq.html,
  https://co-optimus.com/review/851/page/2/gears-of-war-3-co-op-review.html
- Steam networking: https://partner.steamgames.com/doc/features/multiplayer/networking; about App
  ID 480: https://github.com/hertzole/fizzysteamymirror
