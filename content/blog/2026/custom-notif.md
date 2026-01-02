+++
title = "Creating custom notifications for reminders"
date = "2026-01-02"
tags = [
    "random",
]
+++

Humans weren't meant for sitting, and it has caused me a great deal of issues personally. I wanted to schedule reminders on Mac for standing up and drinking water, since I lose track of time pretty easily.

Turns out this is very simple to do.

1. Create a shell script (for example, `drink-water.sh`):


```sh
#!/bin/bash
osascript -e 'display notification "You need water" with title "Drink water"'
```

2. Make the script executable, then register it with `launchd`.


3. If it doesn't already exist, create `~/Library/LaunchAgents`.

4. Create `~/Library/LaunchAgents/com.my.drink-water.plist`:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>drink.water.reminder</string>

  <key>ProgramArguments</key>
  <array>
    <string>/full/path/to/drink-water.sh</string>
  </array>

  <key>StartInterval</key>
  <integer>1800</integer>

  <key>StandardErrorPath</key>
  <string>/tmp/water.err</string>
  <key>StandardOutPath</key>
  <string>/tmp/water.out</string>
</dict>
</plist>
```

5. Load it:

```sh
launchctl load /full/path/to/com.my.drink-water.plist
```