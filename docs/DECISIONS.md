---
title: Decisions
description: All design decisions with verbatim rationale, alternatives considered, and traceability to goals/values.
when: Making design choices, checking why something was decided, adding new decisions
tags: [decisions, rationale, architecture]
---

# lnotify — Decisions

---

## Session: 2026-03-09 — Initial Design & Proof of Concept

**Context:** Greenfield project. No code existed. The user wanted to build a universal Linux toast notification system that works on virtual consoles, Wayland, X11 — anywhere. Session covered language choice, architecture, rendering backends, session detection, and fallback chain design. Built multiple Python proof-of-concept scripts to validate assumptions empirically.

**GVP source:** Inferred inline

### Inferred Goals/Values (refine later)

- **G1: Universal rendering** — notifications should appear on any Linux display environment (TTY, Wayland, X11, headless)
- **G2: Cross-machine portability** — single binary works on desktops and Raspberry Pis
- **V1: "Just Works"** — minimize configuration and dependencies; correct backend selection should be automatic
- **V2: Robustness over elegance** — prefer reliable delivery (even if queued) over perfect presentation
- **V3: Honest behavior** — never silently claim success; if a notification can't be shown, queue it transparently
- **P1: Design for extensibility, implement for now** — interfaces accommodate future features (stacking, priority) without implementing them in v1
- **P2: Empirical validation** — test assumptions with real code before committing to design decisions

---

### D1: Implementation language — C

> Use C for the implementation rather than Rust, Go, or other languages.

- **Chosen:** C
- **Rationale:** user said, "i don't know either super well. i'm perfectly happy with C"
- **Maps to:** G2 (cross-machine portability)
- **Tags:** language, build

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Rust | discussed | Safe framebuffer manipulation, good ecosystem, static binary | User was open to either; Claude recommended C for natural framebuffer affinity. User said "i was thinking rust, but i can be sold on C" |
| Go | discussed | Fast to develop, static binary | Framebuffer libs are sparse |
| C + shell wrapper | discussed | C daemon, shell CLI | Not discussed further |

---

### D2: Dynamic loading via dlopen for display backends

> Single binary that loads libwayland-client, libxcb, D-Bus libs at runtime if available.

- **Chosen:** `dlopen` at runtime
- **Rationale:** user said, "2 sounds good :)" (in response to the dlopen option)
- **Maps to:** G1, G2
- **Tags:** build, backends

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Compile-time optional linking | discussed | Exclude backends if lib not present at build time | Requires multiple binaries or build variants |
| Raw protocol (no libs) | discussed | Speak Wayland/X11 protocols directly over sockets | "Heroic but impractical" |

---

### D3: Three-component architecture (daemon + CLI + D-Bus listener)

> lnotifyd daemon, lnotify CLI client, optional D-Bus listener inside daemon.

- **Chosen:** daemon + CLI + optional D-Bus
- **Rationale:** TBD
- **Maps to:** G1, V1
- **Tags:** architecture

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| CLI-only (no daemon) | claude considered | Direct rendering from CLI tool | Can't monitor VT switches or maintain notification queue |
| D-Bus only | claude considered | Rely entirely on D-Bus | D-Bus won't exist on headless Pis |

---

### D4: IPC via Unix socket (not D-Bus for primary IPC)

> Daemon listens on a Unix socket. D-Bus is optional on top.

- **Chosen:** Unix socket + optional D-Bus
- **Rationale:** Claude recommended socket + CLI, user agreed. D-Bus is nice for desktop integration but adds a dependency that won't exist on headless Pis.
- **Maps to:** G1, G2
- **Tags:** IPC

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| D-Bus only | discussed | Use D-Bus as sole IPC | user said, "i think D-Bus would be nice" but agreed on socket as primary since D-Bus won't exist everywhere |
| Named pipe | claude considered | Simpler than socket | Less capable (no bidirectional, no concurrent clients) |

---

### D5: GUI notifications via D-Bus org.freedesktop.Notifications (not custom windows)

> For Wayland/X11 sessions, send notifications through the compositor's own D-Bus notification server rather than rendering custom windows.

- **Chosen:** D-Bus `org.freedesktop.Notifications`
- **Rationale:** user said, "tbh, #4 is what i was envisioning haha -- if we detect a GUI, hook into that GUI's nativate notification mechanism (if one exists), then fallback to framebuffer"
- **Maps to:** G1, V1, V2
- **Tags:** rendering, wayland, x11

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Custom GTK4 Wayland window | discussed, tested | Render our own window on the compositor | GDM's GNOME Shell suppresses foreign windows (PHANTOM rendering). Even our own session showed "python3 is ready" noise. Compositor has full authority to ignore us. |
| DRM overlay planes | discussed | Hardware planes above compositor | Hardware-dependent, not universal |
| Framebuffer cross-check | discussed, tested | Verify Wayland window visibility by reading fb0 | Compositors don't write to fb0 — cross-check always shows 0% pixel change |

---

### D6: Engine config with `conflicts_with_framebuffer` attribute

> Each rendering engine declares whether its compositor bypasses /dev/fb0, controlling whether framebuffer fallback is attempted.

- **Chosen:** Static boolean per engine type
- **Rationale:** user said, "only that perhaps our extensible engines can opt to skip the framebuffer fallback on a per-engine basis. and in this case, we know that if wayland is detected (i.e.: is the engine we're using), we should skip the framebuffer fallback. it's not immediately apparent to me that that should be true for all engines"
- **Maps to:** V3, P1
- **Tags:** rendering, framebuffer, extensibility

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Always skip fb on GUI sessions | discussed | Blanket skip | user said it's "not immediately apparent to me that that should be true for all engines" — links2, fbi, etc. use fb directly |
| Dynamic per-attempt flag | discussed | Return FAILED_SKIP_FB vs FAILED | User asked if it could be dynamic; concluded it's always static per compositor type |

---

### D7: Fallback chain — D-Bus → Framebuffer (with defense) → Queue

> Three-step fallback with retry, verification, and queuing.

- **Chosen:** D-Bus with retry/backoff → framebuffer with write-and-defend → queue
- **Rationale:** user said, "i love that! retry with backoff, after N attempts fallback to framebuffer, if that fails queue"
- **Maps to:** V2, V3
- **Tags:** rendering, reliability

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| No queue (drop if undeliverable) | claude considered | Simpler but lossy | user said "i'd prefer we got it to work everywhere always no matter what" and referenced an apocalypse scenario |
| GDM-specific skip | discussed | Hardcode skip for GDM greeter | user said, "i don't want to engineer a skip for this precise scenario" — wanted generic fallback |

---

### D8: Framebuffer read-after-write verification with sustained defense

> Verify toast visibility by reading back pixels, and re-render if clobbered.

- **Chosen:** Initial verify at +16ms/+50ms, sustained defense every 200ms for DURATION/2, background thread for remainder
- **Rationale:** user said, "can we have an immediate check (say, within ~200ms (or more realistically, `display duration / 2`) after a framebuffer write to see if our pixels are there" and "i would still stick with TOAST_DURATION / 2 (or if we want it configurable, TOAST_VALID_DIVISOR or similar)"
- **Maps to:** V2, V3
- **Tags:** framebuffer, verification

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Single verify at 50ms | discussed | Quick check, no sustained defense | GDM launches seconds after VT switch; 50ms check passes but toast gets clobbered later |
| Hardcoded verify window | discussed | Fixed timing | User preferred configurable divisor that scales with notification duration |

---

### D9: Per-user and system daemon modes

> Same binary supports both modes. Per-user is default, system mode adds cross-user coordination.

- **Chosen:** Both modes, same binary
- **Rationale:** user said, "can we structure so that either is permissible? a user-level or system-level daemon? that way we can support the cross-user alice/bob scenario as well as a simpler userland setup on a Pi?"
- **Maps to:** G1, G2
- **Tags:** architecture, daemon

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Per-user only | discussed | Simpler, one daemon per user | Doesn't handle cross-user scenarios |
| System only | discussed | One daemon, full visibility | Overkill for single-user Pi |

---

### D10: Root mode shows notifications everywhere

> When run as root, notifications appear on all sessions regardless of owner.

- **Chosen:** Root = show everywhere
- **Rationale:** user said, "as the admin, i 'own' all sessions. i simply sometimes use different user accounts to organize tools and permissions, but all notifications are for me"
- **Maps to:** G1
- **Tags:** daemon, permissions

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Root delegates to per-user daemons | discussed | System daemon routes to user daemons | Adds complexity; user wants direct rendering |

---

### D11: Configuration from the start (no magic numbers)

> Config struct populated at startup with defaults, used throughout. No hardcoded style values in rendering code.

- **Chosen:** Config from day one
- **Rationale:** user said, "wiring up configuration settings after the fact can be a pain if/when magic values end up interwoven in hard-to-see ways when we try to separate it out. configurability from the onset seems like less effort"
- **Maps to:** P1
- **Tags:** config, architecture

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Hardcode v1, add config later | discussed | Simpler initial implementation | user explicitly rejected this approach |

---

### D12: Key=value config format

> Simple key=value flat file, with path to INI sections if it outgrows it.

- **Chosen:** Key=value
- **Rationale:** user said, "i think 2 or 3. i don't imagine (perhaps wrongly, but this seems easier to correct later if wrong) we'll need complex configs"
- **Maps to:** V1
- **Tags:** config

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| TOML | discussed | Clean, nested sections | User felt it was more than needed; "easier to correct later if wrong" |
| INI | discussed | Sections but simple | Upgrade path from key=value if needed |

---

### D13: Toast style — rounded toast with configurable theming

> Android/macOS-style rounded rectangle toast with title + body text. Configurable position, colors, border, padding.

- **Chosen:** Rounded toast, configurable, default top-right
- **Rationale:** user said, "i agree on 2 + 3" (rounded toast as default + config layer)
- **Maps to:** V1
- **Tags:** rendering, style

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Minimal text bar | discussed | Single-line status bar flash | Less visually distinctive |
| Hardcoded style | discussed | No config, fixed appearance | User insisted on configurability from the start (see D11) |

---

### D14: VT switch detection via sysfs poll()

> Use poll() on /sys/class/tty/tty0/active as primary detection, with timed read fallback.

- **Chosen:** sysfs poll() primary, timed read fallback, logind for enrichment
- **Rationale:** Empirically tested 5 methods simultaneously. poll() fired immediately on every switch with no ownership requirements.
- **Maps to:** V1, P2
- **Tags:** detection

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| VT signals (ioctl VT_SETMODE) | discussed | Zero-latency kernel signals | Requires owning the console fd; compositor already claims it; only gets signals for the VT you own |
| libseat | discussed | Session management library | Not installed; adds a dependency |
| logind D-Bus only | discussed | Event-driven session signals | Requires D-Bus + systemd; may not exist on minimal Pis |

---

### D15: Notification lifetime — fixed timeout, v1 single notification

> Each notification shows for configurable duration, then dismisses. One at a time in v1. Queue interface designed for stacking later.

- **Chosen:** Fixed timeout, single notification, queue designed for future stacking
- **Rationale:** user said, "yeah, i'd aim for 1 for v1 with 2 and 3 as known future flex points. it wouldn't be bad to add priority levels to notifications even if we don't do anything with them now"
- **Maps to:** P1
- **Tags:** behavior, queue

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Stacking from v1 | discussed | Multiple visible at once | "real complexity to the rendering and layout logic — especially across three different backends" |
| Priority-based from v1 | discussed | High priority interrupts | Deferred; priority field exists in struct but v1 ignores it |

---

## Session: 2026-03-09 — Engine Architecture & SSH Notifications

**Context:** Second design session. Reviewed the v1 design doc and prototypes. Focused on engine extensibility (what if Sway has no notification daemon? what about links2 -g?), detection pipeline design, SSH terminal notifications, and notification data model enrichment. Resulted in the v2 design spec.

**GVP source:** Inferred inline

---

### D16: Engine vtable architecture (not just config booleans)

> Engines implement a vtable with detect/render/dismiss functions instead of being passive config structs.

- **Chosen:** vtable with `detect()`, `render()`, `dismiss()` + priority ordering
- **Rationale:** User said "what if i want to run a separate engine when `links2 -g` is detected? are there situations where we might detect Wayland but want to use a different backend because we (a) know that the notification dbus approach won't work or (b) just because we want to?" Claude proposed two directions: (A) engine as config + optional render function, or (B) full backend vtable. User did not explicitly choose between them, but engaged with Direction B throughout the session — asking about NEED_PROBE enums, rejection tracking, and the resolver loop — implicitly accepting the vtable approach through iterative design. User also said "oh god no to overengineering :p we want to meticulously explore future possibility states and have a solid idea where we're going to go before building. and ideally, if we can explore that enough, it shouldn't be too much effort to implement a feature rich v1 with minimal flex points for future additions."
- **Maps to:** G1, P1, V1
- **Tags:** architecture, engines, extensibility

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Engine as config + optional render function | discussed (Claude proposed as Direction A) | Declarative config with one function pointer bolt-on | User did not explicitly reject; engaged with Direction B instead |
| Static config booleans (v1 design) | original design | `has_dbus` + `conflicts_with_framebuffer` | User's question about links2 -g and optional backends implied need for more expressive engine model |

---

### D17: Multi-key session context with layered probe pipeline

> Engine detection uses a context struct with multiple keys (session_type, session_class, has_dbus_notifications, etc.) populated lazily via probes.

- **Chosen:** Multi-key context with demand-driven probing
- **Rationale:** User said "i kinda like #2 + #3, and using something like `has_dbus_notifications` as one of the keys" and envisioned "a sort of 'context' struct that gets passed to each engine, and as it gets built, an engine can say 'oh! i'm interested in this, let me queue an additional probe since the context contains XYZ'. then that probe is run, the context is updated, and then passed again to our engines until one accepts it or all reject it. this let's us start cheap, expend resources probing only where useful"
- **Maps to:** V1, P1
- **Tags:** detection, architecture

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Session type only (v1 design) | original design | Single key from logind | User said "i kinda like #2 + #3" when presented with options: (1) session type + probing, (2) multi-key detection, (3) ordered backend list per engine |
| Session type + probing only (Claude's option 1) | discussed | Probe D-Bus as a one-off, keep session type as sole key | User preferred the multi-key + ordered list combination (#2 + #3) |
| All probes upfront | discussed (implicit) | Run every probe before engine evaluation | User specifically envisioned lazy probing: "start cheap, expend resources probing only where useful" |

---

### D18: Static engine priority with config struct (not exposed in v1)

> Engines are tried in a hardcoded priority order. The ordering lives in a config struct ready to be surfaced later.

- **Chosen:** Static priority, config struct ready but not exposed
- **Rationale:** User said "i'd go with 1 for now :) 3 would be nice, and maybe we leave the default ordering in a config struct in case we ever want to go that route, but for now using a super basic `key=var` config, ordering feels more annoying than it's worth to expose via a config"
- **Maps to:** V1, P1
- **Tags:** config, engines

**Considered:**

| Alternative | Source | Description | Why not? |
|---|---|---|---|
| Engine-reported confidence scoring | discussed (Claude proposed as option 2) | Each engine returns a confidence score | Claude's analysis: "in practice the 'confidence' would just be hardcoded per engine anyway, which is the same as priority order with extra indirection." User chose option 1 without commenting on this reasoning. |
| User-configurable ordering from v1 | discussed | Expose in config file | User said "ordering feels more annoying than it's worth to expose via a config" for v1 |

---

### D19: NEED_PROBE as enum (compile-time safe probe requests)

> Engines request probes via a `probe_key` enum, not string names. Compiler catches invalid probe requests.

- **Chosen:** `probe_key` enum with `ENGINE_NEED_PROBE` result
- **Rationale:** User said "would it be reasonable to use an enum with NEED_PROBE? i like all the static :3"
- **Maps to:** P1
- **Tags:** architecture, detection

---

### D20: SSH terminal notifications — additive, with 4-tier fallback

> SSH notifications are always sent alongside local VT/engine notifications. Per-pty rendering falls back through OSC → tmux → cursor overlay → plain text.

- **Chosen:** Additive delivery, 4-tier terminal fallback
- **Rationale:** User said "SSH notifications are additive. i would *always* send notifications locally using the active tty/engine" and on rendering: "3 sounds closest to what i was thinking, but 4 is sexy if we can hook into that. can we do some probing there to see what the terminal supports from the server side? and have a 4 -> 3 -> 1 fallback approach?" and "oh, and to your question, i'd be fine with overwriting if there's a full screen, but IF we can easily detect that and make that configurable, that'd be ideal :)" The tmux tier (between OSC and cursor overlay) was added when user noted "i use tmux a lot! having the notification show up as a pop-up or display-message would be slick." Claude proposed tmux `display-popup` / `display-message`, user confirmed "looks good to me :)"
- **Maps to:** G1, V2
- **Tags:** SSH, rendering

---

### D21: SSH routing — explicit opt-in for non-owner users

> Daemon always notifies its own user's SSH sessions. Other users require explicit `ssh_users` or `ssh_groups` config.

- **Chosen:** Default to daemon owner only, explicit config for others
- **Rationale:** User said "i would have it *always* send the notification to SSH connections where lnotifyd's process owner is logged in (whether it's running as `root` or owned by a specific user). we would only send the notification to other users/groups if configured to do so. e.g.: we *could* configure `ssh_groups = sudo admin` so that root-level notifications still get passed along to users who are logged in AND have root access. i might consider making this the default with `ssh_group =` to set it to null, and while that's tempting (and somewhat aligns with the 'root notifications get shown everywhere' mentality... idk if it's just because it's SSH and it's making me think about standard security practices lol or what, but i think i'd default to 'only root' with extra groups requiring configuration (i.e.: require explicitly configured `ssh_groups = sudo admin`). i don't know if that makes sense lmao since we're already showing root-owned notifications *everywhere* to any logged in users at the screen. somehow it feels different over SSH, and i'm not sure if that's logically consistent :p thoughts?" Claude proposed a distinction: local VT rendering is passive (pixels on a screen in front of someone with physical access), SSH terminal writing is active injection into a remote session — a different trust boundary. User responded "i love all of that :D"
- **Maps to:** V3
- **Tags:** SSH, security, config

When Claude raised the edge case that a root system daemon's "always notify own user" rule would only notify root's SSH sessions (not the typical user's), and proposed an `ssh_always_user` config key, user said "the simpler answer is the preference -- just set `ssh_users` or `ssh_groups`. that's why they're there :p"

---

### D22: Client-side SSH opt-out via environment variable

> SSH clients control notification modes via `LNOTIFY_SSH` env var, plus server-side `ssh_modes` config.

- **Chosen:** `LNOTIFY_SSH` env var + `ssh_modes` config key (no per-user config variants)
- **Rationale:** User said "i would also allow an environment variable (or ssh options if SSHD allows setting/sending arbitrary options?) to let the client-side opt-out of each mode (OSC / overlay / injected plaintext) OR opt out altogether." When Claude proposed per-user config overrides (`ssh_modes_user_guy = overlay,text`), user said "i definitely like the idea of at least keeping `ssh_modes` as a config option. per-user seems icky to me lol, i don't know that i want to have dynamic vars in this just yet."
- **Maps to:** V1, V3
- **Tags:** SSH, config

---

### D23: Notification data model — app, group_id, origin_uid, split timestamps

> Notifications carry an optional app name, group ID for dedup, daemon-captured origin UID, and three timestamps (wall clock sent/received, monotonic for internal use).

- **Chosen:** Full model with all fields
- **Rationale:** User said "let's add an optional app/source attribute and i think a notification_group_id would be nice, too, for deduplication." On timestamps: user said "i might consider a `timestamp_sent` and `timestamp_recv` in case we want to ever note that for performance or even display it 'hey, i got this notification that the server is about to meltdown in 5 minutes... an hour ago.'" Claude proposed splitting into wall clock timestamps (cross-machine comparable, on the wire) and a separate monotonic timestamp (for local timeout math, immune to NTP jumps, never on the wire). User confirmed "nope! that looks perfect :)"
- **Maps to:** P1, V3
- **Tags:** data model, wire protocol

---

### D24: Color format — `#RRGGBBAA` with discriminator prefix

> Config colors use `#RRGGBBAA` hex format. The `#` prefix enables future format detection.

- **Chosen:** `#RRGGBBAA`
- **Rationale:** User said "yeah, i'd at least include the leading `#`. and it lets us add other color types (e.g.: `rgb(X, Y, Z)`) later without breaking configs if we really wanted to."
- **Maps to:** P1
- **Tags:** config

---

### D25: Shared rendering utilities (DRY across engines)

> Common rendering code (rounded rectangles, text drawing, etc.) lives in shared utility modules, not duplicated per engine.

- **Chosen:** Shared utilities, engines compose from them
- **Rationale:** User said "i love DRY. i don't want it to necessarily be the case that we have to rewrite vast render / detection pieces for compositors that require similar approaches"
- **Maps to:** P1
- **Tags:** architecture, rendering

---

### Decisions With Late-Captured Rationale

- **D3: Three-component architecture** — rationale captured in session 2 (2026-03-09). User said "a daemon lets us more easily send notifications from a user account that have root-permission propagation." On the D-Bus listener component specifically, user said "i'm honestly not sure haha, are we still using that? i would conceptualize that as a component of the daemon?" Claude clarified: the D-Bus listener is a component inside the daemon that implements `org.freedesktop.Notifications`, letting existing tools (`notify-send`, desktop apps) route through lnotifyd. User accepted this framing. Especially relevant for the Sway scenario where lnotifyd acts as the notification daemon.
- **D4: Unix socket for IPC** — rationale captured in session 2 (2026-03-09). User said "i actually don't remember D4 being made. sometimes when Claude produces a long response, i miss small details :3 if that was a 7 word suggestion in the middle of a long response, i could easily have missed it, and Claude noted my lack of response as confirmation." When revisited, user said "i'd go with a socket over D-Bus for exactly that reason -- portability."
