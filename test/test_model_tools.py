"""Optional nightly tooling tests; Python is not required by make check."""
import importlib.util
from pathlib import Path
import tempfile
import unittest


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parents[1] / 'tools' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class ModelTools(unittest.TestCase):
    def report(self):
        return '\n'.join(['model_source=GGUF',
                          'model_sha256=4321e21b9da533f40386aa5ab968cced6196e21ecbf395ae04e5bce1ee88e767',
                          'corpus=scifact-256-100-v1 documents=256 dimension=1024 window=256 omit_bos=1 omit_eos=0',
                          'query_prefix=query: '] + [f'query={i} language=en relevant=0 float_rank=1 binary_rank=1' for i in range(100)])

    def test_complete_report(self):
        self.assertTrue(module('check-model-quality').check(self.report()).startswith('PASS:'))

    def test_rejections(self):
        check = module('check-model-quality').check
        report = self.report()
        for bad in [report.replace('GGUF', 'mock'), report.replace('query=99', 'query=98'),
                    report.replace('binary_rank=1', 'binary_rank=256'),
                    report.replace('float_rank=1', 'float_rank=0'),
                    report.replace('float_rank=1', 'float_rank=NaN'),
                    report.replace('query_prefix=query: ', 'query_prefix='),
                    report.replace('documents=256', 'documents=512')]:
            with self.subTest(bad=bad[:40]), self.assertRaises(ValueError):
                check(bad)

    def test_unknown_source_preserves_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, output = Path(tmp)/'input.zip', Path(tmp)/'fixture.h'
            source.write_bytes(b'not the pinned corpus')
            output.write_bytes(b'existing')
            with self.assertRaises(ValueError):
                module('prepare-quality').prepare(source, output)
            self.assertEqual(output.read_bytes(), b'existing')


if __name__ == '__main__':
    unittest.main()
