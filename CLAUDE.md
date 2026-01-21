## About this project
You're in my fork of qutebrowser's source.

## Testing Environment
The `.venv` directory contains a Python virtual environment with PyQt6 and test dependencies. To run tests:

```bash
source .venv/bin/activate
QT_QPA_PLATFORM=offscreen PYTHONPATH=. pytest tests/unit/path/to/test.py -v
```

The `QT_QPA_PLATFORM=offscreen` prevents Qt windows from appearing during tests.

Available packages: PyQt6, PyQt6-WebEngine, pytest, pytest-qt, pytest-mock, hypothesis, and other pytest plugins.
