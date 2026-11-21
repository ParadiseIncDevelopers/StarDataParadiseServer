"""Launch the installed, user-bound worker from MySQL Shell Python mode."""
from pathlib import Path
import sys
import os

def normalized(path):
    return os.path.normcase(str(Path(path or '.').resolve())).removeprefix('\\\\?\\')

# mysqlsh adds the entry's directory to sys.path. It contains our secrets.py,
# which must not shadow Python's unrelated standard-library secrets module.
entry_directory = normalized(Path(__file__).parent)
sys.path[:] = [value for value in sys.path if normalized(value) != entry_directory]
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from compatibility.ai.local_service import main

main(sys.argv[1])
