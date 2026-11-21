"""The native DD boundary must cover every installed managed AI routine."""
from pathlib import Path
import os
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
CATALOG = Path(os.environ.get('SDP_AI_MANIFEST_DIRECTORY', ROOT/'compatibility/ai'))
HEADER = Path(os.environ.get('SDP_AI_MANIFEST_HEADER', ROOT/'compatibility/ai/native/sdp_ai_catalog_guard.h'))
ROUTINE = re.compile(r"CREATE\s+(?:DEFINER\s*=\s*'([^']+)'\s*@\s*'([^']+)'\s+)?(FUNCTION|PROCEDURE)\s+sdp_ai\.([a-z_]+)\s*\(", re.I)


def routines(source):
    found = ROUTINE.findall(source)
    return {kind.upper(): {name.lower() for user, host, actual, name in found if actual.upper() == kind.upper()}
            for kind in ('FUNCTION', 'PROCEDURE')}


def manifest(source, name):
    match = re.search(r'\b' + re.escape(name) + r'\[\]\s*=\s*\{([^}]+)\}', source)
    if not match:
        raise AssertionError('Missing native manifest ' + name)
    values = re.findall(r'"([a-z_]+)"', match.group(1))
    if len(values) != len(set(values)):
        raise AssertionError('Duplicate native manifest member in ' + name)
    return set(values)


class CatalogManifestTests(unittest.TestCase):
    def test_core_manifest_matches_managed_sql_exactly(self):
        actual = routines((CATALOG/'catalog.sql').read_text(encoding='utf-8'))
        header = HEADER.read_text(encoding='utf-8')
        self.assertEqual(actual['FUNCTION'], manifest(header, 'functions'))
        self.assertEqual(actual['PROCEDURE'], manifest(header, 'procedures'))

    def test_optional_tool_manifest_covers_new_routines_and_only_explicit_overrides(self):
        actual = routines((CATALOG/'tool_catalog.sql').read_text(encoding='utf-8'))
        header = HEADER.read_text(encoding='utf-8')
        # Tool publication replaces claim_run to add the durable continuation
        # phase. All other optional routines need their own pinned entry.
        self.assertEqual(actual['FUNCTION'], set())
        self.assertEqual(actual['PROCEDURE'] & manifest(header, 'procedures'), {'claim_run'})
        self.assertEqual(actual['PROCEDURE'] - {'claim_run'}, manifest(header, 'tool_procedures'))

    def test_security_mode_matches_every_pinned_routine(self):
        header = HEADER.read_text(encoding='utf-8')
        expected_invokers = manifest(header, 'invoker_functions')
        actual_invokers = set()
        for file in ('catalog.sql', 'tool_catalog.sql', 'action_catalog.sql'):
            if not (CATALOG/file).exists():
                continue
            source = (CATALOG/file).read_text(encoding='utf-8')
            for match in ROUTINE.finditer(source):
                self.assertEqual((match[1],match[2]), ('sdp_ai_internal','localhost'), 'Unbounded or implicit routine DEFINER: '+match[4])
                body = source[match.end():source.index('$$', match.end())]
                security = re.search(r'\bSQL\s+SECURITY\s+(DEFINER|INVOKER)\b', body, re.I)
                self.assertIsNotNone(security, 'Implicit security mode: ' + match[4])
                if security[1].upper() == 'INVOKER':
                    self.assertEqual(match[3].upper(), 'FUNCTION', 'Unspecified procedure INVOKER policy')
                    actual_invokers.add(match[4].lower())
        self.assertEqual(actual_invokers, expected_invokers)

    def test_optional_action_manifest_is_exact(self):
        path=CATALOG/'action_catalog.sql'
        if not path.exists():
            self.skipTest('Action module is not in this catalog profile.')
        actual=routines(path.read_text(encoding='utf-8'))
        header=HEADER.read_text(encoding='utf-8')
        self.assertEqual(actual['FUNCTION'],set())
        self.assertEqual(actual['PROCEDURE'],manifest(header,'action_procedures'))


if __name__ == '__main__':
    unittest.main()
