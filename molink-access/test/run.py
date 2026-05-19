"""MoLink test runner. Usage: python test/run.py"""
import sys
import os
import pytest

if __name__ == "__main__":
    test_dir = os.path.dirname(os.path.abspath(__file__))
    sys.exit(pytest.main([test_dir, "-v", "-s", "--tb=short"]))
