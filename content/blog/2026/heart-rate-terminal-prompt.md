+++
title = "Putting My ❤️ Rate in My Terminal Prompt"
date = "2026-06-28"
category = "tech"
tags = [
    "apple-health",
    "terminal",
    "python",
    "local-first",
]
+++

My terminal prompt now starts like this:

<img src="../images/hrterm.png" alt="Terminal prompt showing heart rate" width="700"/>

That little heart is my latest Apple Health heart rate reading.

This is not a product. It is not a polished app. It is a tiny local bridge between my iPhone and my shell prompt, held together by an iPhone Shortcut, a small Python HTTP server, and a JSON file.


## Flow

The whole thing is intentionally boring:

```text
iPhone Shortcut
    ↓ POST JSON
local Python server
    ↓ writes latest-health.json
prompt helper
    ↓ reads JSON
zsh prompt
```

The iPhone reads the latest heart rate sample from Apple Health and posts it to a server running on my laptop. The server stores only the latest reading. The shell prompt does not call the server at all. It just reads the local cache file and prints a compact prompt segment.

No database. No cloud. No account. No dashboard.

Just this:

```json
{
  "heart_rate": 82,
  "unit": "bpm",
  "sent_at": "2026-06-28T14:30:00+05:30",
  "device": "iphone"
}
```

## Why Not Use an App?

Because I did not want another app.

I wanted the value to show up where I already spend time: the terminal. I also did not want this data going through a random third-party service just so I could render a number next to `~/code`.

Apple Health data is personal enough that the right default is local-only. The laptop receives the reading over the local network, writes a file, and that is it.

The server has two endpoints:

```text
POST /health
GET  /health/latest
```

The prompt helper skips HTTP entirely:

```sh
python3 health_prompt.py --cache-file latest-health.json
```

That matters because prompts run constantly. A prompt should not hang because a server is down or Wi-Fi is weird.

## The Annoying Part

The annoying part is the iPhone automation.

There is no magic Apple Health API that lets my laptop casually ask my phone for the latest heart rate sample. At least not in the small local-first way I wanted this to work.

So the setup is manual:

1. Create an iPhone Shortcut.
2. Read the latest Heart Rate sample from Health.
3. Format it as JSON.
4. POST it to `http://YOUR_LAPTOP_IP:8787/health`.
5. Optionally add an API key header.
6. Trigger the Shortcut however you want.


## The Server

The server is just Python from the standard library. No packages.

It validates the payload enough to avoid writing nonsense, then replaces `latest-health.json` with the newest reading.

The API key is optional. If configured, the Shortcut sends:

```text
X-Health-Api-Key: some-long-random-value
```

This is not internet-facing software. It is meant to run on my machine, on my network. The API key is there so another device on the LAN cannot casually write junk into my prompt.

## Limitations

This is not real-time.

It depends on when the Shortcut runs, whether the phone and laptop are on the same network, and whether Apple Health has a recent sample. The prompt shows an age suffix because stale data is worse than no data if it pretends to be current.

It is also not portable in the "install this and it just works" sense. The iPhone setup is manual. You need to know your laptop's LAN IP. If your network changes, you may need to update the Shortcut.

That would be unacceptable for a product. For a personal terminal hack, it is fine.

## Why This Was Worth Building

I had *fun* making it , Not everything needs to be a product. Sometimes you can just piece up small things to make something useful for yourself. Build for yourself first !