# CLAUDE.md: project memory entry point

Project: open-source support for the **Kodak i2600** document scanner (USB `040a:601d`)
on a **Raspberry Pi**, scanning headlessly into **Paperless-ngx**, with the
scanner's panel (LCD function number) and buttons in use.

## Read first, every session
1. `memory/STATUS.md`: where we are and what's next.
2. `PLAN.md`: phases, structure, exit criteria.
3. Relevant entries in `memory/FINDINGS.md` and `memory/DECISIONS.md`.

## Update before ending a session (and commit it)
- `memory/STATUS.md`: current phase, done/next, blockers.
- `memory/JOURNAL.md`: append a dated entry (what was tried, the result, links to files/commits).
- `memory/FINDINGS.md`: new verified facts, **each with source + confidence**.
- `memory/DECISIONS.md`: any architecture/tooling decision as a new ADR.
- `memory/OPEN_QUESTIONS.md`: add or close questions.
Commit memory updates with the prefix `memory:`.

## Hard rules
- **Never commit vendor binaries, firmware, installers, or decompiled vendor code.** They live in `vendor/` (git-ignored).
- Clean-room: `backend/` is written only from `docs/protocol/`, never from decompiled listings.
- Never send unclassified opcodes to the real device; never touch firmware update commands.
- Everything above the driver talks only to SANE (the driver must stay swappable: vendor-under-box64 ↔ native).
- Mark hypotheses as hypotheses. Facts in FINDINGS.md need a source (capture file, doc URL, symbol name).

## Layout (short)
`memory/` project memory · `docs/protocol/` protocol spec · `re/` RE notes · `captures/` USB pcaps ·
`tools/` our tooling · `backend/` native SANE backend · `pi/` scan daemon · `paperless/` upload/profiles · `vendor/` ignored.
