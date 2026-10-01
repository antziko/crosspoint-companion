# CrossPoint Sync (KOReader plugin)

Keeps the bookmarks and highlights on a CrossPoint (Xteink) e-reader and in KOReader in
step, and syncs the reading position with it, through the same self-hosted
`koreader-sync-server` that stock progress sync already uses.

Annotations sync **both ways**, but uploading is off until you turn it on.

## Install

Copy the whole `crosspointsync.koplugin` directory into KOReader's plugin folder and
restart KOReader:

| Platform | Destination |
|---|---|
| Kobo / Kindle / PocketBook | `.adds/koreader/plugins/` (Kobo) or `koreader/plugins/` |
| Android | `koreader/plugins/` on internal storage |
| Desktop / emulator | `<koreader source>/plugins/` |

Then: **☰ → Tools → CrossPoint sync**.

## The menu

```
CrossPoint sync
├─ Sync now
├─ Sync automatically                        [on]
├─ Bookmarks and highlights: Receive from CrossPoint  ▸  Off / Receive / Two-way
├─ Reading position: Receive from CrossPoint          ▸  Off / Receive / Send / Two-way
├─ Server and login
└─ Advanced                                  ▸  Match books by filename, diagnostics,
                                                forget this book's sync record
```

Two things sync — bookmarks and highlights, and the reading position — and each is in its
own mode. The position has four (off, receive, send, two-way); annotations have three,
because *send only* is not a state this plugin can be in: an annotation upload is built
from the merge against the fetch that immediately precedes it, so receiving is how
sending works. A position carries no merge, so **Send to CrossPoint** is a real setting —
it means this device decides where you are, and the device's own position is ignored.

**Sync now** does everything that is turned on, in both directions. So does an automatic
sync when a book opens.

## Setup

Open **Tools → CrossPoint sync → Server and login** and enter the server
address, username and password. The password is not stored: the server authenticates with
the MD5 of it, so only that is kept.

If you leave the username blank it falls back to whatever the stock **Progress sync**
plugin has configured, so there is one login to maintain. That fallback depends on
internals which have moved between KOReader releases, so when the diagnostics report no
credentials while progress sync plainly works, set a login here instead.

Book matching follows the device. CrossPoint defaults to matching **by filename** —
`(X3)`/`(X4)` tags are stripped and `Author - Title` is canonicalised against
`Title - Author`, so a tagged copy on the device matches an untagged one here. That is
this plugin's default too. A sync that finds nothing under one key automatically retries
the other, so a mismatched setting costs one extra request rather than showing an empty
result.

## Reading position

A CrossPoint device files its annotations and its reading position under the **same**
document key, so once a book matches well enough for its highlights to arrive, its
position is already sitting there too. **Reading position → Receive from CrossPoint** (the
default) takes both in one go.

This matters because stock **Progress sync** computes its own document key, which need not
be the one the device used — which is why a book's annotations can arrive here while its
position does not. Set **Reading position → Off** if you would rather leave the position
entirely to stock progress sync.

- A sync you asked for goes straight there.
- An automatic sync, on opening a book, **asks first** — a silent jump as a book opens is
  indistinguishable from having lost your place.
- Either way the place you were is pushed onto the location stack, so **Back** undoes it.

### Sending your position back

**Reading position → Send to CrossPoint** and **→ Two-way** both upload where you are, so
the device follows KOReader as well — which means you need only this plugin, not stock
progress sync as well. Sending is off by default. Use **Send** when this device should be
the one that decides the position and the device's own is to be ignored; use **Two-way**
when either may be ahead.

> **Turn KOReader's own Progress sync off first** — manually, under
> *Tools → Progress sync → Automatically keep documents in sync*. This plugin will not
> change another plugin's settings, but it does check on the way in and says so if stock
> sync is still set to write. Both write the same record, and whichever saved last wins,
> which is not always the one you were reading on.

It sends when you close the book, when the device suspends, and after about 15 seconds of
not turning pages (and only once at least 10 pages have turned, so a book merely left open
never touches the network). **Sync now** also sends it on demand, and the
*Send my reading position to CrossPoint* gesture action still sends it on its own.

It does not send a position that nothing has changed since — including one the device
offered and you declined, for as long as you have not moved. Re-sending an unchanged
position is not free: the server keeps a richer anchor alongside the plain one for
CrossPoint devices, and discards it on any write that carries none.

## Sending your own changes back

**Bookmarks and highlights → Two-way** is off by default, and asks before it goes on.
While it is off the device is the only writer, so an annotation you delete here stays
deleted here but the device keeps it.

The delete is not thrown away, though. It is recorded in the book's sidecar as a whole
record — the device identity a tombstone needs exists nowhere else once the annotation is
gone — and goes out on the first sync after you turn two-way on. Re-making the mark at
the same spot takes the delete back. This is what makes *delete a few, then enable
two-way* work; before it, the next sync simply put the annotations back and the deletes
could never be told to anyone.

Turning it on has two consequences worth knowing before you do:

- the **first** upload sends every annotation the book already has, which may be many more
  than you expect;
- this device can now change what the others hold.

How the two sides agree without a clock: a CrossPoint device has no RTC, so every mark
carries a Lamport version instead. For each spot the highest version across
{bookmark, tombstone} on either side wins, and a tie goes to the live bookmark. A delete
is a tombstone one tick above everything seen, which is why it survives a re-add of the
same mark elsewhere — and why deleting here now sticks. This plugin implements the same
rule as `BookmarkStore::mergeFrom`, because a merge the two sides disagree about never
settles.

Safeguards, all of them in the write direction:

- the blob is merged against the GET immediately before the PUT, so the window in which
  another peer can be overwritten is one round trip. It is not zero: the server has no
  compare-and-swap;
- an upload is **refused whole** if the merged set would exceed the 128 marks a device can
  hold, rather than being truncated — a truncated blob loses records for every peer;
- a record this plugin does not understand is carried back **verbatim**, so a rewrite here
  never strips a field the device relies on;
- a deletion stays owed until a pull shows the spot gone from the server, not merely
  until an upload is accepted. An accepted tombstone can still lose: a device that raised
  the record's version without publishing it outranks a stamp made from what the server
  showed, and a tie goes to the live bookmark. The delete is simply re-sent, one above
  whatever beat it, and the annotation is never put back here in the meantime;
- annotations are only ever *removed* here if this plugin inserted them.

## What it does, and does not, do

- **A deletion follows the anchor, not the identity.** A device that adopts a mark made
  here re-files it under its own coordinates, so the spot changes while the XPointer does
  not. Deleting therefore buries every record naming that anchor, the device's re-keyed
  copy included — otherwise the delete lands on the abandoned spot and the mark comes
  back on the next pull.
- **A mark KOReader made gets a synthetic identity.** The device keys a highlight by
  page-local word indices and a bookmark by its own paragraph numbering, neither of which
  KOReader can reproduce. It does not need to: an identity only has to be stable and
  unique. So a mark made here is keyed by a hash of its XPointer, in a numeric range the
  device's own numbering cannot reach, and travels with the anchor alongside. A device
  that knows about this re-files the mark under its own coordinates and tombstones the
  synthetic one; a device that does not simply stores and echoes it.
- A mark the device could not resolve to an XPointer is **skipped and counted**, never
  approximated — but it is still passed back on upload, so it is not lost for anyone else.
- **The reading position travels both ways**, but as a plain XPointer and percentage.
  The device's richer anchor is dropped by the server on any write that lacks one, so a
  push from here costs the device a little precision on its own next restore.
- An edited note or a changed highlight colour does not travel: the device stores neither.

## Tests

```sh
busted            # with busted installed
texlua spec/run.lua   # or any Lua 5.1+ interpreter, no rocks needed
```

## Troubleshooting

A sync that produces nothing looks the same whatever caused it, so the plugin keeps a
trace of the last attempt. **Tools → CrossPoint sync → Advanced → Show sync diagnostics**
prints it; *Save sync diagnostics to a file* writes it to `crosspointsync-debug.txt` in
KOReader's data directory, which is easier to copy off over USB. The same lines go to
`crash.log` through KOReader's logger.

The trace names the thing that actually went wrong:

| Line | Meaning |
|---|---|
| `no kosync settings at all, and no login set here` | Nothing to authenticate with. Set one under *Server and login*. |
| `kosync settings present, fields: ...` | Stock kosync is configured but does not expose `username`/`userkey` under those names in this KOReader build. Set a login here instead. |
| `nothing under that key; retrying...` | The first document key was empty. The retry is normal and harmless; if the second key works, set *Advanced → Match books by filename* to match the device permanently. |
| `response blob: absent` after both keys | The server holds no annotations for this book under either key — the device has not uploaded, or uploaded under a different name. |
| `[n] NO ANCHOR (device sent no xp)` | The device stored the mark but could not resolve an XPointer for it, so there is nothing to place. Sync again from the device after opening the book there. |
| `[n] ANCHOR NOT IN THIS BOOK` | The anchor is well-formed but does not resolve here — usually a different edition or conversion of the same title. |
| `position: staying put -- the server holds no reading position for this book` | The key matched for annotations but carries no progress record; sync progress from the device. |
| `position: staying put -- already at the position the device recorded` | Nothing to do: the two agree to within 0.01% of the book. |
| `position: anchor does not resolve in this book` | Same cause as `ANCHOR NOT IN THIS BOOK`, for the position rather than a mark. |
| `here since last sync: N new, M deleted ...` | What changed in KOReader since the last sync, and the Lamport clock it is measured against. |
| `cannot tombstone (no identity recorded)` | A mark pulled by a release older than two-way sync, deleted here before the first pull that could record its identity. It comes back once, then behaves normally. |
| `cannot send (no usable anchor)` | An annotation with no XPointer, or one naming no `DocFragment` — nothing the device could place. |
| `upload refused: ...` | The merged set exceeds the device's 128-mark cap. Nothing was sent; delete some marks on either side. |
| `nothing to upload: the server already holds this set` | Normal. The merge produced exactly what is stored. |
| `bookmarks and highlights: receive` | Uploading is off, so a deletion here is held rather than sent. This is the first line to check when a change does not reach the device. `off` on that line means annotations are not syncing at all. |
| `owed-deletes=N` in `merge inputs` | Deletions made here while uploading was off, still waiting to go out. They leave on the first sync with two-way on. |
| `N deletion(s) held until uploading is on` | Confirms the deletes were recorded rather than undone. |
| `tombstoning ... deleted here earlier` | One of those held deletes, now going out. |
| `tombstoning ... a device's copy of a mark deleted here` | A mark made here that a device had adopted and re-keyed. The deletion follows the anchor to the device's own record, which is what actually has to be buried. |
| `deletion taken back (the mark is here again)` | A held delete was cancelled because the annotation was re-made at the same spot. |
| `deletion confirmed, the server no longer holds it` | A delete has landed and is no longer owed. Until this appears the plugin keeps re-sending it. |
| `merge inputs: remote b=.. t=.., ledger=.., uploading ..` | What went into the merge. `ledger` is how many marks this plugin is tracking for the book. |
| `tombstoning ... at v=N (was v=M)` | A mark deleted here, going out as a tombstone. `N` must be greater than `M`, or the device keeps the bookmark. |
| `sending ... at v=N -- made here` | A mark made here, going out under its synthetic identity. |
| `merged to N spots: live=.. dead=..` | The outcome per spot, after both sides' versions were compared. |
| `re-keyed by a device, keeping the annotation` | A device adopted a mark made here and gave it real coordinates. Expected, once per mark. |
| `uploading b=.. t=..` then `upload: ok` | The write succeeded. A `FAILED` here keeps the deletions pending for the next pull. |
| `a pull is already in flight` | An automatic sync was still running. Harmless; try again. |

Both document keys are printed on every sync, so they can be compared against the device
directly. The device's own mode is **Settings → KOReader Sync → document matching**, and
it defaults to filename.
