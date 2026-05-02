# Claude Code Guide

Use this project as an Agent-ready Qt QML + bgfx starter kit.

Start with:

```powershell
ctest --test-dir .build-release\build -C Release --output-on-failure
```

When changing UI or rendering, verify with the real app smoke tests. Use the MCP
tools in `tools/testbridge-mcp` for live inspection instead of guessing QML
object state from source alone.

Important docs:

- `AGENTS.md`
- `docs/TEMPLATE_USAGE.md`
- `docs/AGENT_WORKFLOWS.md`
- `docs/PROJECT_CONVENTIONS.md`
- `docs/RENDER_PROVIDER_SDK.md`
