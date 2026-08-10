#!/usr/bin/env python3
"""Concatenates wayland-scanner output into one header and one source.

The scanner emits a header and a source per protocol, two thirds of which is
doc comment. Committing that verbatim puts thousands of lines of generated
prose in the repo for no benefit, so this merges the files and strips the
comments. See tools/gen_wayland_protocols.sh, which is the only caller.

Each protocol's leading comment is its upstream copyright and license, and the
merged files are committed and redistributed, so those blocks are kept and
everything after them is stripped.

Concatenation is safe because the scanner prefixes every static with its
protocol name, so the per-protocol `*_types[]` tables cannot collide, and each
header carries its own include guard.
"""

import re
import sys

# The scanner's own includes are hoisted to the top of the merged file, since
# repeating them once per protocol is just noise.
_INCLUDE = re.compile(r'^\s*#include\s+[<"][^>"]+[>"]\s*$')


def strip_comments(text):
    """Removes C comments without touching string or character literals."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if text[i] == '\\':          # Escape, so take the pair whole
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(text[i])
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == '/' and i + 1 < n:
            if text[i + 1] == '*':
                end = text.find('*/', i + 2)
                i = n if end < 0 else end + 2
                continue
            if text[i + 1] == '/':
                end = text.find('\n', i)
                i = n if end < 0 else end
                continue
        out.append(c)
        i += 1
    return ''.join(out)


_COMMENT = re.compile(r'/\*.*?\*/', re.S)


def extract_license(text):
    """Pulls the upstream copyright out of one scanner-generated file.

    The source carries it as a plain comment. The header wraps the same text in
    a doxygen page block, so the surrounding @page lines are dropped and both
    forms come out identical.
    """
    for block in _COMMENT.findall(text):
        if 'Copyright' not in block:
            continue

        kept    = []
        started = False
        for line in block.split('\n'):
            body = line.strip().lstrip('*').strip()
            if not started:
                if not body.startswith('Copyright'):
                    continue
                started = True
            # A closing ' */' line strips down to '/' after the '*' strip
            if body in ('</pre>', '*/', '/'):
                break
            kept.append((' * ' + body).rstrip())
        while kept and not kept[-1].strip(' *'):
            kept.pop()
        return '/*\n' + '\n'.join(kept) + '\n */'
    return ''


def collapse_blank_lines(text):
    lines = [line.rstrip() for line in text.split('\n')]
    result = []
    for line in lines:
        if not line and (not result or not result[-1]):
            continue
        result.append(line)
    return '\n'.join(result).strip() + '\n'


def merge(paths, banner):
    includes = []
    licenses = []
    bodies = []
    for path in paths:
        with open(path) as handle:
            text = handle.read()
        # Protocols routinely share a copyright holder and wording
        license_text = extract_license(text)
        if license_text and license_text not in licenses:
            licenses.append(license_text)
        body = strip_comments(text)

        kept = []
        for line in body.split('\n'):
            if _INCLUDE.match(line):
                if line.strip() not in includes:
                    includes.append(line.strip())
                continue
            kept.append(line)
        bodies.append(collapse_blank_lines('\n'.join(kept)))

    parts = [banner, '']
    for license_text in licenses:
        parts.append(license_text)
        parts.append('')
    parts.extend(includes)
    parts.append('')
    parts.extend(bodies)
    return '\n'.join(parts)


if __name__ == '__main__':
    output, banner_text = sys.argv[1], sys.argv[2]
    with open(output, 'w') as handle:
        handle.write(merge(sys.argv[3:], banner_text))
