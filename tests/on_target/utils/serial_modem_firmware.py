# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
"""Download and flash Serial Modem firmware for nRF54 host hardware tests."""

from __future__ import annotations

import json
import os
import time
import zipfile
from collections.abc import Callable
from pathlib import Path
from typing import TypeVar
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen, urlretrieve

import yaml

from utils.flash_tools import FULL_FLASH_PROGRAM_OPTIONS, flash_firmware_hex
from utils.helpers import REPO_ROOT
from utils.logger import get_logger

logger = get_logger()

GITHUB_API = "https://api.github.com"

NETWORK_TIMEOUT_SECONDS = 30
NETWORK_ATTEMPTS = 4
NETWORK_BACKOFF_SECONDS = 5.0

_T = TypeVar("_T")

# A single test flashes the modem, reads the console baud rate, and may extract
# the bundle, each of which needs the resolved release. Without this the same
# lookup runs three or four times per test and burns the API quota.
_release_cache: dict[tuple[str, str, str | None], dict] = {}


def clear_serial_modem_release_cache() -> None:
    _release_cache.clear()


def load_serial_modem_static_config(root: Path | None = None) -> dict:
    repo_root = root or REPO_ROOT
    config_path = repo_root / "tests/on_target/ci/serial_modem_firmware.yml"
    return yaml.safe_load(config_path.read_text(encoding="utf-8"))


def _github_token() -> str | None:
    for name in ("GITHUB_TOKEN", "GH_TOKEN"):
        value = os.environ.get(name, "").strip()
        if value:
            return value
    return None


def _is_transient(exc: OSError) -> bool:
    if isinstance(exc, HTTPError):
        return exc.code >= 500
    return isinstance(exc, (URLError, TimeoutError))


def _with_network_retry(description: str, operation: Callable[[], _T]) -> _T:
    """Run *operation*, retrying DNS, connection and 5xx failures with backoff."""
    delay = NETWORK_BACKOFF_SECONDS
    for attempt in range(1, NETWORK_ATTEMPTS + 1):
        try:
            return operation()
        except (URLError, TimeoutError) as exc:
            if not _is_transient(exc) or attempt == NETWORK_ATTEMPTS:
                raise
            logger.warning(
                "%s failed (%s), retrying in %.0fs (%d/%d)",
                description,
                exc,
                delay,
                attempt,
                NETWORK_ATTEMPTS,
            )
            time.sleep(delay)
            delay *= 2
    raise AssertionError("unreachable")


def _github_api_request(url: str) -> object:
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    token = _github_token()
    if token:
        headers["Authorization"] = f"Bearer {token}"

    request = Request(url, headers=headers)

    def fetch() -> object:
        with urlopen(request, timeout=NETWORK_TIMEOUT_SECONDS) as response:
            return json.load(response)

    try:
        return _with_network_retry(f"GitHub API request {url}", fetch)
    except HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"GitHub API request failed ({exc.code}): {body}") from exc


def _asset_suffix(static: dict) -> str:
    suffix = static.get("asset_suffix")
    if suffix:
        return str(suffix)
    raise RuntimeError("serial_modem_firmware.yml must set asset_suffix")


def _release_config_from_payload(
    release: dict,
    *,
    asset_suffix: str,
) -> dict:
    tag = release["tag_name"]
    for asset in release.get("assets", []):
        name = asset["name"]
        if name.endswith(asset_suffix):
            return {
                "release": tag,
                "bundle": name,
                "hex": f"{Path(name).stem}.hex",
                "download_url": asset["browser_download_url"],
                "upstream_release_url": release["html_url"],
            }

    raise RuntimeError(
        f"Serial Modem release {tag!r} has no asset ending with {asset_suffix!r}"
    )


def resolve_serial_modem_release(
    tag: str | None = None,
    *,
    root: Path | None = None,
) -> dict:
    """Resolve a Serial Modem release from GitHub.

    When *tag* is set, fetch that release. Otherwise walk upstream releases
    newest-first and return the first that ships the nrf91m1 bundle.
    """
    static = load_serial_modem_static_config(root)
    repo = static["upstream_repo"]
    asset_suffix = _asset_suffix(static)

    cache_key = (repo, asset_suffix, tag)
    if cache_key in _release_cache:
        return dict(_release_cache[cache_key])

    resolved = _resolve_uncached(repo, asset_suffix, tag)
    _release_cache[cache_key] = resolved
    return dict(resolved)


def _resolve_uncached(
    repo: str,
    asset_suffix: str,
    tag: str | None,
) -> dict:
    if tag:
        release = _github_api_request(f"{GITHUB_API}/repos/{repo}/releases/tags/{tag}")
        return _release_config_from_payload(release, asset_suffix=asset_suffix)

    releases = _github_api_request(f"{GITHUB_API}/repos/{repo}/releases")
    if not isinstance(releases, list):
        raise RuntimeError(f"Unexpected GitHub API response for {repo} releases")

    for release in releases:
        try:
            return _release_config_from_payload(release, asset_suffix=asset_suffix)
        except RuntimeError:
            continue

    raise RuntimeError(
        f"No Serial Modem release in {repo!r} ships an asset ending with "
        f"{asset_suffix!r}"
    )


def _pinned_release_tag(static: dict) -> str | None:
    pinned = os.environ.get("SERIAL_MODEM_RELEASE", "").strip()
    if pinned:
        return pinned
    yaml_pin = static.get("pinned_release")
    if yaml_pin:
        return str(yaml_pin).strip() or None
    return None


def load_serial_modem_firmware_config(root: Path | None = None) -> dict:
    static = load_serial_modem_static_config(root)
    resolved = resolve_serial_modem_release(_pinned_release_tag(static), root=root)
    return {**static, **resolved}


def serial_modem_console_baudrate(root: Path | None = None) -> int:
    """Baud rate of the Serial Modem console (uart1), not the usual 115200."""
    config = load_serial_modem_firmware_config(root)
    return int(config["console_baudrate"])


def serial_modem_cache_dir(config: dict, *, root: Path | None = None) -> Path:
    repo_root = root or REPO_ROOT
    return repo_root / "build" / "serial-modem-firmware" / config["release"]


def download_serial_modem_bundle(*, root: Path | None = None) -> Path:
    """Download the resolved Serial Modem release zip if needed, and return its path.

    Releases ship this archive as it comes from upstream; tests extract it.
    """
    repo_root = root or REPO_ROOT
    config = load_serial_modem_firmware_config(repo_root)
    cache_dir = serial_modem_cache_dir(config, root=repo_root)
    zip_path = cache_dir / config["bundle"]

    if not zip_path.is_file():
        cache_dir.mkdir(parents=True, exist_ok=True)
        logger.info(
            "Downloading Serial Modem firmware %s (%s)",
            config["release"],
            config["download_url"],
        )
        partial_path = zip_path.with_name(f"{zip_path.name}.part")
        _with_network_retry(
            f"Serial Modem firmware download {config['download_url']}",
            lambda: urlretrieve(config["download_url"], partial_path),
        )
        partial_path.replace(zip_path)

    return zip_path


def ensure_serial_modem_firmware(*, root: Path | None = None) -> Path:
    """Download and extract the resolved Serial Modem bundle if needed."""
    repo_root = root or REPO_ROOT
    config = load_serial_modem_firmware_config(repo_root)
    cache_dir = serial_modem_cache_dir(config, root=repo_root)
    hex_path = cache_dir / config["hex"]

    if hex_path.is_file():
        return hex_path

    zip_path = download_serial_modem_bundle(root=repo_root)
    with zipfile.ZipFile(zip_path) as archive:
        archive.extractall(cache_dir)

    if not hex_path.is_file():
        raise FileNotFoundError(
            f"Serial Modem hex not found after extracting {zip_path}: {hex_path}"
        )

    return hex_path


def flash_serial_modem_firmware(segger_sn: str, *, root: Path | None = None) -> None:
    """Program the resolved Serial Modem release on the nRF9151 / SMA DK."""
    config = load_serial_modem_firmware_config(root)
    hex_path = ensure_serial_modem_firmware(root=root)
    logger.info(
        "Flashing Serial Modem firmware %s (%s) to %s",
        config["release"],
        hex_path.name,
        segger_sn,
    )
    flash_firmware_hex(
        hex_path,
        segger_sn,
        recover=True,
        program_options=FULL_FLASH_PROGRAM_OPTIONS,
    )
