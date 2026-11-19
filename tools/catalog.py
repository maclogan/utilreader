"""
The app catalog: checks the apps in apps/ and builds what GitHub Pages publishes.

    python tools/catalog.py check [--base REF] [--require-luac]
    python tools/catalog.py build OUT_DIR

check   Every apps/<id>/ must be an installable package: a valid app.json (the same rules the
        board applies, plus a few stricter ones), allowed file names and sizes, main.lua and
        an icon, and Lua that compiles (with luac, if installed; --require-luac makes a
        missing luac an error, as in CI). With --base, every app whose files changed since
        REF (a git ref) must also have a higher version than it had there.

build   Writes OUT_DIR/index.json, OUT_DIR/apps/<id>-<version>.zip for each app, and a
        small OUT_DIR/index.html for people. The board's web page reads index.json:

        {"format": 1, "generated": "...", "apps": [{"id", "name", "version", "api", "author",
         "description", "background", "url" (relative to index.json), "sha256", "size",
         "icon" (a data: URL of the PNG)}]}

No dependencies beyond Python 3.8+ (and luac for the syntax check).
"""
import argparse
import base64
import datetime
import hashlib
import html
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPS = os.path.join(ROOT, 'apps')
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import app_icon  # noqa: E402  (icon.bin -> PNG for apps without an icon.png)

APP_ID = re.compile(r'^[a-z0-9_]{1,24}$')
APP_FILE = re.compile(r'^[A-Za-z0-9_-][A-Za-z0-9._-]{0,31}$')
VERSION = re.compile(r'^\d+(\.\d+){0,3}$')
FILE_MAX = 256 * 1024                   # per file (APP_SCRIPT_FILE_MAX on the board)
APP_MAX = 1024 * 1024                   # a whole app: the apps partition is 2 MB in total
SETTING_TYPES = {'bool', 'int', 'string', 'secret', 'choice'}
SKIP = {'icon.png', 'README.md', 'readme.md', 'LICENSE'}   # kept in the zip; the board ignores them


def board_api():
    """APP_SCRIPT_API_VERSION from the firmware."""
    header = open(os.path.join(ROOT, 'components/app_script/include/app_script.h'), encoding='utf-8').read()
    return int(re.search(r'#define APP_SCRIPT_API_VERSION\s+(\d+)', header).group(1))


def version_key(v):
    return [int(x) for x in str(v).split('.')]


def find_luac():
    for name in filter(None, [os.environ.get('LUAC'), 'luac5.4', 'luac']):
        path = shutil.which(name)
        if path:
            return path
    return None


def check_settings(settings, errors):
    if not isinstance(settings, list):
        errors.append('"settings" must be a list')
        return
    keys = set()
    for i, s in enumerate(settings):
        where = f'settings[{i}]'
        if not isinstance(s, dict):
            errors.append(f'{where} must be an object')
            continue
        key, kind = s.get('key'), s.get('type', 'string')
        if not isinstance(key, str) or not key:
            errors.append(f'{where} needs a "key"')
        elif key in keys:
            errors.append(f'{where}: key "{key}" used twice')
        keys.add(key)
        if kind not in SETTING_TYPES:
            errors.append(f'{where}: type must be one of {", ".join(sorted(SETTING_TYPES))}')
            continue
        if 'label' in s and not isinstance(s['label'], str):
            errors.append(f'{where}: "label" must be text')
        d = s.get('default')
        if kind == 'choice':
            opts = s.get('options')
            values = [o.get('value') if isinstance(o, dict) else o for o in opts] if isinstance(opts, list) else None
            if not values or not all(isinstance(v, str) for v in values):
                errors.append(f'{where}: a choice needs "options" (strings, or {{"value", "label"}} objects)')
            elif d is not None and d not in values:
                errors.append(f'{where}: default "{d}" isn\'t one of the options')
        elif kind == 'int':
            for k in ('min', 'max', 'default'):
                if k in s and not isinstance(s[k], int):
                    errors.append(f'{where}: "{k}" must be a whole number')
        elif kind == 'bool' and d is not None and not isinstance(d, bool):
            errors.append(f'{where}: default must be true or false')
        elif kind in ('string', 'secret') and d is not None and not isinstance(d, str):
            errors.append(f'{where}: default must be text')
        if kind == 'secret' and d:
            errors.append(f'{where}: a secret must not have a default (it would be public)')


def check_app(app_id, luac, api_max):
    """Returns (manifest or None, errors, warnings) for apps/<app_id>."""
    errors, warnings = [], []
    folder = os.path.join(APPS, app_id)
    if not APP_ID.match(app_id):
        return None, [f'folder name must be 1-24 of a-z, 0-9, _'], warnings
    try:
        m = json.load(open(os.path.join(folder, 'app.json'), encoding='utf-8'))
    except FileNotFoundError:
        return None, ['no app.json'], warnings
    except ValueError as e:
        return None, [f'app.json isn\'t valid JSON: {e}'], warnings
    if not isinstance(m, dict):
        return None, ['app.json must be an object'], warnings

    if m.get('id') != app_id:
        errors.append(f'"id" must be "{app_id}" (the folder name)')
    name = m.get('name')
    if not isinstance(name, str) or not name.strip():
        errors.append('"name" is missing')
    elif len(name) > 24:
        errors.append('"name" over 24 characters (it labels a menu tile)')
    elif len(name) > 9:
        warnings.append(f'name "{name}" is long: about 9 characters fit under a menu tile')
    if not isinstance(m.get('version'), str) or not VERSION.match(m['version']):
        errors.append('"version" must look like 1.0.0')
    api = m.get('api')
    if not isinstance(api, int) or isinstance(api, bool) or api < 1:
        errors.append('"api" must be a whole number, 1 or more')
    elif api > api_max:
        errors.append(f'"api" {api} is newer than the firmware in this repository ({api_max})')
    if not isinstance(m.get('description', ''), str) or not m.get('description'):
        errors.append('"description" is missing (one line about the app, shown in the catalog)')
    if not isinstance(m.get('author', ''), str):
        errors.append('"author" must be text')
    if 'background' in m and not isinstance(m['background'], bool):
        errors.append('"background" must be true or false')
    if 'settings' in m:
        check_settings(m['settings'], errors)

    total = 0
    for entry in sorted(os.listdir(folder)):
        path = os.path.join(folder, entry)
        if os.path.isdir(path):
            errors.append(f'"{entry}/": packages have no subfolders')
            continue
        size = os.path.getsize(path)
        total += size
        if not APP_FILE.match(entry):
            errors.append(f'file name "{entry}": letters, digits, . _ - only, up to 32, no leading dot')
        if size > FILE_MAX:
            errors.append(f'"{entry}" is {size // 1024} KB; the limit is {FILE_MAX // 1024} KB per file')
        if entry.endswith('.lua') and luac:
            r = subprocess.run([luac, '-p', path], capture_output=True, text=True)
            if r.returncode != 0:
                errors.append(f'{entry}: ' + (r.stderr.strip() or 'doesn\'t compile').replace(path, entry))
    if total > APP_MAX:
        errors.append(f'the app is {total // 1024} KB in all; keep it under {APP_MAX // 1024} KB')
    if not os.path.isfile(os.path.join(folder, 'main.lua')):
        errors.append('no main.lua')
    icon_bin = os.path.join(folder, 'icon.bin')
    if os.path.isfile(icon_bin) and os.path.getsize(icon_bin) != 128:
        errors.append('icon.bin must be 128 bytes (32x32, 1 bit per pixel); or leave it out and ship icon.png')
    if not os.path.isfile(os.path.join(folder, 'icon.png')) and not os.path.isfile(icon_bin):
        errors.append('no icon: add icon.png (32x32 black and white looks best)')
    return m, errors, warnings


def changed_since(base):
    """App ids with changes since git ref `base`, and that ref's app.json versions. None if `base` isn't usable."""
    if not base or set(base) == {'0'}:
        return None
    try:
        subprocess.run(['git', 'rev-parse', '--verify', base + '^{commit}'], cwd=ROOT, check=True, capture_output=True)
        out = subprocess.run(['git', 'diff', '--name-only', base, '--', 'apps/'], cwd=ROOT, check=True,
                             capture_output=True, text=True).stdout
    except subprocess.CalledProcessError:
        return None
    ids = {p.split('/')[1] for p in out.splitlines() if p.count('/') >= 2}
    old = {}
    for app_id in ids:
        r = subprocess.run(['git', 'show', f'{base}:apps/{app_id}/app.json'], cwd=ROOT, capture_output=True, text=True)
        if r.returncode == 0:
            try:
                old[app_id] = json.loads(r.stdout).get('version')
            except ValueError:
                pass
    return ids, old


def cmd_check(args):
    luac = find_luac()
    if not luac:
        if args.require_luac:
            print('error: luac not found (install Lua 5.4, or set LUAC)')
            return 1
        print('note: luac not found, so Lua syntax isn\'t checked')
    api_max = board_api()
    failed = 0
    ids = sorted(d for d in os.listdir(APPS) if os.path.isdir(os.path.join(APPS, d)))
    changes = changed_since(args.base) if args.base else None
    if args.base and changes is None:
        print(f'note: can\'t compare with "{args.base}", so version bumps aren\'t checked')
    for app_id in ids:
        m, errors, warnings = check_app(app_id, luac, api_max)
        if changes and app_id in changes[0] and app_id in changes[1] and m and isinstance(m.get('version'), str) \
                and VERSION.match(m['version']) and changes[1][app_id] and VERSION.match(str(changes[1][app_id])) \
                and version_key(m['version']) <= version_key(changes[1][app_id]):
            errors.append(f'files changed but the version is still {m["version"]} (was {changes[1][app_id]}): raise it')
        status = 'FAIL' if errors else 'ok  '
        print(f'{status} {app_id}' + (f' {m.get("version")}' if m else ''))
        for e in errors:
            print(f'       error: {e}')
        for w in warnings:
            print(f'       warning: {w}')
        failed += bool(errors)
    print(f'\n{len(ids)} apps, {failed} with errors')
    return 1 if failed else 0


def zip_app(app_id):
    """The app's folder as a zip (files under "<id>/"), byte-for-byte reproducible."""
    buf = io.BytesIO()
    folder = os.path.join(APPS, app_id)
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for entry in sorted(os.listdir(folder)):
            info = zipfile.ZipInfo(f'{app_id}/{entry}', date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, open(os.path.join(folder, entry), 'rb').read())
    return buf.getvalue()


def icon_data_url(app_id):
    folder = os.path.join(APPS, app_id)
    png = os.path.join(folder, 'icon.png')
    if not os.path.isfile(png):
        with tempfile.TemporaryDirectory() as tmp:
            png_tmp = os.path.join(tmp, 'icon.png')
            app_icon.to_png(os.path.join(folder, 'icon.bin'), png_tmp)
            data = open(png_tmp, 'rb').read()
    else:
        data = open(png, 'rb').read()
    return 'data:image/png;base64,' + base64.b64encode(data).decode()


def cmd_build(args):
    if cmd_check(argparse.Namespace(base=None, require_luac=False)) != 0:
        print('not building: fix the errors above first')
        return 1
    out = args.out_dir
    os.makedirs(os.path.join(out, 'apps'), exist_ok=True)
    entries = []
    for app_id in sorted(d for d in os.listdir(APPS) if os.path.isdir(os.path.join(APPS, d))):
        m = json.load(open(os.path.join(APPS, app_id, 'app.json'), encoding='utf-8'))
        data = zip_app(app_id)
        name = f'{app_id}-{m["version"]}.zip'
        open(os.path.join(out, 'apps', name), 'wb').write(data)
        entries.append({
            'id': app_id, 'name': m['name'], 'version': m['version'], 'api': m['api'],
            'author': m.get('author', ''), 'description': m.get('description', ''),
            'background': bool(m.get('background', False)),
            'url': f'apps/{name}', 'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data),
            'icon': icon_data_url(app_id),
        })
    entries.sort(key=lambda e: e['name'].lower())
    index = {'format': 1, 'generated': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
             'apps': entries}
    with open(os.path.join(out, 'index.json'), 'w', encoding='utf-8') as f:
        json.dump(index, f, indent=1, ensure_ascii=False)
    rows = ''.join(
        f'<tr><td><img src="{e["icon"]}" width="32" height="32" alt=""></td><td><b>{html.escape(e["name"])}</b> '
        f'{html.escape(e["version"])}<br>{html.escape(e["description"])}</td>'
        f'<td><a href="{e["url"]}">download</a></td></tr>' for e in entries)
    with open(os.path.join(out, 'index.html'), 'w', encoding='utf-8') as f:
        f.write('<!doctype html><meta charset="utf-8"><title>App catalog</title>'
                '<style>body{font:15px system-ui,sans-serif;max-width:720px;margin:24px auto;padding:0 16px}'
                'td{padding:6px 8px;border-bottom:1px solid #ddd;vertical-align:top}img{image-rendering:pixelated}</style>'
                '<h1>E-paper board app catalog</h1><p>Install these from the board\'s web page: Transfer app, '
                '<b>App catalog</b>. Machine-readable list: <a href="index.json">index.json</a>.</p>'
                f'<table>{rows}</table>')
    print(f'built {len(entries)} apps into {out}')
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check', help='check every app in apps/')
    c.add_argument('--base', help='git ref to compare versions with')
    c.add_argument('--require-luac', action='store_true', help='fail if luac is missing')
    b = sub.add_parser('build', help='build the catalog into a folder')
    b.add_argument('out_dir')
    args = p.parse_args()
    return cmd_check(args) if args.cmd == 'check' else cmd_build(args)


if __name__ == '__main__':
    sys.exit(main())
