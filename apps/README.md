# apps: the app catalog

Every folder here is a Lua app for the board, and together they are the **app catalog**:
on every push to `main`, GitHub Actions checks them and publishes them to GitHub Pages
(`https://maclogan.github.io/utilreader/index.json`), where the board's web page finds them
(**Transfer** app → **App catalog**). Weather and Clock are also built into the firmware.

How to write an app: [docs/api.md](../docs/api.md) (start with the quick start).

## Adding your app

1. Fork this repository.
2. Add a folder `apps/<id>/`, named exactly like the `id` in your `app.json`:

   ```
   apps/myapp/
     app.json     required: id, name, version, api, description (author recommended)
     main.lua     required
     icon.png     required for the catalog (32x32, black and white)
     README.md    optional: what the app does, its settings
     *.lua        optional: modules main.lua loads with require()
   ```

3. Check it: `python tools/catalog.py check` (install Lua 5.4 so `luac` can check the
   syntax too). Fix anything it reports.
4. Test it on a board: install the folder from the board's web page.
5. Open a pull request. The **App catalog** workflow runs the same check.

## Updating your app

Change the files and **raise `version`** in `app.json` (e.g. `1.0.0` to `1.1.0`): the
check fails on a pull request that changes an app but keeps its version. After the merge,
boards with the old version show **Update**.

## What the check enforces

| Rule | Why |
|---|---|
| Folder name = `id`: 1-24 of `a-z`, `0-9`, `_` | It's the app's folder on the board |
| `name` up to 24 characters (about 9 fit under a menu tile) | Menu label |
| `version` like `1.2.0`, higher than before when files change | Boards decide about updates by it |
| `api` no higher than the firmware's (`APP_SCRIPT_API_VERSION`) | Older boards refuse newer apps |
| A one-line `description` | Shown in the catalog |
| Settings well formed; a `secret` has no default | A default would be public |
| File names: letters, digits, `.` `_` `-`, up to 32; no subfolders | The board's limits |
| Each file up to 256 KB, the app up to 1 MB | The apps partition is 2 MB in all |
| `main.lua` and an icon; every `.lua` compiles | It has to run |

## Running a catalog of your own

`python tools/catalog.py build <folder>` turns an `apps/` folder into a catalog (zips,
`index.json`, a small `index.html`). Host the folder anywhere that serves files over HTTPS,
and paste the address of its `index.json` into the board's **App catalog** page.
