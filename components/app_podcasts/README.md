# app_podcasts: Podcasts app

Plays podcast episodes downloaded to the microSD card, with the same screens as Music (the
shared `components/player`). Feeds and their rules are managed on the web page (**Transfer**
app, **Podcasts** section); see `components/podcasts/README.md`.

```
Podcasts
  Now Playing          (while an episode plays)
  <podcast>      3  >  (unplayed episodes on the card)
     <episode>   new / 25m left / started / played
  New episodes: open Transfer   (or the last update's problem)
```

| Where | Left / right | Hold left / right | Press | Hold press | BACK |
|---|---|---|---|---|---|
| Lists | Move | A page | Open / play | Episode: mark played / unplayed (else Now Playing) | Up (from the top: leave) |
| Now Playing, VOL | Volume | Previous / next episode | Play / pause | Switch to SEEK | Back to the list |
| Now Playing, SEEK | Back 10 s / ahead 30 s | Previous / next episode | Play / pause | Switch to VOL | Back to the list |

- **New episodes come in while the Transfer app is open**: it checks every feed and downloads
  what the rules want, showing progress on its screen, and stops when you leave it (a partial
  download resumes next time). So downloads never compete with playback. Opening Podcasts
  only tidies up by the keep rules (deleting played episodes). Playback carries on when you
  leave the app.
- **An episode** starts a few seconds before where you stopped. Its position is saved on
  pause, on skips, every 2 minutes, when leaving the app and at power-off.
- **At the end** it's marked played; with "play the next unplayed" (the default) the next
  unplayed episode in the feed's order starts.
- The seek steps (10 s back, 30 s ahead) and the volume (shared with Music) are settings.
- Now Playing shows the episode, the podcast and the release date.
