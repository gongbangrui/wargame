# Standalone Server Release

Release hosting is external to this repository. Obtain the archive and its
checksum sidecar from your approved release channel; this package does not
embed a release host. Verify the archive before extracting it:

```sh
archive="wargame-server-<version>.tar.gz"
sha256sum -c "$archive.sha256"
release_dir="$HOME/wargame-release"
mkdir -p "$release_dir"
tar -tzf "$archive" >/dev/null
if tar -tzf "$archive" | grep -Eq '(^/|(^|/)\.\.(/|$))'; then
  printf '%s\n' 'archive contains an unsafe absolute or parent path' >&2
  exit 1
fi
tar -xzf "$archive" -C "$release_dir" --no-same-owner --no-same-permissions
cd "$release_dir/wargame-server-<version>"
```

The package contains the source and Docker files for the desktop protocol
client contract, `account-web`, and the authority `game-server`; the installer
builds both server services locally. The included `deploy/release-manifest.env`
locks the package to protocol v6/schema 6 and the WebSocket authoritative data
plane.

The game-server image disables the optional Fast DDS adapter by default. The
installer persists `WARGAME_ENABLE_FASTDDS`, `WARGAME_FASTDDS_MODE`,
`FASTDDS_DOMAIN_ID`, `FASTDDS_DISCOVERY_TIMEOUT_MS`, and `FASTDDS_STATIC_PEERS`
in the managed `.env`; compatibility mode is rejected until authenticated and
encrypted DDS transport is deployed. WebSocket remains the only authoritative
snapshot/delta/command channel.

Run the installer as the deployment user (or through `sudo`); it prompts for
the administrator password when one is not supplied. The default managed root
is the invoking user's `~/wargame` directory. Use an explicit root and stable
Compose project when an isolated deployment is needed:

```sh
sudo ./deploy/install-server.sh \
  --install-dir "$HOME/wargame" \
  --compose-project wargame
```

The runtime layout keeps the replaceable source under `current`, configuration
in `.env`, recovery copies in `backups`, and a managed-root marker in
`.wargame-install`. The named Docker data volume is retained across normal
installs and updates. Ordinary updates and uninstall preserve the runtime
`.env`, `backups/`, and named Docker data volume.

To update, verify and extract the newer release, then run its installer with
the same install root and Compose project. Existing configuration and
persistent volume data are preserved:

```sh
sudo ./deploy/install-server.sh \
  --install-dir "$HOME/wargame" \
  --compose-project wargame
```

When the existing images are sufficient and only container recreation is
needed, include `--no-build` in the same command.

To force a clean rebuild after applying a source update, include `--no-cache`.
This rebuilds both server images with the same staged source digest; do not
rebuild only one service when changing protocol or projection code.

Inspect the installed configuration without printing credentials, validate the
Compose file, and check service health from any directory:

```sh
install_root="$HOME/wargame"
sudo grep -E '^(WARGAME_COMPOSE_PROJECT|WARGAME_DATA_VOLUME|WARGAME_RUNTIME_ENV_FILE|HTTP_PORT|WS_PORT)=' \
  "$install_root/.env"
sudo docker compose \
  --project-name wargame \
  --env-file "$install_root/.env" \
  -f "$install_root/current/deploy/compose.yml" ps
sudo docker compose \
  --project-name wargame \
  --env-file "$install_root/.env" \
  -f "$install_root/current/deploy/compose.yml" config --quiet
curl -fsS http://127.0.0.1:8080/api/health
```

Use the installed lifecycle helpers for routine operations:

```sh
install_root="$HOME/wargame"
sudo "$install_root/current/deploy/reset-room.sh" --yes
sudo "$install_root/current/deploy/reset-admin.sh"
sudo "$install_root/current/deploy/uninstall-server.sh"
```

The room reset requires `--yes` and creates a backup before clearing transient
room state. The normal uninstall above is safe: it stops the services while
preserving `.env`, `backups/`, and the named volume. Destructive cleanup is
opt-in and must be explicit, for example:

```sh
sudo "$HOME/wargame/current/deploy/uninstall-server.sh" \
  --purge-data --remove-config --yes
```

`--purge-data` permanently removes the named Docker data volume and
`--remove-config` permanently removes the runtime `.env`; use them only after
an intentional backup and confirmation of the target installation.
