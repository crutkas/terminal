# Developer skills

Skills in this directory are for **contributors working on this fork of
Windows Terminal / OpenConsole**. Copilot CLI (and other agents) read them to
perform repo-specific developer tasks — most importantly, reviewing a feature
branch before push.

This formalizes the multi-model review the team already runs by hand
(Claude + GPT + Gemini + a security pass + a test pass) and turns it into a
single command.

## Available skills

| Skill | Purpose |
|-------|---------|
| [`pr-review/`](pr-review/SKILL.md) | Multi-dimensional review of a feature branch / PR diff: security (untrusted VT input, C++ memory safety), correctness & edge cases, spec/protocol conformance, alternative-solution check, test coverage, docs & screenshots, build & performance impact, and a multi-model cross-check. Reports findings to stdout; does NOT apply fixes and does NOT build. |

## Conventions

- Each skill is a directory with a `SKILL.md` (the entry point the orchestrating
  agent reads) plus prompt fragments under `dimensions/`.
- Skills do not run scripts, builds, or tests. The orchestrating agent uses its
  own tools (`task`, `grep`, `view`, `powershell` for git, `gh`) per `SKILL.md`.
- Prompt fragments passed verbatim to sub-agents live under `dimensions/`.
- Output goes to stdout unless the user explicitly asks for a file.
- These are **fork-local developer tooling**. They are not part of upstream
  `microsoft/terminal` and are not shipped to end users. If a change here is
  ever proposed upstream, that's a separate maintainer decision.
