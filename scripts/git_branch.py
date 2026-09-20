"""
PlatformIO pre-build script: inject the base version and short SHA into
CROSSPOINT_VERSION for the default (dev) environment.

Results in a version string like:  1.6.0-05c6cf8

The base version is the HIGHER of [crosspoint] version in platformio.ini and the
latest non-rc release tag reachable from upstream's develop. Deriving it keeps dev
builds from under-reporting on a long-lived fork: OtaUpdater polls the UPSTREAM
release feed and compares semver, so a stale number makes the device offer an OTA
that would flash upstream firmware over the fork.

Release environments are unaffected; they set CROSSPOINT_VERSION from the ini, so a
release still needs the ini bumped by hand.
"""

import configparser
import os
import subprocess
import sys


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def get_ini_version(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    if not os.path.isfile(ini_path):
        warn(f'platformio.ini not found at {ini_path}; base version will be "0.0.0"')
        return '0.0.0'
    config = configparser.ConfigParser()
    config.read(ini_path, encoding='utf-8')
    if not config.has_option('crosspoint', 'version'):
        warn('No [crosspoint] version in platformio.ini; base version will be "0.0.0"')
        return '0.0.0'
    return config.get('crosspoint', 'version')


def parse_semver(value):
    """Leading MAJOR.MINOR.PATCH as a tuple, or None if absent.

    Mirrors OtaUpdater::isUpdateNewer(), which sscanf's "%d.%d.%d" off the front
    and ignores any suffix.
    """
    parts = value.lstrip('v').split('.')[:3]
    if len(parts) < 3:
        return None
    out = []
    for part in parts:
        digits = ''
        for c in part:
            if not c.isdigit():
                break
            digits += c
        if not digits:
            return None
        out.append(int(digits))
    return tuple(out)


def get_upstream_release_version(project_dir):
    """Latest non-rc release tag reachable from upstream's develop, or None.

    --match keeps junk tags (sd-fonts, _pre_*) out; --exclude drops release
    candidates. A missing remote is normal (fresh clone, CI on the upstream repo
    itself), so this falls through quietly rather than warning.
    """
    for ref in ('upstream/develop', 'origin/develop'):
        try:
            value = subprocess.check_output(
                ['git', 'describe', '--tags', '--abbrev=0',
                 '--match=[0-9]*.[0-9]*.[0-9]*', '--exclude=*rc*', ref],
                text=True, stderr=subprocess.DEVNULL, cwd=project_dir
            ).strip()
        except (OSError, subprocess.CalledProcessError):
            continue
        if parse_semver(value):
            return value.lstrip('v')
    return None


def get_base_version(project_dir):
    ini_version = get_ini_version(project_dir)
    tag_version = get_upstream_release_version(project_dir)
    if not tag_version:
        return ini_version

    ini_parsed = parse_semver(ini_version)
    tag_parsed = parse_semver(tag_version)
    if not ini_parsed or tag_parsed <= ini_parsed:
        return ini_version

    # Upstream released past the ini. Dev builds follow it now; release builds read
    # the ini directly, so say so rather than letting them silently disagree.
    print(
        f'NOTE [git_branch.py]: upstream release tag {tag_version} is newer than '
        f'[crosspoint] version {ini_version} in platformio.ini; using {tag_version} '
        f'for this dev build. Bump the ini before cutting a release.'
    )
    return tag_version


def inject_version(env):
    # Applies to the dev environments (default + single-diagnostic variants);
    # release envs set the version via build_flags in platformio.ini and are
    # unaffected.
    if env['PIOENV'] not in ('default', 'oomtrace', 'memtrace', 'sticky', 'x4pro'):
        return

    project_dir = env['PROJECT_DIR']
    base_version = get_base_version(project_dir)
    short_sha = get_git_short_sha(project_dir)
    version_string = f'{base_version}-{short_sha}'

    env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', f'\\"{version_string}\\"')])
    print(f'CrossPoint build version: {version_string}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')           # noqa: F821  # type: ignore[name-defined]
    inject_version(env)     # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_): pass

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
