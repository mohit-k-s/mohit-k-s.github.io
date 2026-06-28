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

THe phone POSTs below payload to my server

```json
{
  "heart_rate": 82,
  "unit": "bpm",
  "sent_at": "2026-06-28T14:30:00+05:30",
  "device": "iphone"
}
```

## Why This Was Worth Building

I had *fun* making it , Not everything needs to be a product. Sometimes you can just piece up small things to make something useful for yourself. Build for yourself first !