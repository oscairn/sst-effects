#!/usr/bin/env python3
"""Versions read from this repo's source (no duplicated constant).

descriptor(file, id) reads clap_plugin_descriptor_t(s) out of one plugin's source
file; each plugin's own version lives there. A file may hold several descriptors
(one bundle of plugins), so id picks one by plugin id; without id the first is used.
project_version() reads the package version from the project() line named by
PROJECT_VERSION_FILE in release.conf. Used by manifest.py (to cross-check the tag)
and by the release workflow (a dry run has no tag, so it must derive one from the
package version).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from release_conf import REPO, conf

DESCRIPTOR = re.compile(
    r'CLAP_VERSION(?:_INIT)?,\s*'
    r'"(?P<id>[^"]*)",\s*"(?P<name>[^"]*)",\s*"(?P<vendor>[^"]*)",\s*"(?P<url>[^"]*)",\s*'
    r'"[^"]*",\s*"[^"]*",\s*"(?P<version>[^"]*)"')

PROJECT = re.compile(r'project\([\w-]+ VERSION ([0-9]+\.[0-9]+\.[0-9]+)')


def descriptor(path, plugin_id=None):
    text = open(os.path.join(REPO, path), encoding='utf-8').read()
    found = DESCRIPTOR.findall(text)
    if not found:
        sys.exit(f'version.py: no clap_plugin_descriptor_t found in {path}')
    if plugin_id is None:
        return dict(zip(('id', 'name', 'vendor', 'url', 'version'), found[0]))
    for row in found:
        if row[0] == plugin_id:
            return dict(zip(('id', 'name', 'vendor', 'url', 'version'), row))
    sys.exit(f'version.py: no descriptor for plugin {plugin_id!r} in {path} '
             f'(found: {", ".join(r[0] for r in found)})')


def project_version():
    f = conf()['PROJECT_VERSION_FILE']
    m = PROJECT.search(open(os.path.join(REPO, f), encoding='utf-8').read())
    if not m:
        sys.exit(f'version.py: no project(<name> VERSION X.Y.Z) found in {f}')
    return m.group(1)


if __name__ == '__main__':
    print(project_version())
