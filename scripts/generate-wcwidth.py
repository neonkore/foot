#!/usr/bin/env python3

import argparse
import sys

from typing import List, Tuple


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('derived', type=argparse.FileType('r'), help='path to DerivedGeneralCategory.txt')
    parser.add_argument('east_asian', type=argparse.FileType('r'), help='path to EastEasianWidth.txt')
    parser.add_argument('output', type=argparse.FileType('w'), help='output, C header file')

    opts = parser.parse_args()

    invalid, zero_width = parse_derived(opts.derived)
    double_width = parse_east_asian(opts.east_asian)

    output = opts.output
    output.write('#pragma once\n')
    output.write('#include <stdint.h>\n')
    output.write('\n')
    output.write('struct ucs_range {\n')
    output.write('    uint32_t start;\n')
    output.write('    uint32_t stop;\n')
    output.write('};\n')
    output.write('\n')

    output.write('static const struct ucs_range ucs_invalid[] = {\n')
    for i, (start, stop) in enumerate(invalid):
        if i % 3 == 0:
            output.write('    ')
        output.write(f'{{0x{start:05x}, 0x{stop:05x}}}')
        if i + 1 < len(invalid):
            output.write(',')
        if i % 3 == 2:
            output.write('\n')
        else:
            output.write(' ')
    if len(invalid) % 3 != 0:
        output.write('\n')
    output.write('};\n')
    output.write('\n')

    output.write('static const struct ucs_range ucs_zero_width[] = {\n')
    for i, (start, stop) in enumerate(zero_width):
        if i % 3 == 0:
            output.write('    ')
        output.write(f'{{0x{start:05x}, 0x{stop:05x}}}')
        if i + 1 < len(zero_width):
            output.write(',')
        if i % 3 == 2:
            output.write('\n')
        else:
            output.write(' ')
    if len(zero_width) % 3 != 0:
        output.write('\n')
    output.write('};\n')
    output.write('\n')

    output.write('static const struct ucs_range ucs_double_width[] = {\n')
    for i, (start, stop) in enumerate(double_width):
        if i % 3 == 0:
            output.write('    ')
        output.write(f'{{0x{start:05x}, 0x{stop:05x}}}')
        if i + 1 < len(double_width):
            output.write(',')
        if i % 3 == 2:
            output.write('\n')
        else:
            output.write(' ')
    if len(double_width) % 3 != 0:
        output.write('\n')
    output.write('};\n')


def parse_derived(f) -> List[Tuple[int, int]]:
    """Returns a list of (start, stop) tuples of zero-width codepoints."""

    zero_width = []
    invalid = []

    for line in f.readlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith('#'):
            continue

        ucs, details = line.split(';', maxsplit=1)
        ucs, details = ucs.rstrip(), details.lstrip()

        if '..' in ucs:
            start, stop = ucs.split('..')
        else:
            start, stop = ucs, ucs

        start = int(start, 16)
        stop = int(stop, 16)

        details = details.split('#', maxsplit=1)[0].strip()

        # Me: Mark, enclosing, Mn: Mark, nonspacing, Cf: ???
        if details in ['Me', 'Mn', 'Cf']:
            zero_width.append((start, stop))

        # Cn: unassigned
        if details == 'Cn':
            invalid.append((start, stop))

    zero_width = sorted(zero_width)
    invalid = sorted(invalid)

    # Merge consecutive invalid ranges
    merged_invalid = [invalid[0]]
    for start, stop in invalid[1:]:
        if merged_invalid[-1][1] + 1 == start:
            merged_invalid[-1] = merged_invalid[-1][0], stop
        else:
            merged_invalid.append((start, stop))

    # Merge consecutive zero-width ranges
    merged_zero_width = [zero_width[0]]
    for start, stop in zero_width[1:]:
        if merged_zero_width[-1][1] + 1 == start:
            merged_zero_width[-1] = merged_zero_width[-1][0], stop
        else:
            merged_zero_width.append((start, stop))

    return merged_invalid, merged_zero_width


def parse_east_asian(f) -> List[Tuple[int, int]]:
    """Returns a list of (start, stop) tuples of double-width codepoints."""

    ranges = []
    for line in f.readlines():
        line = line.strip()
        if not line:
            continue
        if line.startswith('#'):
            continue

        ucs, details = line.split(';', maxsplit=1)
        ucs, details = ucs.rstrip(), details.lstrip()

        if '..' in ucs:
            start, stop = ucs.split('..')
        else:
            start, stop = ucs, ucs

        start = int(start, 16)
        stop = int(stop, 16)

        details = details.split('#', maxsplit=1)[0].strip()

        if details in 'WF':
            ranges.append((start, stop))

    # Merge consecutive ranges
    merged = [ranges[0]]
    for start, stop in ranges[1:]:
        if merged[-1][1] + 1 == start:
            merged[-1] = merged[-1][0], stop
        else:
            merged.append((start, stop))

    return merged

if __name__ == '__main__':
    sys.exit(main())
