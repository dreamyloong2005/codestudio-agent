"""Run the delivery snapshot regression class without package installation."""
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
root=Path(__file__).resolve().parents[2]
path=root/'tests/delivery_integration.py'
spec=importlib.util.spec_from_file_location('delivery_snapshot_regression',path)
module=importlib.util.module_from_spec(spec)
with patch.object(sys,'argv',[str(path),'--standalone','--package',str(root)]):
    spec.loader.exec_module(module)
suite=unittest.defaultTestLoader.loadTestsFromTestCase(module.PackageSnapshotTests)
result=unittest.TextTestRunner(verbosity=2).run(suite)
raise SystemExit(0 if result.wasSuccessful() else 1)
