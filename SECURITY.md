# Security policy

## Reporting a vulnerability

Please report security problems privately, not in a public issue.

Use GitHub's private reporting: on the repository page open the **Security** tab and choose **Report a
vulnerability**. Say what you found, which version you ran (the bottom right corner of the launcher shows it), and
how to reproduce it. A game file or a save state that triggers the problem helps, but do not attach anything you
don't have the right to share.

You can expect an answer within a week. NovaStar64 is a one-person project, so fixes arrive when they can, and a
report that comes with a way to reproduce it gets fixed sooner.

## What counts

NovaStar64 reads files you give it (game images, save states, texture packs, settings, Lua scripts) and runs them
on your computer. These are in scope:

- a crash, a hang or memory corruption caused by a damaged or hostile game image, save state, texture pack or settings file
- a way for a game image, save state, texture pack or settings file to run code outside the emulator, or to read
  or write files it should not (a Lua script you chose to run does not count, see below)
- a mistake in how an installer or the release workflow handles files

A game that simply does not run correctly is a normal bug; please use the bug report form instead.

## What it does not do

NovaStar64 has no networking code. It does not check for updates, send reports or collect any information.
Lua scripts are not sandboxed: they have the standard Lua libraries, including `io` and `os`, so a script can do
anything a program you run can do. Only run scripts you trust.

## Supported versions

Only the latest release gets fixes.
