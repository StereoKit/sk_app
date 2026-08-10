#!/usr/bin/env python3
"""Drops unused interfaces and trailing requests from a protocol XML.

Opcodes are positions in the generated message table, so removing a request
renumbers every later one and silently corrupts the wire protocol. Removing the
*last* request is the exception: nothing follows it to renumber. This drops
whole interfaces, and trailing requests when named as `interface.request`,
refusing anything that is not actually last.

It also refuses to drop an interface another surviving interface still refers
to, which is what makes the pruning list safe to edit.

Usage: wl_prune.py in.xml out.xml [interface | interface.request ...]
"""

import sys
import xml.etree.ElementTree as ET


def prune(in_path, out_path, drop):
    tree = ET.parse(in_path)
    root = tree.getroot()

    drop_interfaces = [name for name in drop if '.' not in name]
    drop_requests   = [name for name in drop if '.' in name]

    interfaces = {i.get('name'): i for i in root.findall('interface')}
    unknown = [name for name in drop_interfaces if name not in interfaces]
    if unknown:
        raise SystemExit('wl_prune: no such interface: %s' % ', '.join(unknown))

    # Requests go first: dropping one is what lets the interface it referenced
    # be dropped in turn.
    for entry in drop_requests:
        iface_name, request_name = entry.split('.', 1)
        iface = interfaces.get(iface_name)
        if iface is None:
            raise SystemExit('wl_prune: no such interface: %s' % iface_name)
        requests = iface.findall('request')
        names    = [r.get('name') for r in requests]
        if request_name not in names:
            raise SystemExit('wl_prune: no such request: %s' % entry)
        if names[-1] != request_name:
            raise SystemExit('wl_prune: %s is not the last request (%s follows), '
                             'dropping it would renumber opcodes' % (entry, names[names.index(request_name) + 1]))
        iface.remove(requests[-1])

    keep = [name for name in interfaces if name not in drop_interfaces]

    # An arg naming a dropped interface would leave the generated table with a
    # dangling reference, so that is an error rather than something to patch up.
    broken = []
    for name in keep:
        for arg in interfaces[name].iter('arg'):
            target = arg.get('interface')
            if target in drop_interfaces:
                broken.append('%s references %s' % (name, target))
    if broken:
        raise SystemExit('wl_prune: cannot drop, still referenced:\n  ' + '\n  '.join(broken))

    for name in drop_interfaces:
        root.remove(interfaces[name])
    tree.write(out_path, encoding='unicode', xml_declaration=True)
    return len(drop_interfaces), len(drop_requests), len(keep)


if __name__ == '__main__':
    dropped, requests, kept = prune(sys.argv[1], sys.argv[2], sys.argv[3:])
    print('    pruned %d interfaces and %d requests, kept %d' % (dropped, requests, kept))
