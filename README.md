# AmigaMCP

A native AmigaDOS command that talks to an LLM API **over HTTPS, straight from
a 68k Amiga**. No bridge, no PC in the loop. You, the machine, the API.

```
AmigaMCP MODEL claude-opus-4-8 INSTRUCTION "mange des nouilles" TOKENS=14400
AmigaMCP RESUME_JOBS
```

## Why it's reboot-proof

Every request is a **durable job** on disk under `SYS:.claude/`:

| file | when |
|------|------|
| `2026-06-12-153012-JOB_PENDING` | written *before* the call |
| `2026-06-12-153012-JOB_DONE`    | renamed once the answer is in |
| `2026-06-12-153012.md`          | prompt + answer transcript |

The on-disk job **is** the resilience. If the machine resets mid-call, the
`JOB_PENDING` file survives. Put one line in your `WBStartup` or
`s:user-startup`:

```
AmigaMCP RESUME_JOBS
```

…and every pending job replays on the next boot. The process doesn't need to
be stable — only the filesystem, which on this kind of Amiga already expects a
hard reset.

## Config

Plain text, default `SYS:.claude/config`. 
`keyword value` per line; use `#`/`;` for comments. See
[`config.sample`](config.sample).

```
key       sk-ant-...
model     claude-opus-4-8
provider  anthropic
host      api.anthropic.com
path      /v1/messages
port      443
version   2023-06-01
```

`provider openai` switches to the OpenAI chat shape (`Authorization: Bearer`,
`choices[0].message.content`), which also covers GPT, Gemini's compat
endpoint, Mistral, and a local llama.cpp / Ollama on the LAN.

## Command line (`ReadArgs` template)

```
MODEL/K  INSTRUCTION/K  TOKENS/K/N  RESUME_JOBS/S  DIR/K  CONFIG/K  NOJOB/S
```

- `MODEL` / `TOKENS` override the config defaults for this run.
- `NOJOB` - fire a one-shot, print to the shell, write no job file.
- `DIR` / `CONFIG` - relocate the harness dir / config file.

The answer is printed to stdout, so ARexx drives it with zero extra code:

```rexx
ADDRESS COMMAND 'AmigaMCP NOJOB INSTRUCTION "résume ce projet" >T:reply'
/* ... then read T:reply, or the .md under SYS:.claude/ */
```

## Build (Bebbo amiga-gcc)

```sh
make            # cleartext transport (HTTP / LAN proxy / local llama.cpp)
make amissl AMISSL=/path/to/amissl-sdk   # real HTTPS to api.anthropic.com
```

The HTTPS transport ([`net_amissl.c`](net_amissl.c)) uses **AmiSSL 5.x**
(OpenSSL API). It needs the AmiSSL SDK on the build host and AmiSSL installed
on the Amiga at run time. The cleartext transport
([`net_plain.c`](net_plain.c)) needs only a `bsdsocket.library` TCP stack
(Roadshow, AmiTCP, Miami…).

## Files

| file | role |
|------|------|
| `AmigaMCP.c`   | command: args, config, jobs, HTTP, JSON, RESUME |
| `net.h`        | transport seam (open / write / read / close) |
| `net_plain.c`  | bsdsocket cleartext backend |
| `net_amissl.c` | AmiSSL HTTPS backend (`-DUSE_AMISSL`) |
| `jsmn.h`       | minimal JSON tokenizer (MIT) |
| `config.sample`| starter config |

## Status

- Cleartext build: **compiles clean** (zero warnings) to a 68020 AmigaOS
  binary with the Bebbo toolchain; logic suite (`make test`) passes.
- HTTPS build: **compiles & links clean** against the **AmiSSL 5.27 SDK**
  (`make amissl`). Init/seed sequence follows the official SDK example
  (utility.library, InitAmiSSLMaster, manual RNG seeding, `$STACK` cookie).
- Runtime: not yet exercised on hardware/UAE — needs a TCP stack and AmiSSL
  installed on the Amiga, plus a real API key.

## Sharp edges (by design)

- No certificate verification by default in the AmiSSL path (we call one known
  host). Flip `SSL_VERIFY_PEER` + a CA bundle in `net_amissl.c` to enable.
- The API key sits in a plain file. Keep `SYS:.claude/` to a trusted machine.
- Non-streaming: one request, read to EOF, parse. Big answers are bounded by
  your `TOKENS` budget and free RAM.
