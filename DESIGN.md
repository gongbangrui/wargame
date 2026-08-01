# Wargame Design System

## 1. Atmosphere & Identity

The application is a dark operations console for repeated, time-sensitive
decisions. Its signature is restrained tactical contrast: layered charcoal
surfaces, compact mono metadata, and a teal signal accent reserved for
confirmed or actionable state. A cool blue secondary signal keeps maps,
selection, and information distinct from successful actions. The design favors
scanability over decoration and must not resemble a promotional landing page.

## 2. Color

| Role | Token | Value | Usage |
| --- | --- | --- | --- |
| Page | `--surface-page` | `#080b12` | Application and map background |
| Panel | `--surface-panel` | `#101820` | Tables, forms, and monitor panels |
| Raised | `--surface-raised` | `#17232d` | Modals and active controls |
| Border | `--border-default` | `#2a3b48` | Panel and input boundaries |
| Text | `--text-primary` | `#edf4f5` | Primary labels and values |
| Muted text | `--text-muted` | `#82919c` | Metadata and secondary help |
| Signal | `--accent-signal` | `#38d2b4` | Confirmed state, primary action, focus |
| Information | `--accent-info` | `#4c9dff` | Map information and neutral status |
| Warning | `--status-warning` | `#f0a040` | Pending or degraded state |
| Danger | `--status-danger` | `#ff7180` | Destructive action and failure |
| Communication range | `--range-communication` | `#edf4f5` | Selected/owned-unit communication radius |
| Detection range | `--range-detection` | `#4c9dff` | Selected/owned-unit sensor radius |
| Attack range | `--range-attack` | `#ff7180` | Selected/owned-unit weapon radius |

No color is decorative. Status colors always pair with visible text or an icon
label so the meaning does not rely on color alone. Web and QML use the same
semantic values. Gradients are limited to the page atmosphere and primary
action/selection surfaces: a charcoal-to-blue-black background and a short
teal-to-cyan signal ramp. Status badges, panels, and ordinary controls remain
flat so state is immediately readable.

## 3. Typography

Use the existing system sans stack for headings and body text, and
`Consolas, monospace` for coordinates, timestamps, revisions, and compact
operational metadata. The existing 10-13px metadata, 12-14px body, and 20px
workspace heading hierarchy is preserved. Letter spacing is zero. Text must
wrap or truncate with a title/accessible name rather than overlap neighboring
controls. Explanatory copy is shown only when it prevents an error; headings,
labels, live state, and concise empty states carry the rest of the interface.

## 4. Spacing & Layout

Spacing uses a 4px base. Compact controls use 8-12px gaps, panel interiors use
16-24px padding, and workspace sections use 20-32px separation. The desktop
console is a sidebar plus one vertically scrollable workspace; at 760px the
sidebar becomes a horizontally scrolling navigation row and primary content
remains a single column. Room collections own their vertical flow and may use
a compact responsive grid. Wide data tables alone own horizontal scrolling.
Dialogs constrain their body and own internal scrolling; neither dialogs nor
the workspace may create page-level horizontal clipping.

## 5. Components

### Lifecycle Badge
- **Structure:** status dot or compact label plus visible text.
- **Variants:** stopped, preparing, ready, paused, running, finished, stale,
  unknown, pending, acknowledged, and failed.
- **States:** default, refresh-pending, stale, and error.
- **Accessibility:** the text states the status; it is not dot-only.
- **Motion:** color/opacity transition only when the observed status changes.

### Room Card & Operation Control
- **Structure:** room name/status, capacity/readiness, occupants, one
  context-aware primary lifecycle command, and an overflow menu for edit,
  reset, redeploy, stop, force-stop, and delete. The card is a repeated item,
  never a page section or a card nested inside another card.
- **States:** enabled, disabled, pending, acknowledged, and failed.
- **Accessibility:** keyboard reachable; disabled controls keep their reason in
  a tooltip or adjacent status summary rather than hiding the action. Overflow
  menus use native focus behavior and close after a command is chosen.
- **Motion:** card hover, menu reveal, pending feedback, and toast use opacity
  and transform only; transforms never shift neighboring layout.

### Operational Table
- **Structure:** concise identity, status badge, compact summary, and action
  cluster.
- **States:** loading, empty, populated, stale, and request-failed.
- **Accessibility:** long labels preserve their full text through `title` or
  accessible labels; action controls use explicit names.

### Command Buttons
- **Structure:** familiar icons for refresh, logout, overflow, and destructive
  tools; short icon-plus-text labels for lifecycle commands whose meaning is
  domain specific.
- **Hierarchy:** one primary command per local task, quiet secondary commands,
  and explicit red treatment only for destructive confirmation.
- **Geometry:** 6px radius, stable 32-36px height, and no pill-shaped actions.

### Map Coordinate Contract
- **Structure:** simulation `x` is east and `y` is north, both in meters from
  the map origin. Tile projection is EPSG:3857/XYZ; the map metadata supplies
  logical origin, extent, and tile zoom.
- **States:** valid metadata, rejected or stale metadata, in-bounds coordinate,
  and bounded picked coordinate.
- **Interaction:** rendering and overlay placement share the tile renderer's
  logical/screen conversion. Picking uses its inverse and clamps to the
  declared extent in the shared `MapCoordinates` helper. Ranges, routes,
  markers, deployments, and labels consume the same logical coordinate.
- **Tokens:** coordinate readout uses `--surface-map-readout`,
  `--border-map-readout`, and `--text-map-readout`; shared markers use
  `--status-danger` / `--accent-info` with `--surface-map-marker-stroke` and
  `--text-map-marker`. Confirmed action feedback uses `--accent-signal`.

### Projected Map Marks & Ranges
- **Mark hierarchy:** a mark sent by the current seat uses a bright outlined
  circle and the visible label "我的标记"; a commander mark uses a larger
  double-outlined diamond and the visible label "指挥官". When the server does
  not project `markType`, ownership is derived only from the projected `seatId`
  and the controller's confirmed `currentSeatId`.
- **Range controls:** communication, detection, and attack ranges are three
  independent local checkboxes. Their lines are respectively white, blue, and
  red, and apply only to the selected friendly unit or the participant's owned
  unit. Toggling them never sends a command or changes authoritative state.
- **Accessibility:** line color is paired with a visible text checkbox; mark
  ownership is carried by geometry and text as well as color.

### Communication State & Inbox
- **Badge variants:** bilateral (including the legacy `twoWay` alias),
  receive-only, and disconnected. The visible labels are "双向通信",
  "仅可接收", and "通信中断".
- **Composer:** subordinate seats address their commander. It remains available
  during preparation; while running it sends only in bilateral state.
  Commander seats receive the live room inbox through the shared chat drawer.
- **Accessibility:** a disabled composer retains a concise reason beside the
  input, and the badge always includes state text.

### Weapon Cooldown
- **Structure:** AttackUAV details show "换弹中", remaining seconds, and a
  determinate progress bar computed from `1 - cooldownRemaining / cooldownSec`.
- **Behavior:** attack actions are disabled while remaining time is positive.
  Reaching zero only restores the action; it never fires automatically.

## 6. Motion & Interaction

Use 150ms for button feedback and 200-300ms for modal, toast, badge, menu, and
page state changes. Animate only opacity and transform. A restrained page-load
reveal and a short signal sweep may run once; motion otherwise signals a state
change, request outcome, or focus change. No looping decorative motion is
introduced. `prefers-reduced-motion` removes all non-essential motion.

## 7. Depth & Surface

Use the existing mixed strategy: tonal surface progression and one-pixel cool
boundaries establish ordinary hierarchy; modest dark shadows are reserved for
modals, menus, active room cards, and toasts. Corner radii remain compact at
6-7px. Nested cards are not used for page sections.

## 8. Accessibility Constraints & Accepted Debt

Target WCAG 2.2 AA contrast, visible keyboard focus, full keyboard access to
all room operations, and reduced-motion support. Status text remains visible
when the game-service status is stale or unknown.

| Item | Location | Why accepted | Owner / Exit |
| --- | --- | --- | --- |
| Game authority acknowledgement | `server/game/` contract | Todo 8 must not fabricate an acknowledgement before the game service reports one. The account console therefore shows a request as pending until the existing internal status callback is observed. | Lifecycle/protocol work |
| Map state revision | `qml/components/MapCanvas.qml` | Revision `0` is accepted only before a positive revision is observed; stale or repeated positive revisions are ignored without changing the current map view. | Map metadata integration |

## Research Log

- Existing UI audit: extracted recurring colors, compact panel geometry,
  sidebar/workspace shell, table, modal, toast, and reduced-motion rules from
  `server/account/static/app.css`, `qml/components/MapCanvas.qml`, and the
  reusable QML controls.
- Map audit: `map/metadata.json` and `ScenarioIo::defaultScenario()` both
  declare a 20,000m x 15,000m east/north extent; the previous MapCanvas and
  fallback shell defaults were the conflicting 40,000m x 30,000m values.
- External reference and image-generation lanes were not used: this is an
  extraction of an established internal operations console, not a greenfield
  visual direction.
