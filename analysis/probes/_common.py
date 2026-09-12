"""Shared entry-point boilerplate for the probe scripts."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def report(label, messages, slides=0):
    if not messages:
        print("  %-30s -> (no response)" % label)
        return
    for message in messages:
        suffix = "  resync=%d" % slides if slides else ""
        print("  %-30s -> %s%s" % (label, message, suffix))
