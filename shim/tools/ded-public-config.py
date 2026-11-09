#!/usr/bin/env python3
"""A PUBLIC config for our native dedicated server (2026-11-09; the user: "start a big team battle match with one walker, and me, and make it
public, set the size to 127 ... pick sand trap, and fiesta oddball ... vehicles enabled too all weapons all powerups all grenade types ...
call the public game "walker texas rangers only!", and set the description to experimental walker test"). Writes, in the test server's
folder (ReclaimerMac/native/test-dedicated), <slug>.toml and playlists/<slug>.playlist.json -- a one-game playlist -- for
native-dedicated.sh (DED_CONFIG=<slug>.toml). Unlike local-test.toml it is LISTED: hidden = false (the project's public masters list it),
bind "::", UPnP on (the router forwards the port), public_address automatic. Kept from the local one: the RCON password and RCON on
127.0.0.1 only, anti-cheat enforce. Stats reporting stays OFF (a walker's games must not count in the community's stats). Every joining
player reads the welcome line saying an automated test player is in the game. host_name is the line under the name in the browser.
    python3 shim/tools/ded-public-config.py --name "walker texas rangers only!" --description "experimental walker test" \\
        --map Sandtrap --game Oddball --title "Fiesta Oddball" --max 127 \\
        --options '{"rounds.teams": 1, "player.primary_weapon": -3, "player.secondary_weapon": -3}' --walkers Second
The option keys and values: shim/docs/option-map.json (-3 = Random)."""
import argparse, json, os, re, sys

D = os.path.expanduser('~/Library/Application Support/ReclaimerMac/native/test-dedicated')

def q(s): return json.dumps(s)   # a TOML basic string (JSON escaping is valid TOML for these)

def main():
    a = argparse.ArgumentParser()
    a.add_argument('--name', required=True); a.add_argument('--description', required=True)
    a.add_argument('--map', required=True); a.add_argument('--game', required=True); a.add_argument('--title', default='')
    a.add_argument('--max', type=int, default=127); a.add_argument('--options', default='{}'); a.add_argument('--walkers', default='Second')
    a.add_argument('--port', type=int, default=49190)
    o = a.parse_args()
    opts = json.loads(o.options)
    if o.max < 2 or o.max > 127: sys.exit('--max: 2..127 (the server shares 127 places)')
    slug = 'public-' + re.sub(r'[^a-z0-9]+', '-', o.name.lower()).strip('-')
    local = open(os.path.join(D, 'local-test.toml')).read()
    game = re.search(r'^game = (".*")$', local, re.M).group(1)
    pw = open(os.path.join(D, 'rcon-password.txt')).read().strip()
    pv = 1
    try: pv = json.load(open(os.path.join(D, 'playlists', 'slayer.playlist.json'))).get('version', 1)
    except Exception: pass
    os.makedirs(os.path.join(D, 'playlists'), exist_ok=True)
    json.dump({'version': pv, 'name': slug, 'games': [{'map': o.map, 'game': o.game, 'name': o.title or o.game, 'options': opts}]},
              open(os.path.join(D, 'playlists', slug + '.playlist.json'), 'w'))
    walkers = ', '.join(w.strip() for w in o.walkers.split(',') if w.strip())
    welcome = 'Welcome to {server}, {name}! Experimental walker test: %s %s an automated test player (a bot) learning to play.' % (
        walkers, 'is' if ',' not in o.walkers else 'are')
    cfg = '''# PUBLIC walker test server (shim/tools/ded-public-config.py): listed on the project's masters; stats reporting off.
game = %s
data = "data"
bans = "bans.json"
public_address = ""
bind_address = "::"
upnp = true
auto_reload = true
auto_update = false
host_name = %s
admins = []
stats_server = "off"
[master]
# off: this machine's master (behind carrier-grade NAT nobody reaches it, and it kept the server's registration to itself: "registered
# with the server list at 127.0.0.1:49175") -- the game socket registers with the project's public masters, which list it and introduce /
# relay players through the NAT
enabled = false
port = 49175
peers = []
[rcon]
password = %s
address = "127.0.0.1"
[defaults]
hidden = false
password = ""
max_players = %d
min_players = 1
post_game_seconds = 15
anti_cheat = "enforce"
welcome = %s
[[server]]
name = %s
port = %d
playlist = "playlists/%s.playlist.json"
''' % (game, q(o.description), q(pw), o.max, q(welcome), q(o.name), o.port, slug)
    p = os.path.join(D, slug + '.toml'); open(p, 'w').write(cfg); os.chmod(p, 0o600)
    print('%s.toml: %s -- %s on %s (%s), %d players, options %s' % (slug, o.name, o.title or o.game, o.map, o.description, o.max, json.dumps(opts)))
    return 0

if __name__ == '__main__':
    sys.exit(main())
