/*
 * The friends list on the Play page (docs/alboria-relay.md, section 4.7), with an Alboria account:
 * friends with what each is doing, requests both ways, invitations to a friend's room, adding a
 * friend by Gamertag#tag, and for each friend Join (their open room), Invite (while the game runs),
 * favourite, remove, block and report.
 *
 * Joining is the game's own: the launcher asks Alboria for the room's code and starts the game with
 * it, or hands it to the running game, which joins once it is at its main menu. The launcher takes
 * no place in the room. Invitations are sent from here only while the game runs.
 *
 * The service's events arrive through the events channel (alboria.rs). They only run while the
 * launcher is in front, as its animations and controller input do: meanwhile they wait, and when it
 * comes back the lists are read again and only what still holds is shown (friends.js).
 */

import { canJoin, createEventQueue, friendActivity, onlineCount, requestName, sortFriends, stillValid } from "./friends.js";
import {
  alboriaFriends,
  joinRoom,
  onAlboriaEvent,
  onAlboriaEventsState,
  startAlboriaEvents,
  stopAlboriaEvents,
} from "./platform.js";

/** The reasons a report may give (alboria-server REPORT_REASONS). */
const REPORT_REASONS = ["cheating", "harassment", "gamertag", "impersonation", "other"];

/**
 * Wires the friends panel. `inFront` says whether the launcher is in front; `serviceBlock` gives
 * what keeps the player out of rooms (serviceView's block); `startGame(code)` starts the game into
 * a room; `formatWhen(ms)` words a past moment.
 */
export const setupFriends = ({ state, t, toast, inFront, serviceBlock, startGame, formatWhen }) => {
  const $ = (id) => document.getElementById(id);
  const panel = $("friends-panel");
  let lists = null;
  let listening = false;
  let menuFor = null;
  let reportFor = null;
  const queue = createEventQueue(inFront);

  const gameName = () => t("alboriaService.gameName");
  const errorText = (error) => {
    if (error?.error === "list_full") return t("friends.listFull");
    if (error?.error === "invite_only" || error?.error === "room_not_found") return t("friends.roomGone");
    if (error?.error === "not_in_room") return t("friends.notInRoom");
    return t("friends.failed");
  };

  /** Reads the lists again and draws them. */
  const load = async () => {
    try {
      lists = await alboriaFriends("/v1/friends");
    } catch (error) {
      console.info("The friends list could not be read:", error?.error ?? error);
    }
    render();
  };

  /** Sends a friends' request; on success reads the lists again, and says `done` when given. */
  const act = async (path, body, done = null) => {
    try {
      const answer = await alboriaFriends(path, body);
      if (done) toast(typeof done === "function" ? done(answer) : done);
      await load();
      return answer;
    } catch (error) {
      toast(errorText(error));
      return null;
    }
  };

  /** Joins the room whose code `path` gives (a friend's, or an invitation's). */
  const join = async (path, body, name) => {
    let room;
    try {
      room = await alboriaFriends(path, body);
    } catch (error) {
      toast(errorText(error));
      await load();
      return;
    }
    toast(t("friends.joining", { name }));
    try {
      if (state.game === "idle") await startGame(room.code);
      else await joinRoom(room.code);
    } catch (error) {
      toast(t("friends.joinFailed", { detail: error?.detail ?? error?.kind ?? String(error) }));
    }
    await load();
  };

  // --- Drawing -----------------------------------------------------------------------------

  const button = (text, className, onClick) => {
    const element = document.createElement("button");
    element.type = "button";
    element.className = className;
    element.textContent = text;
    element.addEventListener("click", onClick);
    return element;
  };

  const personRow = (name, detail, ...actions) => {
    const row = document.createElement("div");
    row.className = "friend-row";
    const text = document.createElement("div");
    text.className = "friend-text";
    const strong = document.createElement("strong");
    strong.textContent = name;
    const small = document.createElement("small");
    small.textContent = detail;
    text.append(strong, small);
    row.append(text, ...actions);
    return row;
  };

  const friendRow = (friend) => {
    const [key, vars] = friendActivity(friend);
    const detail =
      key === "lastSeen" ? t("friends.lastSeen", { when: formatWhen(vars.at) }) : t(`friends.${key}`, { game: gameName() });
    const actions = [];
    if (canJoin(friend, serviceBlock())) {
      actions.push(button(t("friends.join"), "btn btn-small btn-primary", () => join("/v1/friends/room", { id: friend.id }, friend.name)));
    } else if (friend.presence?.status === "playing") {
      const pill = document.createElement("span");
      pill.className = "friend-pill";
      pill.textContent = t("friends.inviteOnly");
      actions.push(pill);
    }
    // Inviting from the launcher only while the game runs (the game is then in a room).
    if (state.game === "running" && friend.presence?.status !== "offline") {
      actions.push(
        button(t("friends.invite"), "btn btn-small", () => act("/v1/invitations", { friend: friend.id }, t("friends.invited", { name: friend.name }))),
      );
    }
    actions.push(
      button("⋯", "btn btn-small friend-more", () => {
        menuFor = menuFor === friend.id ? null : friend.id;
        reportFor = null;
        render();
      }),
    );
    const row = personRow(`${friend.favourite ? "★ " : ""}${friend.name}`, detail, ...actions);
    row.dataset.state = friend.presence?.status ?? "offline";
    const wrap = document.createElement("div");
    wrap.className = "friend";
    wrap.append(row);
    if (menuFor === friend.id) wrap.append(friendMenu(friend));
    if (reportFor === friend.id) wrap.append(reportForm(friend));
    return wrap;
  };

  const friendMenu = (friend) => {
    const menu = document.createElement("div");
    menu.className = "friend-menu";
    menu.append(
      button(friend.favourite ? t("friends.unfavourite") : t("friends.favourite"), "btn btn-small", () => {
        menuFor = null;
        act("/v1/friends/favourite", { id: friend.id, favourite: !friend.favourite });
      }),
      button(t("friends.remove"), "btn btn-small", () => {
        menuFor = null;
        act("/v1/friends/remove", { id: friend.id }, t("friends.removed", { name: friend.name }));
      }),
      button(t("friends.block"), "btn btn-small btn-danger", () => {
        menuFor = null;
        act("/v1/friends/block", { id: friend.id }, t("friends.blocked", { name: friend.name }));
      }),
      button(t("friends.report"), "btn btn-small btn-danger", () => {
        menuFor = null;
        reportFor = friend.id;
        render();
      }),
    );
    return menu;
  };

  const reportForm = (friend) => {
    const form = document.createElement("div");
    form.className = "friend-report";
    const reason = document.createElement("select");
    for (const value of REPORT_REASONS) {
      const option = document.createElement("option");
      option.value = value;
      option.textContent = t(`friends.reason.${value}`);
      reason.append(option);
    }
    const text = document.createElement("textarea");
    text.className = "path";
    text.maxLength = 1000;
    text.placeholder = t("friends.reportPlaceholder");
    form.append(
      reason,
      text,
      button(t("friends.reportSend"), "btn btn-small btn-danger", async () => {
        if (!text.value.trim()) {
          text.focus();
          return;
        }
        reportFor = null;
        await act("/v1/reports", { gamertag: friend.name, reason: reason.value, text: text.value.trim() }, t("friends.reported"));
      }),
      button(t("friends.cancel"), "btn btn-small", () => {
        reportFor = null;
        render();
      }),
    );
    return form;
  };

  const section = (title, rows) => {
    if (rows.length === 0) return [];
    const head = document.createElement("p");
    head.className = "friends-section";
    head.textContent = title;
    return [head, ...rows];
  };

  const render = () => {
    const shown = state.account === "alboria" && lists !== null;
    panel.hidden = !shown;
    if (!shown) return;
    $("friends-count").textContent = t("friends.onlineCount", { count: onlineCount(lists.friends) });

    const invitations = lists.invitations.map((invitation) =>
      personRow(
        t("friends.invitation", { name: invitation.from.name }),
        gameName(),
        button(t("friends.join"), "btn btn-small btn-primary", () =>
          join("/v1/invitations/room", { id: invitation.id }, invitation.from.name),
        ),
        button(t("friends.notNow"), "btn btn-small", () => act("/v1/invitations/decline", { id: invitation.id })),
      ),
    );
    const incoming = lists.incoming.map((request) =>
      personRow(
        request.name,
        t("friends.wantsToBeFriend"),
        button(t("friends.accept"), "btn btn-small btn-primary", () =>
          act("/v1/friends/accept", { id: request.id }, t("friends.nowFriends", { name: request.name })),
        ),
        button(t("friends.decline"), "btn btn-small", () => act("/v1/friends/decline", { id: request.id })),
      ),
    );
    const outgoing = lists.outgoing.map((request) =>
      personRow(request.name, t("friends.pending"), button(t("friends.cancel"), "btn btn-small", () => act("/v1/friends/cancel", { id: request.id }))),
    );
    const friends = sortFriends(lists.friends).map(friendRow);
    const empty = document.createElement("p");
    empty.className = "help";
    empty.textContent = t("friends.empty");
    $("friends-body").replaceChildren(
      ...section(t("friends.invitations"), invitations),
      ...section(t("friends.requests"), incoming),
      ...section(t("friends.sent"), outgoing),
      // The friends get their own heading when other lists come before them.
      ...(friends.length
        ? invitations.length + incoming.length + outgoing.length
          ? section(t("friends.yourFriends"), friends)
          : friends
        : [empty]),
    );
  };

  // --- Events ------------------------------------------------------------------------------

  /** What an event does when it runs: presence changes the friend at once, the rest reads again. */
  const run = (event) => {
    if (event.event === "presence" && lists) {
      const friend = lists.friends.find((entry) => entry.id === event.data?.id);
      if (friend) {
        friend.presence = event.data.presence;
        render();
        return;
      }
    }
    if (event.event === "invitation_gone" && lists) {
      lists.invitations = lists.invitations.filter((entry) => entry.id !== event.data?.id);
      render();
      return;
    }
    if (event.event === "friend_request") toast(t("friends.requestToast", { name: event.data?.name ?? "" }));
    if (event.event === "invitation") toast(t("friends.invitation", { name: event.data?.from?.name ?? "" }));
    load();
  };

  /** Once the launcher is in front again: reads the lists, then shows what waited and still holds. */
  const flush = async () => {
    const waited = queue.take();
    if (waited.length === 0) return;
    await load();
    for (const event of waited) {
      if (!lists || !stillValid(event, lists)) continue;
      if (event.event === "friend_request") toast(t("friends.requestToast", { name: event.data?.name ?? "" }));
      if (event.event === "invitation") toast(t("friends.invitation", { name: event.data?.from?.name ?? "" }));
    }
  };

  onAlboriaEvent((event) => queue.offer(event, run));
  // A channel that opens again may have missed events: the lists are read again.
  onAlboriaEventsState(({ connected }) => {
    if (connected && listening) load();
  });
  window.addEventListener("focus", () => flush());
  document.addEventListener("visibilitychange", () => flush());

  $("friends-add").addEventListener("submit", (event) => {
    event.preventDefault();
    const field = $("friends-add-name");
    const name = requestName(field.value);
    if (!name) {
      toast(t("friends.badName"));
      return;
    }
    field.value = "";
    act("/v1/friends/request", { name }, (answer) =>
      answer?.result === "friends" ? t("friends.nowFriends", { name }) : t("friends.requestSent", { name }),
    );
  });

  return {
    /** Follows the account: listens and shows the list with an Alboria account, stops otherwise. */
    sync() {
      const alboria = state.account === "alboria" && state.alboria?.status === "connected";
      if (alboria && !listening) {
        listening = true;
        startAlboriaEvents().catch((error) => console.warn("Alboria's events channel did not start:", error));
        load();
      } else if (!alboria && listening && state.account !== "alboria") {
        listening = false;
        lists = null;
        stopAlboriaEvents().catch(() => {});
      }
      render();
      // The game ended or the window came back: what waited runs now.
      if (inFront()) flush();
    },
  };
};
