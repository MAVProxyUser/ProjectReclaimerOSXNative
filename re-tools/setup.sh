#!/bin/bash
# Recreate the Rust/PE decompilation environment (angr). Scratchpad venvs are ephemeral;
# this makes a stable one at re-tools/angrenv. Run once per machine; re-runs are fast (uv cache).
set -e
cd "$(dirname "$0")"

# uv (standalone python/pkg manager) + a rust toolchain (rustup) if missing.
command -v uv >/dev/null 2>&1 || curl -LsSf https://astral.sh/uv/install.sh | sh
export PATH="$HOME/.local/bin:$PATH"

# IMPORTANT: use PyPI angr (prebuilt wheels). Do NOT `uv sync` the oxidizer repo — its pinned
# pyvex builds VEX from source and fails on this macOS (pycparser "Unmatched '}'"). The PyPI
# wheels avoid the native build entirely.
uv venv --python 3.12 angrenv
source angrenv/bin/activate
# bitstring>=5 removed ConstBitStream which pyvex imports at module load -> pin 4.1.4.
uv pip install 'angr==10.0.3' 'bitstring==4.1.4' 'capstone==5.0.9'
python -c "import angr; print('angr', angr.__version__, 'ready')"
echo "OK. Decompile with:  source angrenv/bin/activate && python decomp.py <bin> <vma> [rstart] [rend]"
