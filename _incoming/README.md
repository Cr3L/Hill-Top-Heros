# `_incoming/`

Staging area for work from elsewhere — another agent, a contributor, a snippet
off a forum, an older branch. Drop files or folders in here and ask for a
review.

**Nothing here is trusted and nothing here is applied without approval.** It is
input to a review, not a change to the project.

A review of anything in this directory answers four things: what is genuinely
better than what we already have, what is wrong, what breaks the project's
rules, and a proposed merge naming which parts to take and which to drop. The
full protocol — including the rule checklist and why cherry-picking beats
taking a whole drop — is in [`CLAUDE.md`](../CLAUDE.md) under
"Incoming work".

Contents are gitignored; this file is the only tracked thing here, so dropped
work never lands in history. Once a merge is approved and landed, delete the
source from here — this is a staging area, not an archive.
