# SPDX-License-Identifier: BSD-3-Clause
"""Check SPDX headers in project-owned source and build files."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = "BSD-3-Clause"


def valid_header(text):
    identifiers = re.findall(
        r"^(?:/\*|#) SPDX-License-Identifier:[ \t]*([^\r\n*]+)", text, re.MULTILINE
    )
    return [value.strip() for value in identifiers] == [EXPECTED]


def main():
    files = [
        ROOT / name for name in ("Makefile", "Makefile.uk", "Config.uk", "Kraftfile")
    ]
    for directory in ("src", "include", "tests", "scripts", "samples", "tools"):
        files.extend(
            path
            for path in (ROOT / directory).rglob("*")
            if path.is_file()
            and (
                path.suffix in (".c", ".h", ".py", ".sh", ".uk")
                or path.name == "defconfig"
            )
        )
    bad = []
    for path in sorted(files):
        if not valid_header(path.read_text(encoding="utf-8")):
            bad.append(str(path.relative_to(ROOT)))
    patch_sources = 0
    for patch in sorted((ROOT / "patches").glob("*.patch")):
        for section in patch.read_text(encoding="utf-8").split("diff --git "):
            if "new file mode " not in section:
                continue
            path = re.search(r"^\+\+\+ b/(.*\.[ch])$", section, re.MULTILINE)
            if path is None:
                continue
            patch_sources += 1
            text = "\n".join(
                line[1:]
                for line in section.splitlines()
                if line.startswith("+") and not line.startswith("+++")
            )
            if not valid_header(text):
                bad.append(f"{patch.relative_to(ROOT)}: {path.group(1)}")
    if bad:
        print(
            "license-check: missing, duplicate, or wrong SPDX header:", file=sys.stderr
        )
        for path in bad:
            print(f"  {path}", file=sys.stderr)
        return 1
    print(
        f"license-check: {len(files)} files and {patch_sources} "
        f"new patch sources carry {EXPECTED}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
