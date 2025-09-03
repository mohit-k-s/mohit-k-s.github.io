+++
title = "Building Meteo"
date = "2025-08-21"
description = "How I built a metro navigation app for Delhi using web technologies and open data"
tags = [
    "hobby_projects",
]
+++

Another weather app ?

<img src="../images/i-cant-hear-you-yes.gif" alt="Can't hear you over the metro noise" width="200" />

No !


I built this app for myself. The design isn't perfect (yet), but it works. Here's how I put it together.
One might ask , why ? To that i have no answer as i just thought that this can be cool !

<div class="iframe-container">
<iframe src="https://meteo-v1.vercel.app/" title="Meteo" loading="lazy" ></iframe>
</div>

## The Data Challenge

Building any transit app starts with data—lots of it. I needed:
- All metro stations with accurate coordinates
- Line connections and routes  
- Station elevations (for that authentic metro map feel)

### Station Data Collection

I started with the official DMRC website, manually extracting all lines and stations. Getting coordinates was trickier. My first attempt used [Nominatim](https://operations.osmfoundation.org/policies/nominatim/), OpenStreetMap's geocoding service, but the results were disappointing—many stations were missing or had wildly inaccurate locations.

Plan B: Google Maps API. More reliable, but required careful rate limiting and cost management.

The final dataset lives in [dmrc.json](https://github.com/mohit-k-s/meteo/blob/main/public/dmrc.json) on GitHub. It includes:
- 250+ metro stations
- Coordinate data for each station
- Line connections and transfers
- (Mostly fictional) elevation data

### The Elevation Problem

Metro maps show elevation changes, but this data isn't publicly available. My current solution? I generated random elevations that look reasonable. Not ideal, but functional.

Future plan: Use OCR on official metro elevation maps to extract real data.

## Route Planning Algorithm

The core routing uses a simple depth-first search (DFS) to find paths between stations. Delhi Metro's network guarantees connectivity—you can get from any station to any other station, even if it requires multiple transfers.

The algorithm could be optimized with Dijkstra's for shortest paths, but DFS works fine for this use case.

## Frontend Implementation

I used Leaflet.js for the interactive map—it's lightweight and handles the station plotting beautifully. The UI is minimal by design:

- Search for start/end stations
- Visual route display on the map
- Step-by-step directions
- Transfer information

The interface needs work. I know it's not intuitive, but hey:

<img src="../images/honest_works.jpg" alt="It ain't much, but it's honest work" width="300"/>

## Deployment

Vercel made deployment trivial. Push to GitHub, automatic builds and done!

## Lessons Learned

1. **Start with data**: Good data is harder to find than you think
2. **Simple algorithms work**: DFS solved the routing problem perfectly
4. **Open source everything**: Someone else might find this useful

The code is on [GitHub](https://github.com/mohit-k-s/meteo) \
The website is live, [Meteo](https://meteo-v1.vercel.app/)

---

*Building tools for problems you actually have is the best kind of side project. Even if the UI looks like it was designed in 2005.*
