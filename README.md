# tiniclaw

A lightweight autonomous AI assistant runtime written in C.

Designed for minimal binary size, minimal memory footprint, and zero runtime
dependencies beyond libc and optional SQLite.

## Features

- **Multi-provider** — OpenAI, Anthropic, Gemini, Ollama, OpenRouter, and any OpenAI-compatible endpoint
- **Multi-channel** — Telegram, Discord, Slack, Matrix, IRC, Email, WhatsApp, iMessage, Lark, DingTalk, QQ, CLI
- **Tool execution** — shell, file r/w/edit/append, HTTP requests, web fetch/search, git, browser, image, screenshot, memory, cron, MCP delegate
- **Memory backends** — SQLite (default), Markdown, none
- **Cron scheduler** — cron expressions, `every:` intervals, `at:` one-shots; state persisted to JSON
- **HTTP gateway** — webhook server with bearer-token pairing, rate limiting, Prometheus metrics
- **MCP client** — JSON-RPC 2.0 stdio bridge; wraps external MCP tools as native tool vtables
- **Security** — sandbox backends (landlock, firejail, bubblewrap, docker), secrets store, audit log
- **Single binary** — no runtime, no VM, no garbage collector

## Build

**Requirements:** clang or gcc, cmake ≥ 3.16, SQLite, cJSON, OpenSSL, libpcre2

```bash
# macOS (Homebrew)
brew install cmake sqlite cjson openssl pcre2

# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# The binary
./build/tiniclaw
```

## Quick start

```bash
# One-shot agent turn
ANTHROPIC_API_KEY=sk-ant-... ./build/tiniclaw agent -m "what is 2+2"
OPENAI_API_KEY=sk-...        ./build/tiniclaw agent -m "what is 2+2"

# Interactive session
./build/tiniclaw agent

# Interactive setup wizard (writes ~/.tiniclaw/config.json)
./build/tiniclaw onboard
```

## Configuration

Config is loaded from `~/.tiniclaw/config.json`. Environment variables override config file values.

| Env var | Description |
|---|---|
| `OPENAI_API_KEY` | OpenAI API key (sets provider to `openai`) |
| `ANTHROPIC_API_KEY` | Anthropic API key (sets provider to `anthropic`) |
| `TINICLAW_PROVIDER` | Override default provider name |
| `TINICLAW_MODEL` | Override default model name |

Example `~/.tiniclaw/config.json`:

```json
{
  "default_provider": "anthropic",
  "default_model": "claude-haiku-4",
  "providers": [
    { "name": "anthropic", "api_key": "sk-ant-..." }
  ],
  "workspace_dir": ".",
  "memory": { "backend": "sqlite" }
}
```

## Commands

```
tiniclaw agent     [-m MSG]       Interactive agent (or one-shot with -m)
tiniclaw gateway   [--port N]     Start HTTP gateway server (default: 7007)
tiniclaw daemon                   Run agent as background daemon
tiniclaw onboard                  Interactive setup wizard
tiniclaw status                   Show configuration summary
tiniclaw doctor                   Run health diagnostics
tiniclaw cron      list|add|rm    Manage scheduled jobs
tiniclaw channel   list           List configured channels
tiniclaw models    list           List known provider models
tiniclaw version                  Print version string
```

## Providers

| Name | API endpoint |
|---|---|
| `openai` | `https://api.openai.com/v1/chat/completions` |
| `anthropic` | `https://api.anthropic.com/v1/messages` |
| `gemini` | `https://generativelanguage.googleapis.com/v1beta/openai/...` |
| `ollama` | `http://localhost:11434/api/chat` |
| `openrouter` | `https://openrouter.ai/api/v1/chat/completions` |
| `compatible` | Any OpenAI-compatible URL via `base_url` in config |

## Gateway

Start an HTTP server that accepts agent requests over webhooks:

```bash
./build/tiniclaw gateway --port 8080
```

Endpoints:

| Method | Path | Description |
|---|---|---|
| `GET` | `/health` | Liveness check |
| `GET` | `/ready` | Readiness check |
| `POST` | `/pair` | Exchange OTP for bearer token |
| `POST` | `/webhook` | Send a message to the agent |
| `GET` | `/metrics` | Prometheus-format metrics |

## Memory

| Backend | Description |
|---|---|
| `sqlite` | Persistent SQLite store with vector similarity search |
| `markdown` | Plain `.md` files in a directory |
| `none` | No persistence |

## Cron

```bash
# List jobs
tiniclaw cron list

# Add a job (every 5 minutes, runs a shell command)
tiniclaw cron add healthcheck "every:5m" "curl -s http://localhost:8080/health"
```

## Architecture

```
tiniclaw-c/
  src/
    main.c              CLI entry point and command routing
    agent/agent.c       Orchestration loop
    config.c            Config loading and merging
    gateway.c           HTTP gateway server
    cron.c              Cron scheduler
    mcp.c               MCP JSON-RPC 2.0 stdio client
    providers/          AI provider implementations
    channels/           Messaging channel implementations
    tools/              Tool implementations
    memory/             Memory backend implementations
    security/           Sandbox, pairing, secrets, audit
  include/              Public headers (vtable interfaces)
```

Extension points are vtable-driven: add a provider, channel, tool, or memory
backend by implementing the corresponding vtable struct and registering it in
the factory.

## License

See [LICENSE](LICENSE).
