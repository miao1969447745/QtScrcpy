import os
import re
import subprocess
import sys


def get_latest_version():
    result = subprocess.run(
        ['git', 'describe', '--tags', '--abbrev=0'],
        capture_output=True,
        text=True,
        check=False,
    )
    tag = result.stdout.strip()
    if tag.startswith('v'):
        tag = tag[1:]
    if re.fullmatch(r'\d+(?:\.\d+){1,3}', tag):
        return tag
    # GitHub creates forks without tags when "Copy the default branch only" is
    # selected. Keep CMake and the Windows resource compiler buildable there.
    return '0.0.0'

if __name__ == '__main__':
    version = get_latest_version()
    version_file = os.path.abspath(os.path.join(os.path.dirname(__file__), "../QtScrcpy/appversion"))
    with open(version_file, 'w') as file:
        file.write(version + '\n')
    sys.exit(0)
