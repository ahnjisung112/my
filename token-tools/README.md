# Token-saving tools for Claude Code (Windows)

| Tool | What it is | How it's installed |
|---|---|---|
| UsageScope | Paid app (one-time purchase) that tracks usage limits | Manual download: https://www.usagescope.com/ |
| Headroom | Local proxy and tray app that compresses context | Script (downloads and runs the GitHub release installer) |
| Serena | MCP server for code navigation by symbol | Script (`uv tool install` + `claude mcp add`) |
| claude-mem | Plugin that keeps memory across sessions | Slash commands in Claude Code |
| Ponytail | Plugin that pushes the model to write less code (YAGNI) | Slash commands in Claude Code |

Run it:

```powershell
powershell -ExecutionPolicy Bypass -File .\install-token-tools.ps1
```

Things to know:
- Headroom points `ANTHROPIC_BASE_URL` at a local proxy (127.0.0.1:6767) and adds a PreToolUse hook. When you quit Headroom, it restores both.
- The creator of the source video notes that Ponytail can raise total token use on reasoning-heavy models.
- If Serena starts slowly, set the `MCP_TIMEOUT=60000` environment variable.
